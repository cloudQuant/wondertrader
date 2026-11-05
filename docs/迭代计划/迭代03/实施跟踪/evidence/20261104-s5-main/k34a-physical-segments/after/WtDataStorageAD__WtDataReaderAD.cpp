#include "WtDataReaderAD.h"
#include "LMDBKeys.h"
#include "DayBarContractAD.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/DateMappingClient.hpp"

#include <boost/filesystem.hpp>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
namespace fs = boost::filesystem;

namespace {
thread_local LMDBRangeCursorGetOverride tick_query_cursor_get_override = NULL;
thread_local void* tick_query_cursor_get_context = NULL;

// R122-002 test hooks (null/zero in production): bar-update chunk size and an
// observer of every staging buffer size, used to prove the staging bound.
thread_local std::size_t bar_update_chunk_override = 0;
thread_local void (*staging_size_observer)(void*, std::size_t) = NULL;
thread_local void* staging_size_observer_context = NULL;

// Largest number of bars a forward K-line update stages at once.
constexpr std::size_t kBarUpdateChunk = 4096;

void observe_staging(std::size_t staged)
{
	if (staging_size_observer != NULL)
		staging_size_observer(staging_size_observer_context, staged);
}

struct ReadTxnGuard
{
	MDB_txn* txn = NULL;
	~ReadTxnGuard() { if (txn != NULL) mdb_txn_abort(txn); }
};

struct CursorGuard
{
	MDB_cursor* cursor = NULL;
	~CursorGuard() { if (cursor != NULL) mdb_cursor_close(cursor); }
};

enum : int { kStreamLmdbFailure = -1, kStreamRejected = -2 };

// R122-002: walk [lower, upper] (lower optionally exclusive) in key order inside
// one read-only transaction, handing each record to visit() without staging the
// range. visit returns false to reject the scan. At most `limit` records are
// visited when limit != 0 (more is then set if another in-range record exists).
// Returns the visited count, kStreamLmdbFailure (error kept in db) or
// kStreamRejected. The transaction is closed before returning, so callers may
// run callbacks that re-enter the reader afterwards.
template<typename Visitor>
int stream_checked_range(WtLMDB& db, const std::string& lower, bool lowerExclusive,
	const std::string& upper, std::size_t limit, bool honorTestOverride, Visitor&& visit, bool& more)
{
	more = false;
	ReadTxnGuard txn;
	int status = mdb_txn_begin(db.env(), NULL, MDB_RDONLY, &txn.txn);
	if (status != MDB_SUCCESS)
	{
		txn.txn = NULL;
		db.update_errno(status);
		return kStreamLmdbFailure;
	}
	const MDB_dbi dbi = db.update_dbi(txn.txn);
	if (dbi == 0)
		return kStreamLmdbFailure;

	CursorGuard cursor;
	status = mdb_cursor_open(txn.txn, dbi, &cursor.cursor);
	if (status != MDB_SUCCESS)
	{
		cursor.cursor = NULL;
		db.update_errno(status);
		return kStreamLmdbFailure;
	}
	const LMDBRangeCursorGetOverride override_get = honorTestOverride ? tick_query_cursor_get_override : NULL;
	auto cursor_get = [&](MDB_val* key, MDB_val* value, MDB_cursor_op op) {
		return override_get == NULL
			? mdb_cursor_get(cursor.cursor, key, value, op)
			: override_get(tick_query_cursor_get_context, cursor.cursor, key, value, op);
	};

	MDB_val key = { lower.size(), (void*)lower.data() };
	MDB_val value = { 0, NULL };
	MDB_cursor_op op = MDB_SET_RANGE;
	int visited = 0;
	for (;;)
	{
		status = cursor_get(&key, &value, op);
		op = MDB_NEXT;
		if (status == MDB_NOTFOUND)
		{
			status = MDB_SUCCESS;
			break;
		}
		if (status != MDB_SUCCESS)
			break;
		if (compare_lmdb_keys(key.mv_data, key.mv_size, upper.data(), upper.size()) > 0)
			break;
		if (lowerExclusive && compare_lmdb_keys(key.mv_data, key.mv_size, lower.data(), lower.size()) == 0)
			continue;
		if (limit != 0 && (std::size_t)visited >= limit)
		{
			more = true;
			break;
		}
		if (visited == (std::numeric_limits<int>::max)() || !visit(key, value))
			return kStreamRejected;
		++visited;
	}
	if (status != MDB_SUCCESS)
	{
		db.update_errno(status);
		return kStreamLmdbFailure;
	}
	db.update_errno(MDB_SUCCESS);
	return visited;
}
}

namespace wtp {

struct WtLMDBReaderTestAccess
{
	static void set_cursor_get_override(WtLMDBQuery& query, LMDBRangeCursorGetOverride callback, void* context)
	{
		query._range_cursor_get_override = callback;
		query._range_cursor_get_context = context;
	}
};

} // namespace wtp

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
#include <vector>
template<typename... Args>
inline void pipe_reader_log(IDataReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	// Messages can carry caller-supplied codes and configured paths; bound the
	// formatter writes to the per-thread buffer instead of trusting their size.
	static thread_local char buffer[512] = { 0 };
	try
	{
		const std::size_t max_output_size = sizeof(buffer) - 1;
		const auto formatted = fmt::format_to_n(buffer, max_output_size, format, args...);
		if (formatted.size > max_output_size)
		{
			constexpr char truncation_marker[] = "...[truncated]";
			constexpr std::size_t marker_size = sizeof(truncation_marker) - 1;
			std::memcpy(buffer + max_output_size - marker_size, truncation_marker, marker_size);
			buffer[max_output_size] = '\0';
		}
		else
		{
			buffer[formatted.size] = '\0';
		}
	}
	catch (...)
	{
		constexpr char fallback[] = "WtDataReaderAD log formatting failed";
		std::memcpy(buffer, fallback, sizeof(fallback));
	}

	sink->reader_log(ll, buffer);
}

// Decision card 1 (A+C): trading dates come from the typed client over the
// guarded legacy IBaseDataMgr virtuals; a failure stops the read.
namespace {

// K34=A: section reads start this many minutes before the session-open boundary so
// that call-auction prints are kept; records are then assigned by their trading date.
const int32_t kRuleSectionLeadMinutes = 60;

// Splits a natural YYYYMMDDHHMM boundary shifted by `minutes` into date and HHMM.
bool rule_boundary_parts(uint64_t value, int32_t minutes, uint32_t& date, uint32_t& hhmm)
{
	if (value / 10000 > 99991231ull)
		return false;
	date = (uint32_t)(value / 10000);
	const uint32_t raw = (uint32_t)(value % 10000);
	const uint32_t month = (date / 100) % 100;
	const uint32_t day = date % 100;
	if (date < 19900101u || month < 1 || month > 12 || day < 1 || day > 31 || raw / 100 > 23 || raw % 100 > 59)
		return false;
	int32_t mins = (int32_t)(raw / 100) * 60 + (int32_t)(raw % 100) + minutes;
	while (mins < 0)
	{
		mins += 1440;
		date = TimeUtils::getNextDate(date, -1);
	}
	while (mins >= 1440)
	{
		mins -= 1440;
		date = TimeUtils::getNextDate(date, 1);
	}
	hhmm = (uint32_t)(mins / 60) * 100 + (uint32_t)(mins % 60);
	return date >= 19900000u;
}

// Minute bar key (date-19900000)*10000+HHMM; false when the V1 32-bit key cannot hold it.
bool rule_minute_key(uint32_t date, uint32_t hhmm, uint32_t& key)
{
	if (date < 19900000u)
		return false;
	const uint64_t value = (uint64_t)(date - 19900000u) * 10000u + hhmm;
	if (value > UINT32_MAX)
		return false;
	key = (uint32_t)value;
	return true;
}

void rule_scale_prices(WTSBarStruct& bar, double factor)
{
	bar.open *= factor;
	bar.high *= factor;
	bar.low *= factor;
	bar.close *= factor;
}

} // namespace

static bool ad_reader_trading_date(IDataReaderSink* sink, const wtp::DateMappingBinding& binding,
	IBaseDataMgr* mgr, const char* stdPID, uint32_t uDate, uint32_t uTime, uint32_t& tradingDate)
{
	wtp::DateMappingClient mapping = binding.client(mgr);
	const wtp::DateMappingResult32 result = mapping.calcTradingDate(stdPID, uDate, uTime, false);
	if (result.status != wtp::DateMappingStatus::Ok)
	{
		pipe_reader_log(sink, LL_ERROR, "Mapping trading date of {} at {}.{} failed: {}",
			stdPID == NULL ? "" : stdPID, uDate, uTime, wtp::dateMappingStatusName(result.status));
		return false;
	}
	tradingDate = result.value;
	return true;
}

namespace {

// Display at most the bytes the key check inspects; the field may be unterminated.
std::string key_field_for_log(const char* field, std::size_t capacity)
{
	if (field == NULL)
		return "(null)";
	std::size_t length = 0;
	while (length < capacity && field[length] != '\0')
		++length;
	std::string text(field, length);
	if (length == capacity)
		text += "...";
	return text;
}

// BUG-L04-004: an identity that cannot be represented by the fixed LMDB key is
// rejected before cache, directory, database or callback side effects, and the
// rejection is observable instead of a silent NULL.
bool accept_key_identity(IDataReaderSink* sink, const char* exchg, const char* code, const char* entry)
{
	if (lmdbKeyIdentityValid(exchg, code))
		return true;
	pipe_reader_log(sink, LL_ERROR,
		"{} rejected storage identity {}.{}: exchange and code must be non-empty and shorter than {} and {} bytes",
		entry, key_field_for_log(exchg, MAX_EXCHANGE_LENGTH),
		key_field_for_log(code, MAX_INSTRUMENT_LENGTH), MAX_EXCHANGE_LENGTH, MAX_INSTRUMENT_LENGTH);
	return false;
}

} // namespace

extern "C"
{
	EXPORT_FLAG IDataReader* createDataReader()
	{
		IDataReader* ret = new WtDataReaderAD();
		return ret;
	}

	EXPORT_FLAG void deleteDataReader(IDataReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}

	// K31=A: optional DateMappingV1 injection (see Includes/DateMappingBinding.h).
	EXPORT_FLAG int bindDataReaderDateMappingV1(IDataReader* reader, const wtp::DateMappingV1* table)
	{
		if (reader == NULL)
			return 0;
		return static_cast<WtDataReaderAD*>(reader)->bindDateMapping(table) ? 1 : 0;
	}
};


WtDataReaderAD::WtDataReaderAD()
	: _base_data_mgr(NULL)
	, _hot_mgr(NULL)
, _last_time(0)
	{
}


WtDataReaderAD::~WtDataReaderAD()
{
}

void WtDataReaderAD::set_test_cursor_get_override(LMDBRangeCursorGetOverride callback, void* context)
{
	tick_query_cursor_get_override = callback;
	tick_query_cursor_get_context = context;
}

void WtDataReaderAD::set_test_staging_hooks(std::size_t barChunk, void (*observer)(void*, std::size_t), void* context)
{
	bar_update_chunk_override = barChunk;
	staging_size_observer = observer;
	staging_size_observer_context = context;
}

void WtDataReaderAD::init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader /* = NULL */)
{
	IDataReader::init(cfg, sink, loader);

	_base_data_mgr = sink->get_basedata_mgr();
	_hot_mgr = sink->get_hot_mgr();

	if (cfg == NULL)
		return ;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	_d1_cache._filename = "cache_d1.dmb";
	_m1_cache._filename = "cache_m1.dmb";
	_m5_cache._filename = "cache_m5.dmb";

	pipe_reader_log(sink, LL_INFO, "WtDataReaderAD initialized, root data folder is {}", _base_dir);
}

WTSTickSlice* WtDataReaderAD::readTickSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readTickSlice"))
		return NULL;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	uint32_t endTDate = 0;
	if (!ad_reader_trading_date(_sink, _date_binding, _base_data_mgr, stdPID.c_str(), curDate, curTime, endTDate))
		return NULL;

	// Decision K34=A (2026-11-04, after C): a HOT/custom rule code is served only from
	// the physical contracts of its rule sections, each limited to its own trading dates.
	// Pre-merged product history and the currently mapped raw contract are never used.
	if (cInfo._ruletag[0] != '\0')
		return readRuleTickSlice(stdCode, cInfo._exchg, cInfo._ruletag, cInfo.stdCommID(), count, etime, endTDate);
	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
		{
			const char* rawCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), endTDate);
			if (!accept_key_identity(_sink, cInfo._exchg, rawCode, "readTickSlice(custom raw code)"))
				return NULL;
			curCode = rawCode;
		}
		//else if (cInfo.isHot())
		//	curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
		//else if (cInfo.isSecond())
		//	curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);
	}

	uint32_t reload_flag = 0; //重载标记，0-不需要重载，1-加载最新，2-全部重载
	std::string key = fmtutil::format<64>("{}.{}", cInfo._exchg, curCode.c_str());
	//先检查缓存
	TicksList& tickList = _ticks_cache[key];
	uint64_t last_access_time = 0;
	if (tickList._ticks.capacity() < count)
	{
		//容量不够，必须限量全量重载；等查询完整成功后再替换缓存。
		reload_flag = 2;
	}
	else if (tickList._last_req_time < etime)
	{
		//容量足够但请求时间向前推进，只拉取新增区间。
		reload_flag = 1;
		last_access_time = tickList._last_req_time;
	}

	//这里要改成从lmdb读取
	WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
	if (db == NULL)
		return NULL;

	if (reload_flag != 0)
	{
		if (reload_flag == 2 && count > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
		{
			pipe_reader_log(_sink, LL_ERROR, "Tick request count {} for {} exceeds the supported limit", count, stdCode);
			return NULL;
		}

		std::vector<WTSTickStruct> loaded_ticks;
		bool values_valid = true;
		auto collect_ticks = [&loaded_ticks, &values_valid](const ValueArray& ayKeys, const ValueArray& ayVals) {
			if (ayKeys.size() != ayVals.size())
			{
				values_valid = false;
				return;
			}
			for (const std::string& item : ayVals)
			{
				if (item.size() != sizeof(WTSTickStruct))
				{
					values_valid = false;
					return;
				}
				WTSTickStruct curTick;
				memcpy(&curTick, item.data(), sizeof(curTick));
				loaded_ticks.push_back(curTick);
			}
		};

		int cnt = -1;
		if (reload_flag == 1)
		{
			//增量更新，只在完整扫描成功后追加到缓存。
			last_access_time += 1;
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, (uint32_t)(last_access_time / 1000000000), (uint32_t)(last_access_time % 1000000000));
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000));
			// R122-002: the cache ring keeps only its capacity, so stage at most
			// that many of the newest rows while still validating the whole range
			// inside one snapshot; a long gap no longer grows memory linearly.
			boost::circular_buffer<WTSTickStruct> newest(tickList._ticks.capacity());
			bool more = false;
			cnt = stream_checked_range(*db, std::string((const char*)&lKey, sizeof(lKey)), false,
				std::string((const char*)&rKey, sizeof(rKey)), 0, true,
				[&newest, &values_valid](const MDB_val&, const MDB_val& item) {
					if (item.mv_size != sizeof(WTSTickStruct))
					{
						values_valid = false;
						return false;
					}
					WTSTickStruct curTick;
					memcpy(&curTick, item.mv_data, sizeof(curTick));
					newest.push_back(curTick);
					return true;
				}, more);
			if (cnt == kStreamRejected)
			{
				values_valid = false;
				cnt = 0;
			}
			observe_staging(newest.size());
			loaded_ticks.assign(newest.begin(), newest.end());
		}
		else
		{
			//首次/扩容全量重载，从结束时间向前限量读取。
			// The query owns a read transaction; keep it scoped to this branch so the
			// streaming scan above never nests a second read transaction in this thread.
			WtLMDBQuery query(*db);
			WtLMDBReaderTestAccess::set_cursor_get_override(query,
				tick_query_cursor_get_override, tick_query_cursor_get_context);
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000));
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, 0, 0);
			cnt = query.get_lowers_checked(std::string((const char*)&lKey, sizeof(lKey)),
				std::string((const char*)&rKey, sizeof(rKey)), static_cast<int>(count), collect_ticks);
		}

		if (cnt < 0)
		{
			pipe_reader_log(_sink, LL_ERROR, "Failed to load ticks of {}: {}", stdCode, db->errmsg());
			return NULL;
		}
		if (!values_valid)
		{
			pipe_reader_log(_sink, LL_ERROR, "Tick query for {} returned malformed values", stdCode);
			return NULL;
		}

		if (reload_flag == 2)
		{
			tickList._ticks.rset_capacity(count);
			tickList._ticks.clear();
		}
		for (const WTSTickStruct& tick : loaded_ticks)
			tickList._ticks.push_back(tick);
		tickList._last_req_time = etime;

		if (reload_flag == 1)
		{
			if (cnt > 0)
				pipe_reader_log(_sink, LL_DEBUG, "{} ticks after {} of {} append to cache", cnt, last_access_time, stdCode);
		}
		else
		{
			pipe_reader_log(_sink, LL_DEBUG, "{} ticks of {} loaded to cache for the first time", cnt, stdCode);
		}
	}

	//全部读取完成以后，再生成切片
	//这里要注意一下，因为读取tick是顺序的，但是返回是需要从后往前取指定条数的，所以应该先读取array_two
	count = min((uint32_t)tickList._ticks.size(), count);	//先对count做一个修正
	auto ayTwo = tickList._ticks.array_two();
	auto cnt_2 = ayTwo.second;
	if (count == 0)
	{
		auto ayOne = tickList._ticks.array_one();
		return WTSTickSlice::create(stdCode, ayOne.first, 0);
	}
	if(cnt_2 >= count)
	{
		//array_two 是物理连续段；operator[] 使用逻辑下标，回绕时不能代替段内偏移。
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, ayTwo.first + (ayTwo.second - count), count);
		if (slice != NULL)
			slice->addOwner(tickList._ticks.owner());
		return slice;
	}
	else
	{
		//如果array_two条数不够，需要再从array_one提取
		auto ayOne = tickList._ticks.array_one();
		auto diff = count - cnt_2;
		auto ret = WTSTickSlice::create(stdCode, ayOne.first + (ayOne.second - diff), diff);
		if (ret != NULL)
			ret->addOwner(tickList._ticks.owner());
		if(cnt_2 > 0)
			ret->appendBlock(ayTwo.first, cnt_2);
		return ret;
	}
}

std::string WtDataReaderAD::read_bars_to_buffer(const char* exchg, const char* code, WTSKlinePeriod period)
{
	std::string buffer;
	(void)read_bars_to_buffer(exchg, code, period, buffer);
	return buffer;
}

bool WtDataReaderAD::read_bars_to_buffer(const char* exchg, const char* code,
	WTSKlinePeriod period, std::string& buffer)
{
	if (!accept_key_identity(_sink, exchg, code, "read_bars_to_buffer"))
		return false;

	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;

	std::string pending;
	bool valuesValid = true;
	WtLMDBQuery query(*db);
	WtLMDBReaderTestAccess::set_cursor_get_override(query,
		tick_query_cursor_get_override, tick_query_cursor_get_context);
	LMDBBarKey lKey(exchg, code, 0);
	LMDBBarKey rKey(exchg, code, UINT32_MAX);
	const int count = query.get_range_checked(std::string((const char*)&lKey, sizeof(lKey)),
		std::string((const char*)&rKey, sizeof(rKey)),
		[&pending, &valuesValid](const ValueArray& keys, const ValueArray& values) {
			if (keys.size() != values.size())
			{
				valuesValid = false;
				return;
			}
			for (std::size_t i = 0; i < values.size(); ++i)
			{
				if (keys[i].size() != sizeof(LMDBBarKey) || values[i].size() != sizeof(WTSBarStruct))
				{
					valuesValid = false;
					return;
				}
				pending.append(values[i]);
			}
		});
	if (count < 0 || !valuesValid)
		return false;
	buffer.swap(pending);
	return true;
}

bool WtDataReaderAD::cacheBarsFromStorage(const std::string& key, const char* stdCode,
	WTSKlinePeriod period, uint32_t count, uint32_t cutoff)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "cacheBarsFromStorage"))
		return false;
	[[maybe_unused]] WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (count > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Kline request count {} for {} exceeds the supported limit", count, stdCode);
		return false;
	}

	//直接从LMDB读取
	WtLMDBPtr db = get_k_db(cInfo._exchg, period);
	if (db == NULL)
		return false;

	WtLMDBQuery query(*db);
	// Bar keys are period-specific and ordered by their encoded time. Bound the
	// LMDB query at the inclusive cutoff before selecting the latest count bars.
	LMDBBarKey rKey(cInfo._exchg, cInfo._code, cutoff);
	LMDBBarKey lKey(cInfo._exchg, cInfo._code, 0);
	std::vector<WTSBarStruct> loadedBars;
	bool valuesValid = true;
	int cnt = query.get_lowers_checked(std::string((const char*)&lKey, sizeof(lKey)),
		std::string((const char*)&rKey, sizeof(rKey)), static_cast<int>(count),
		[&loadedBars, &valuesValid, &lKey, period, cutoff](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayKeys.size() != ayVals.size())
		{
			valuesValid = false;
			return;
		}

		for (std::size_t i = 0; i < ayVals.size(); i++)
		{
			if (ayKeys[i].size() != sizeof(LMDBBarKey) || ayVals[i].size() != sizeof(WTSBarStruct))
			{
				valuesValid = false;
				return;
			}
		}

		for (std::size_t i = 0; i < ayVals.size(); i++)
		{
			//要检查左边界
			if(memcmp(ayKeys[i].data(), (void*)&lKey, sizeof(lKey)) < 0)
				continue;

			WTSBarStruct bar;
			memcpy(&bar, ayVals[i].data(), sizeof(bar));
			const uint64_t barTime = period == KP_DAY ? bar.date : bar.time;
			if (barTime > cutoff)
				continue;
			if (period == KP_DAY)
				wt_ad_daybar::normalize(bar);	//卡3 3c=C：旧 day 记录 time=交易日，读端归一为 0
			loadedBars.push_back(bar);
		}
	});
	if (cnt < 0 || !valuesValid)
	{
		pipe_reader_log(_sink, LL_ERROR, "Malformed bar key/value in LMDB while loading {} {} bars",
			PERIOD_NAME[period], stdCode);
		return false;
	}

	// Only publish the new cache after every key and value has passed validation.
	BarsList& barList = _bars_cache[key];
	barList._bars.rset_capacity(count);
	barList._bars.clear();
	for (const WTSBarStruct& bar : loadedBars)
		barList._bars.push_back(bar);
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo._exchg;
	barList._last_from_cache = false;
	barList._last_req_time = 0;

	pipe_reader_log(_sink, LL_DEBUG, "{} {} bars of {} loaded to cache",
		loadedBars.size(), PERIOD_NAME[period], stdCode);
	return true;
}

bool WtDataReaderAD::update_cache_from_lmdb(BarsList& barsList, const char* exchg, const char* code,
	WTSKlinePeriod period, uint32_t& lastBarTime, uint32_t cutoff, double factor, uint32_t minTradingDate)
{
	if (!accept_key_identity(_sink, exchg, code, "update_cache_from_lmdb"))
		return false;
	bool isDay = (period == KP_DAY);
	if (lastBarTime > cutoff)
		return true;

	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;

	LMDBBarKey lKey(exchg, code, lastBarTime);
	LMDBBarKey rKey(exchg, code, cutoff);
	const std::string lower((const char*)&lKey, sizeof(lKey));
	const std::string upper((const char*)&rKey, sizeof(rKey));
	const std::size_t chunkLimit = bar_update_chunk_override != 0 ? bar_update_chunk_override : kBarUpdateChunk;

	// Decodes one record of the inclusive range; false means malformed.
	auto decode = [isDay, cutoff](const MDB_val& key, const MDB_val& item, bool& keep, WTSBarStruct& bar) {
		if (key.mv_size != sizeof(LMDBBarKey) || item.mv_size != sizeof(WTSBarStruct))
			return false;
		memcpy(&bar, item.mv_data, sizeof(bar));
		const uint64_t barTime = isDay ? bar.date : bar.time;
		if (isDay)
			wt_ad_daybar::normalize(bar);	//卡3 3c=C
		keep = barTime <= cutoff;
		return true;
	};

	// R122-002 phase 1: validate the whole range inside one snapshot without
	// staging it. Only a range that fits one chunk is kept, and then it is
	// applied from that same snapshot. Nothing is published or called back
	// unless every record in the range is well formed.
	std::vector<WTSBarStruct> staged;
	bool overflowed = false;
	bool more = false;
	int cnt = stream_checked_range(*db, lower, false, upper, 0, false,
		[&](const MDB_val& key, const MDB_val& item) {
			WTSBarStruct bar;
			bool keep = false;
			if (!decode(key, item, keep, bar))
				return false;
			if (keep && !overflowed)
			{
				if (staged.size() < chunkLimit)
					staged.push_back(bar);
				else
				{
					overflowed = true;
					std::vector<WTSBarStruct>().swap(staged);
				}
			}
			return true;
		}, more);
	if (cnt < 0)
	{
		pipe_reader_log(_sink, LL_ERROR, "Malformed bar key/value in LMDB while updating {}.{} {} bars",
			exchg, code, PERIOD_NAME[period]);
		return false;
	}
	observe_staging(staged.size());

	uint32_t committedLastBarTime = lastBarTime;
	auto apply = [&](const WTSBarStruct& sourceBar) {
		// K34=A: a rule cache keeps only its newest section's trading dates, at its factor.
		if (sourceBar.date < minTradingDate)
			return;
		WTSBarStruct curBar = sourceBar;
		if (factor != 1.0)
			rule_scale_prices(curBar, factor);
		uint64_t curBarTime = isDay ? curBar.date : curBar.time;
		if (curBarTime < committedLastBarTime)
			return;
		if (curBarTime == committedLastBarTime)
		{
			if (barsList._last_from_cache)
			{
				barsList._last_from_cache = false;
				memcpy(&barsList._bars.mutate().back(), &curBar, sizeof(WTSBarStruct));
			}
		}
		else
		{
			barsList._bars.push_back(curBar);
			barsList._last_from_cache = false;
			committedLastBarTime = (uint32_t)curBarTime;
			_sink->on_bar(barsList._code.c_str(), barsList._period, &barsList._bars.back());
		}
	};

	if (!overflowed)
	{
		for (const WTSBarStruct& curBar : staged)
			apply(curBar);
	}
	else
	{
		// Phase 2 for a range longer than one chunk: re-read it chunk by chunk,
		// each chunk validated in its own snapshot and applied with the
		// transaction closed so callbacks may re-enter the reader. Corruption
		// already present was rejected by phase 1; only a concurrent rewrite
		// between the phases can stop here, and lastBarTime then covers exactly
		// the bars already applied, so a retry never repeats a callback.
		std::string resume = lower;
		bool exclusive = false;
		for (;;)
		{
			std::vector<WTSBarStruct> chunk;
			std::string lastKey;
			more = false;
			const int read = stream_checked_range(*db, resume, exclusive, upper, chunkLimit, false,
				[&](const MDB_val& key, const MDB_val& item) {
					WTSBarStruct bar;
					bool keep = false;
					if (!decode(key, item, keep, bar))
						return false;
					if (keep)
						chunk.push_back(bar);
					lastKey.assign((const char*)key.mv_data, key.mv_size);
					return true;
				}, more);
			if (read < 0)
			{
				lastBarTime = committedLastBarTime;
				pipe_reader_log(_sink, LL_ERROR,
					"{}.{} {} bars changed while being applied; stopped after {}",
					exchg, code, PERIOD_NAME[period], committedLastBarTime);
				return false;
			}
			observe_staging(chunk.size());
			for (const WTSBarStruct& curBar : chunk)
				apply(curBar);
			if (!more || lastKey.empty())
				break;
			resume.swap(lastKey);
			exclusive = true;
		}
	}
	lastBarTime = committedLastBarTime;

	pipe_reader_log(_sink, LL_DEBUG, "{} bars of {}.{} updated to {}",
		PERIOD_NAME[period], exchg, code, lastBarTime);
	return true;
}

WtDataReaderAD::RTBarRead WtDataReaderAD::get_rt_cache_bar(const char* exchg, const char* code, WTSKlinePeriod period, WTSBarStruct& result)
{
	RTBarCacheWrapper* wrapper = NULL;
	if (period == KP_DAY)
		wrapper = &_d1_cache;
	else if (period == KP_Minute1)
		wrapper = &_m1_cache;
	else if (period == KP_Minute5)
		wrapper = &_m5_cache;

	if (wrapper == NULL || exchg == NULL || code == NULL)
		return RTBarRead::Absent;

	StdUniqueLock lock(wrapper->_mtx);
	if (wrapper->empty())
	{
		const std::string filename = _base_dir + wrapper->_filename;
		try
		{
			if (!StdFile::exists(filename.c_str()))
				return RTBarRead::Absent;
			BoostMFPtr mapped(new BoostMappingFile);
			if (!mapped->map(filename.c_str(), boost::interprocess::read_only,
				boost::interprocess::read_only) || mapped->addr() == NULL ||
				mapped->size() < sizeof(RTBlockHeader))
			{
				lock.unlock();
				pipe_reader_log(_sink, LL_ERROR, "Invalid or truncated real-time bar cache {}", filename);
				return RTBarRead::Error;
			}
			wrapper->_file_ptr = mapped;
			wrapper->_cache_block = static_cast<RTBarCache*>(mapped->addr());
			wrapper->_last_size = 0;
		}
		catch (const std::exception& ex)
		{
			lock.unlock();
			pipe_reader_log(_sink, LL_ERROR, "Cannot map real-time bar cache {}: {}", filename, ex.what());
			return RTBarRead::Error;
		}
	}
	const auto rejectCache = [&]() {
		const std::string filename = wrapper->_filename;
		wrapper->_idx.clear();
		wrapper->_last_size = 0;
		wrapper->_cache_block = NULL;
		wrapper->_file_ptr.reset();
		// The sink may re-enter the reader; do not hold the cache lock while
		// reporting a malformed mapping.
		lock.unlock();
		pipe_reader_log(_sink, LL_ERROR, "Invalid or truncated real-time bar cache {}", filename);
		return RTBarRead::Error;
	};

	static_assert(sizeof(RTBarCache) == sizeof(RTBlockHeader), "unexpected AD cache header layout");
	static_assert(offsetof(BarCacheItem, _bar) == 48 &&
		sizeof(BarCacheItem) == 48 + sizeof(WTSBarStruct), "unexpected AD bar cache layout");
	const auto* bytes = reinterpret_cast<const unsigned char*>(wrapper->_cache_block);
	uint16_t type = 0, version = 0;
	uint32_t size = 0, capacity = 0;
	std::memcpy(&type, bytes + FLAG_SIZE, sizeof(type));
	std::memcpy(&version, bytes + FLAG_SIZE + sizeof(type), sizeof(version));
	std::memcpy(&size, bytes + sizeof(BlockHeader), sizeof(size));
	std::memcpy(&capacity, bytes + sizeof(BlockHeader) + sizeof(size), sizeof(capacity));
	const size_t mappedRecords = (wrapper->_file_ptr->size() - sizeof(RTBarCache)) / sizeof(BarCacheItem);
	if (std::memcmp(bytes, BLK_FLAG, FLAG_SIZE) != 0 || type != BT_RT_Cache ||
		!wt_ad_daybar::isAcceptedRTCacheVersion(version, wrapper == &_d1_cache) || size > capacity || size > mappedRecords)
		return rejectCache();

	// Build new index entries only after every published record has passed its
	// bounded identity check. A rejected update must not publish half an index.
	if (wrapper->_last_size != size)
	{
		auto staged = wrapper->_idx;
		uint32_t begin = wrapper->_last_size;
		if (begin > size)
		{
			staged.clear();
			begin = 0;
		}
		for (uint32_t i = begin; i < size; ++i)
		{
			const auto* entry = bytes + sizeof(RTBarCache) + static_cast<size_t>(i) * sizeof(BarCacheItem);
			char itemExchg[16], itemCode[32];
			std::memcpy(itemExchg, entry, sizeof(itemExchg));
			std::memcpy(itemCode, entry + sizeof(itemExchg), sizeof(itemCode));
			if (std::memchr(itemExchg, '\0', sizeof(itemExchg)) == NULL ||
				std::memchr(itemCode, '\0', sizeof(itemCode)) == NULL)
				return rejectCache();
			staged[fmtutil::format<64>("{}.{}", itemExchg, itemCode)] = i;
		}
		wrapper->_idx.swap(staged);
		wrapper->_last_size = size;
	}

	const auto it = wrapper->_idx.find(fmtutil::format<64>("{}.{}", exchg, code));
	if (it == wrapper->_idx.end() || it->second >= size)
		return RTBarRead::Absent;
	const auto* entry = bytes + sizeof(RTBarCache) + static_cast<size_t>(it->second) * sizeof(BarCacheItem);
	char actualExchg[16], actualCode[32];
	std::memcpy(actualExchg, entry, sizeof(actualExchg));
	std::memcpy(actualCode, entry + sizeof(actualExchg), sizeof(actualCode));
	if (std::memchr(actualExchg, '\0', sizeof(actualExchg)) == NULL ||
		std::memchr(actualCode, '\0', sizeof(actualCode)) == NULL ||
		std::strcmp(actualExchg, exchg) != 0 || std::strcmp(actualCode, code) != 0)
		return rejectCache();
	std::memcpy(&result, entry + offsetof(BarCacheItem, _bar), sizeof(result));
	if (period == KP_DAY)
		wt_ad_daybar::normalize(result);	//卡3 3c=C：版本 1 旧日线缓存兼容读取
	return RTBarRead::Found;
}

WTSKlineSlice* WtDataReaderAD::readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readKlineSlice"))
		return NULL;
	if (count == 0)
	{
		// Preserve the established empty-slice shape only when this reader already
		// owns a cache buffer. A first zero-count read has no backing block and
		// historically returns NULL.
		const std::string zeroCountKey = fmt::format("{}#{}", stdCode, (uint32_t)period);
		auto cached = _bars_cache.find(zeroCountKey);
		if (cached == _bars_cache.end() || cached->second._bars.empty())
			return NULL;
	}

	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else if (etime >= 10000000000000000ull)
	{
		// AD documents YYYYMMDDHHMMSSmmm. The generic IDataReader contract also
		// documents YYYYMMDDHHMM, so keep both public encodings unambiguous.
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}
	else if (etime <= 999999999999ull)
	{
		// Generic IDataReader encoding: YYYYMMDDHHMM (minute precision).
		curDate = (uint32_t)(etime / 10000);
		curTime = (uint32_t)(etime % 10000);
		curSecs = 0;
	}
	else
	{
		pipe_reader_log(_sink, LL_ERROR, "Unsupported Kline etime encoding {} for {}", etime, stdCode);
		return NULL;
	}
	if (curDate < 19900000u || curTime > 2359u || curTime % 100u > 59u)
	{
		pipe_reader_log(_sink, LL_ERROR, "Invalid Kline etime {} for {}", etime, stdCode);
		return NULL;
	}

	uint32_t endTDate = 0;
	if (!ad_reader_trading_date(_sink, _date_binding, _base_data_mgr, stdPID.c_str(), curDate, curTime, endTDate))
		return NULL;

	// Decision K34=A (2026-11-04, after C): a HOT/custom rule code is stitched from the
	// physical contracts of its rule sections, each limited to its own trading dates and
	// adjusted by its section factor; the cache then follows the newest section's
	// physical contract and is rebuilt when the rule switches. Pre-merged product
	// history and an unbounded raw-contract fallback are never used.
	const bool isRule = cInfo._ruletag[0] != '\0';
	std::string curCode = cInfo._code;
	if (isRule)
	{
		const char* rawCode = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), endTDate);
		if (!accept_key_identity(_sink, cInfo._exchg, rawCode, "readKlineSlice(rule raw code)"))
			return NULL;
		curCode = rawCode;
	}

	const bool isDay = (period == KP_DAY);
	// R100-003/R32-003: minute bar times are (date-19900000)*10000+HHMM. Compute
	// in 64 bits; V1 LMDB minute keys hold 32 bits and the V2 key migration is not
	// enabled, so an unrepresentable cutoff (2033 onwards) is refused rather than
	// wrapped into an earlier key range.
	const uint64_t minuteCutoff = (uint64_t)(curDate - 19900000u) * 10000u + curTime;
	if (!isDay && minuteCutoff > UINT32_MAX)
	{
		pipe_reader_log(_sink, LL_ERROR,
			"Kline cutoff {} of {} exceeds the 32-bit LMDB minute key range; request refused",
			minuteCutoff, stdCode);
		return NULL;
	}
	const uint32_t cutoffBarTime = isDay ? endTDate : (uint32_t)minuteCutoff;
	if (count > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Kline request count {} for {} exceeds the supported limit", count, stdCode);
		return NULL;
	}

	std::string key = fmt::format("{}#{}", stdCode, (uint32_t)period);
	BarsList& barsList = _bars_cache[key];
	const bool cachedFutureBar = !barsList._bars.empty() &&
		(isDay ? barsList._bars.back().date : barsList._bars.back().time) > cutoffBarTime;
	const bool cutoffMovedBack = barsList._last_req_time != 0 && cutoffBarTime < barsList._last_req_time;
	const bool ruleSourceChanged = isRule && (!barsList._is_rule || barsList._rule_phys_code != curCode);
	if (count > 0 && (barsList._bars.capacity() < count ||
		(barsList._bars.empty() && barsList._last_req_time == 0) ||
		cutoffMovedBack || cachedFutureBar || ruleSourceChanged))
	{
		// Capacity growth or a cutoff behind the current cache needs a bounded reload.
		const bool loaded = isRule
			? cacheRuleBars(key, stdCode, cInfo._exchg, cInfo._ruletag, cInfo.stdCommID(), cInfo._exright,
				period, count, cutoffBarTime, endTDate)
			: cacheBarsFromStorage(key, stdCode, period, count, cutoffBarTime);
		if (!loaded)
			return NULL;
	}
	const double ruleFactor = isRule ? barsList._rule_factor : 1.0;
	const uint32_t ruleStartDate = isRule ? barsList._rule_start_date : 0;
	if (cInfo._ruletag[0] != '\0' && barsList._bars.empty())
	{
		// With no bounded product-key history, the custom-rule storage source is
		// unknown. Do not scan the currently mapped raw contract from the beginning.
		pipe_reader_log(_sink, LL_WARN,
			"No bounded history for custom-rule {}; refusing raw-contract fallback", stdCode);
		return NULL;
	}

	//这里只需要检查一下RTBarCache的K线时间戳和etime是否一致
	//如果一致，说明最后一条K线还没完成，但是系统要读取，这个时候就用缓存中的最后一条bar追加到最后
	//如果不一致，说明Writer那边已经处理完成了，直接用缓存好的K线即可
	//OnMinuteEnd的时候也要做类似的检查
	etime = cutoffBarTime;
	uint32_t lastBarTime = barsList._bars.empty()
		? 0
		: (isDay ? barsList._bars.back().date : (uint32_t)barsList._bars.back().time);
	if (lastBarTime < etime || (lastBarTime == etime && barsList._last_from_cache))
	{
		// Recheck the inclusive LMDB boundary while the tail is still RT data, so
		// a writer flush can replace that provisional bar without adding a duplicate.
		if (!update_cache_from_lmdb(barsList, cInfo._exchg, curCode.c_str(), period,
			lastBarTime, cutoffBarTime, ruleFactor, ruleStartDate))
			return NULL;
		lastBarTime = barsList._bars.empty()
			? 0
			: (isDay ? barsList._bars.back().date : (uint32_t)barsList._bars.back().time);
	}

	// RT cache can advance between reads with the same cutoff; inspect it on every call.
	WTSBarStruct rtSnapshot;
	const RTBarRead rtRead = get_rt_cache_bar(cInfo._exchg, curCode.c_str(), period, rtSnapshot);
	if (rtRead == RTBarRead::Error)
		return NULL;
	bool rtUsable = rtRead == RTBarRead::Found;
	if (rtUsable && isRule)
	{
		// K34=A: only the newest section's own trading dates, at its factor.
		if (rtSnapshot.date < ruleStartDate)
			rtUsable = false;
		else if (ruleFactor != 1.0)
			rule_scale_prices(rtSnapshot, ruleFactor);
	}
	if (rtUsable)
	{
		uint64_t cacheBarTime = isDay ? rtSnapshot.date : rtSnapshot.time;
		if (cacheBarTime > etime)
		{
			// The writer may have flushed this bar while the RT cache was read.
			if (lastBarTime < etime && !update_cache_from_lmdb(barsList, cInfo._exchg,
				curCode.c_str(), period, lastBarTime, cutoffBarTime, ruleFactor, ruleStartDate))
				return NULL;
		}
		else if (cacheBarTime > lastBarTime)
		{
			barsList._bars.push_back(rtSnapshot);
			barsList._last_from_cache = true;
			pipe_reader_log(_sink, LL_DEBUG,
				"{} bars at {} of {} updated from cache instead of lmdb in {}",
				PERIOD_NAME[period], etime, stdCode, __FUNCTION__);
		}
		else if (cacheBarTime == lastBarTime && barsList._last_from_cache && !barsList._bars.empty())
		{
			memcpy(&barsList._bars.mutate().back(), &rtSnapshot, sizeof(WTSBarStruct));
		}
	}

	// The request cutoff is also the cache reload boundary for a later call.
	barsList._last_req_time = etime;
	if (barsList._bars.empty())
		return NULL;

	//这里要注意一下，因为读取数据是顺序的，但是返回是需要从后往前取指定条数的，所以应该先读取array_two
	count = min((uint32_t)barsList._bars.size(), count);	//先对count做一个修正
	auto ayTwo = barsList._bars.array_two();
	auto cnt_2 = ayTwo.second;
	if (count == 0)
	{
		auto ayOne = barsList._bars.array_one();
		return WTSKlineSlice::create(stdCode, period, 1, ayOne.first, 0);
	}
	if (cnt_2 >= count)
	{
		//array_two 是物理连续段；operator[] 使用逻辑下标，回绕时不能代替段内偏移。
		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, ayTwo.first + (ayTwo.second - count), count);
		if (slice != NULL)
			slice->addOwner(barsList._bars.owner());
		return slice;
	}
	else
	{
		//如果array_two条数不够，需要再从array_one提取
		auto ayOne = barsList._bars.array_one();
		auto diff = count - cnt_2;
		auto ret = WTSKlineSlice::create(stdCode, period, 1, ayOne.first + (ayOne.second - diff), diff);
		if (ret == NULL)
			return NULL;
		ret->addOwner(barsList._bars.owner());
		if (cnt_2 > 0 && !ret->appendBlock(ayTwo.first, cnt_2))
		{
			ret->release();
			return NULL;
		}
		return ret;
	}


	return NULL;
}

bool WtDataReaderAD::rule_sections(const char* stdCode, const char* ruleTag, const char* stdCommID,
	uint32_t endTDate, HotSections& secs)
{
	secs.clear();
	if (_hot_mgr == NULL || !_hot_mgr->splitCustomSections(ruleTag, stdCommID, 19900102, endTDate, secs) || secs.empty())
	{
		pipe_reader_log(_sink, LL_ERROR, "No {} rule sections of {} up to {}; rule-mapped code refused",
			ruleTag == NULL ? "" : ruleTag, stdCode == NULL ? "" : stdCode, endTDate);
		return false;
	}
	return true;
}

bool WtDataReaderAD::read_section_bars(const char* exchg, const char* code, const char* stdCommID, WTSKlinePeriod period,
	uint32_t sDate, uint32_t eDate, uint32_t cutoff, uint32_t maxCount, std::vector<WTSBarStruct>& out)
{
	out.clear();
	if (maxCount == 0)
		return true;
	if (!accept_key_identity(_sink, exchg, code, "read_section_bars"))
		return false;
	const bool isDay = (period == KP_DAY);
	uint32_t lo = 0;
	uint32_t hi = cutoff;
	if (isDay)
	{
		lo = sDate;
		hi = (std::min)(eDate, cutoff);
	}
	else
	{
		wtp::DateMappingClient mapping = _date_binding.client(_base_data_mgr);
		const wtp::DateMappingResult64 sBound = mapping.getBoundaryTime(stdCommID, sDate, false, true);
		const wtp::DateMappingResult64 eBound = mapping.getBoundaryTime(stdCommID, eDate, false, false);
		if (sBound.status != wtp::DateMappingStatus::Ok || eBound.status != wtp::DateMappingStatus::Ok)
		{
			pipe_reader_log(_sink, LL_ERROR, "Mapping boundaries of {}.{} section {}-{} failed: {}/{}", exchg, code, sDate, eDate,
				wtp::dateMappingStatusName(sBound.status), wtp::dateMappingStatusName(eBound.status));
			return false;
		}
		// Without valid boundaries the scan of a contract that kept trading after its
		// section could return only out-of-section records, so the read fails closed.
		uint32_t date = 0, hhmm = 0;
		if (!rule_boundary_parts(sBound.value, -kRuleSectionLeadMinutes, date, hhmm) || !rule_minute_key(date, hhmm, lo) ||
			!rule_boundary_parts(eBound.value, 0, date, hhmm))
		{
			pipe_reader_log(_sink, LL_ERROR, "Boundaries {}/{} of {}.{} section {}-{} are not usable minute times",
				sBound.value, eBound.value, exchg, code, sDate, eDate);
			return false;
		}
		uint32_t upper = 0;
		if (rule_minute_key(date, hhmm, upper))
			hi = (std::min)(upper, cutoff);
	}
	if (lo > hi)
		return true;

	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;
	WtLMDBQuery query(*db);
	LMDBBarKey lKey(exchg, code, lo);
	LMDBBarKey rKey(exchg, code, hi);
	bool valuesValid = true;
	const int cnt = query.get_lowers_checked(std::string((const char*)&lKey, sizeof(lKey)),
		std::string((const char*)&rKey, sizeof(rKey)), static_cast<int>(maxCount),
		[&](const ValueArray& ayKeys, const ValueArray& ayVals) {
			if (ayKeys.size() != ayVals.size())
			{
				valuesValid = false;
				return;
			}
			for (std::size_t i = 0; i < ayVals.size(); i++)
			{
				if (ayKeys[i].size() != sizeof(LMDBBarKey) || ayVals[i].size() != sizeof(WTSBarStruct))
				{
					valuesValid = false;
					return;
				}
			}
			for (std::size_t i = 0; i < ayVals.size(); i++)
			{
				if (memcmp(ayKeys[i].data(), (void*)&lKey, sizeof(lKey)) < 0)
					continue;
				WTSBarStruct bar;
				memcpy(&bar, ayVals[i].data(), sizeof(bar));
				const uint64_t barTime = isDay ? bar.date : bar.time;
				if (barTime < lo || barTime > hi)
					continue;
				if (isDay)
					wt_ad_daybar::normalize(bar);	//卡3 3c=C
				// Records are assigned to sections by their own trading date.
				if (bar.date < sDate || bar.date > eDate)
					continue;
				out.push_back(bar);
			}
		});
	if (cnt < 0 || !valuesValid)
	{
		pipe_reader_log(_sink, LL_ERROR, "Malformed bar key/value in LMDB while loading {} bars of {}.{} section {}-{}",
			PERIOD_NAME[period], exchg, code, sDate, eDate);
		out.clear();
		return false;
	}
	return true;
}

bool WtDataReaderAD::cacheRuleBars(const std::string& key, const char* stdCode, const char* exchg, const char* ruleTag,
	const char* stdCommID, uint8_t exright, WTSKlinePeriod period, uint32_t count, uint32_t cutoff, uint32_t endTDate)
{
	if (count > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Kline request count {} for {} exceeds the supported limit", count, stdCode);
		return false;
	}
	HotSections secs;
	if (!rule_sections(stdCode, ruleTag, stdCommID, endTDate, secs))
		return false;
	// Forward adjustment keeps the newest section unchanged; backward adjustment scales
	// every section by its own factor (as the file-based reader does).
	const double baseFactor = (exright == 1) ? secs.back()._factor : 1.0;
	if (exright != 0 && !(baseFactor > 0.0))
	{
		pipe_reader_log(_sink, LL_ERROR, "Invalid rule factor {} for {}; request refused", baseFactor, stdCode);
		return false;
	}

	std::vector<std::vector<WTSBarStruct>> parts;	// newest section first
	std::size_t total = 0;
	for (auto it = secs.rbegin(); it != secs.rend() && total < count; ++it)
	{
		std::vector<WTSBarStruct> bars;
		if (!read_section_bars(exchg, it->_code.c_str(), stdCommID, period, it->_s_date, it->_e_date, cutoff,
			(uint32_t)(count - total), bars))
			return false;
		const double factor = (exright != 0) ? it->_factor / baseFactor : 1.0;
		if (factor != 1.0)
		{
			for (WTSBarStruct& bar : bars)
				rule_scale_prices(bar, factor);
		}
		total += bars.size();
		parts.push_back(std::move(bars));
	}

	// Only publish the new cache after every section has been read and validated.
	BarsList& barList = _bars_cache[key];
	barList._bars.rset_capacity(count);
	barList._bars.clear();
	for (auto it = parts.rbegin(); it != parts.rend(); ++it)
	{
		for (const WTSBarStruct& bar : *it)
			barList._bars.push_back(bar);
	}
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = exchg;
	barList._last_from_cache = false;
	barList._last_req_time = 0;
	barList._is_rule = true;
	barList._rule_tag = ruleTag;
	barList._rule_comm_id = stdCommID;
	barList._rule_phys_code = secs.back()._code;
	barList._rule_start_date = secs.back()._s_date;
	barList._rule_factor = (exright != 0) ? secs.back()._factor / baseFactor : 1.0;
	barList._rule_exright = exright;

	pipe_reader_log(_sink, LL_DEBUG, "{} {} bars of {} stitched from physical sections, newest {} since {}",
		total, PERIOD_NAME[period], stdCode, barList._rule_phys_code, barList._rule_start_date);
	return true;
}

WTSTickSlice* WtDataReaderAD::readRuleTickSlice(const char* stdCode, const char* exchg, const char* ruleTag,
	const char* stdCommID, uint32_t count, uint64_t etime, uint32_t endTDate)
{
	if (count == 0)
		return NULL;
	if (count > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Tick request count {} for {} exceeds the supported limit", count, stdCode);
		return NULL;
	}
	HotSections secs;
	if (!rule_sections(stdCode, ruleTag, stdCommID, endTDate, secs))
		return NULL;

	wtp::DateMappingClient mapping = _date_binding.client(_base_data_mgr);
	std::vector<std::vector<WTSTickStruct>> parts;	// newest section first
	std::size_t total = 0;
	for (auto it = secs.rbegin(); it != secs.rend() && total < count; ++it)
	{
		const char* phys = it->_code.c_str();
		if (!accept_key_identity(_sink, exchg, phys, "readTickSlice(rule section)"))
			return NULL;
		const wtp::DateMappingResult64 sBound = mapping.getBoundaryTime(stdCommID, it->_s_date, false, true);
		const wtp::DateMappingResult64 eBound = mapping.getBoundaryTime(stdCommID, it->_e_date, false, false);
		if (sBound.status != wtp::DateMappingStatus::Ok || eBound.status != wtp::DateMappingStatus::Ok)
		{
			pipe_reader_log(_sink, LL_ERROR, "Mapping boundaries of {}.{} section {}-{} failed: {}/{}", exchg, phys,
				it->_s_date, it->_e_date, wtp::dateMappingStatusName(sBound.status), wtp::dateMappingStatusName(eBound.status));
			return NULL;
		}
		uint32_t sDay = 0, sHHMM = 0, eDay = 0, eHHMM = 0;
		if (!rule_boundary_parts(sBound.value, -kRuleSectionLeadMinutes, sDay, sHHMM) ||
			!rule_boundary_parts(eBound.value, 0, eDay, eHHMM))
		{
			pipe_reader_log(_sink, LL_ERROR, "Boundaries {}/{} of {}.{} section {}-{} are not usable minute times",
				sBound.value, eBound.value, exchg, phys, it->_s_date, it->_e_date);
			return NULL;
		}
		const uint64_t lo = (uint64_t)sDay * 1000000000 + (uint64_t)sHHMM * 100000;
		const uint64_t hi = (std::min)(etime, (uint64_t)eDay * 1000000000 + (uint64_t)eHHMM * 100000 + 59999);
		if (lo > hi)
			continue;

		WtLMDBPtr db = get_t_db(exchg, phys);
		if (db == NULL)
			return NULL;
		WtLMDBQuery query(*db);
		WtLMDBReaderTestAccess::set_cursor_get_override(query,
			tick_query_cursor_get_override, tick_query_cursor_get_context);
		LMDBHftKey lKey(exchg, phys, (uint32_t)(lo / 1000000000), (uint32_t)(lo % 1000000000));
		LMDBHftKey rKey(exchg, phys, (uint32_t)(hi / 1000000000), (uint32_t)(hi % 1000000000));
		const uint32_t sDate = it->_s_date;
		const uint32_t eDate = it->_e_date;
		std::vector<WTSTickStruct> ticks;
		bool valuesValid = true;
		const int cnt = query.get_lowers_checked(std::string((const char*)&lKey, sizeof(lKey)),
			std::string((const char*)&rKey, sizeof(rKey)), static_cast<int>(count - total),
			[&](const ValueArray& ayKeys, const ValueArray& ayVals) {
				if (ayKeys.size() != ayVals.size())
				{
					valuesValid = false;
					return;
				}
				for (std::size_t i = 0; i < ayVals.size(); i++)
				{
					if (ayKeys[i].size() != sizeof(LMDBHftKey) || ayVals[i].size() != sizeof(WTSTickStruct))
					{
						valuesValid = false;
						return;
					}
				}
				for (std::size_t i = 0; i < ayVals.size(); i++)
				{
					if (memcmp(ayKeys[i].data(), (void*)&lKey, sizeof(lKey)) < 0)
						continue;
					WTSTickStruct tick;
					memcpy(&tick, ayVals[i].data(), sizeof(tick));
					// Ticks are assigned to sections by their own trading date.
					if (tick.trading_date < sDate || tick.trading_date > eDate)
						continue;
					ticks.push_back(tick);
				}
			});
		if (cnt < 0 || !valuesValid)
		{
			pipe_reader_log(_sink, LL_ERROR, "Malformed tick key/value in LMDB while loading {}.{} section {}-{}",
				exchg, phys, sDate, eDate);
			return NULL;
		}
		total += ticks.size();
		parts.push_back(std::move(ticks));
	}
	if (total == 0)
		return NULL;

	std::shared_ptr<std::vector<WTSTickStruct>> holder = std::make_shared<std::vector<WTSTickStruct>>();
	holder->reserve(total);
	for (auto it = parts.rbegin(); it != parts.rend(); ++it)
		holder->insert(holder->end(), it->begin(), it->end());
	WTSTickSlice* slice = WTSTickSlice::create(stdCode, holder->data(), (uint32_t)holder->size());
	if (slice != NULL && !slice->addOwner(holder))
	{
		slice->release();
		return NULL;
	}
	return slice;
}

void WtDataReaderAD::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */)
{
	//这里应该触发检查
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	if (nowTime <= _last_time)
		return;

	// R100-003/R32-003: widen before multiplying; a minute end that the 32-bit
	// LMDB minute key cannot represent (or a date before the 1990 epoch) must not
	// be narrowed into an unrelated earlier range.
	const bool minuteEndRepresentable = uDate >= 19900000u &&
		((uint64_t)(uDate - 19900000u) * 10000u + uTime) <= UINT32_MAX;
	const uint64_t minuteEndBarTime = uDate >= 19900000u
		? (uint64_t)(uDate - 19900000u) * 10000u + uTime : 0;
	const uint32_t dailyEndDate = endTDate;
	bool rtCacheError = false;
	bool minuteEndRefused = false;

	for (auto it = _bars_cache.begin(); it != _bars_cache.end(); it++)
	{
		BarsList& barsList = (BarsList&)it->second;
		if (barsList._bars.empty())
			continue;

		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(barsList._code.c_str(), _hot_mgr);
		if (barsList._period != KP_DAY && !minuteEndRepresentable)
		{
			minuteEndRefused = true;
			continue;
		}
		// K34=A: a stitched rule cache follows the physical contract of its newest
		// section; when the rule has switched it is rebuilt from the sections.
		if (barsList._is_rule && endTDate != 0)
		{
			const char* activeCode = _hot_mgr->getCustomRawCode(barsList._rule_tag.c_str(), barsList._rule_comm_id.c_str(), endTDate);
			if (activeCode != NULL && activeCode[0] != '\0' && barsList._rule_phys_code != activeCode)
			{
				const std::string ruleKey = it->first;
				const std::string ruleCode = barsList._code;
				const std::string ruleExchg = barsList._exchg;
				const std::string ruleTag = barsList._rule_tag;
				const std::string ruleCommId = barsList._rule_comm_id;
				const uint8_t ruleExright = barsList._rule_exright;
				const WTSKlinePeriod rulePeriod = barsList._period;
				const bool ruleIsDay = rulePeriod == KP_DAY;
				const uint64_t previousLast = ruleIsDay ? barsList._bars.back().date : barsList._bars.back().time;
				const uint32_t capacity = (uint32_t)barsList._bars.capacity();
				const uint32_t ruleCutoff = ruleIsDay ? dailyEndDate : (uint32_t)minuteEndBarTime;
				if (!cacheRuleBars(ruleKey, ruleCode.c_str(), ruleExchg.c_str(), ruleTag.c_str(), ruleCommId.c_str(),
					ruleExright, rulePeriod, capacity, ruleCutoff, endTDate))
					continue;
				std::vector<WTSBarStruct> fresh;
				for (std::size_t idx = 0; idx < barsList._bars.size(); ++idx)
				{
					const WTSBarStruct& bar = barsList._bars[idx];
					if ((ruleIsDay ? (uint64_t)bar.date : bar.time) > previousLast)
						fresh.push_back(bar);
				}
				// Callbacks get copies; they may re-enter the reader and rebuild the cache.
				for (WTSBarStruct& bar : fresh)
					_sink->on_bar(ruleCode.c_str(), rulePeriod, &bar);
				continue;
			}
		}
		const char* lmdbCode = barsList._is_rule ? barsList._rule_phys_code.c_str() : cInfo._code;
		const double ruleFactor = barsList._is_rule ? barsList._rule_factor : 1.0;
		const uint32_t ruleStartDate = barsList._is_rule ? barsList._rule_start_date : 0;
		if (barsList._period != KP_DAY)
		{
			uint32_t lastBarTime = (uint32_t)barsList._bars.back().time;
			pipe_reader_log(_sink, LL_DEBUG,
				"Updating {} bars of {} in section ({},{}]", PERIOD_NAME[barsList._period], barsList._code, lastBarTime, minuteEndBarTime);
			if (!update_cache_from_lmdb(barsList, barsList._exchg.c_str(), lmdbCode,
				barsList._period, lastBarTime, (uint32_t)minuteEndBarTime, ruleFactor, ruleStartDate))
				continue;
			if(lastBarTime < minuteEndBarTime)
			{
				WTSBarStruct rtSnapshot;
				const RTBarRead rtRead = get_rt_cache_bar(cInfo._exchg, lmdbCode, barsList._period, rtSnapshot);
				if (rtRead == RTBarRead::Error)
					rtCacheError = true;
				const bool rtInSection = rtRead == RTBarRead::Found && rtSnapshot.date >= ruleStartDate;
				if (rtInSection && ruleFactor != 1.0)
					rule_scale_prices(rtSnapshot, ruleFactor);
				if (rtInSection &&
					rtSnapshot.time > lastBarTime && rtSnapshot.time <= minuteEndBarTime)
				{
					barsList._bars.push_back(rtSnapshot);
					barsList._last_from_cache = true;
					// The callback receives this invocation's aligned snapshot, which
					// remains valid even if it re-enters and grows the bars buffer.
					_sink->on_bar(barsList._code.c_str(), barsList._period, &rtSnapshot);
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @ {} of {} updated from cache instead of lmdb in {}", PERIOD_NAME[barsList._period], minuteEndBarTime, barsList._code, __FUNCTION__);
				}
			}
		}
		else if(endTDate != 0)
		{
			uint32_t lastBarTime = barsList._bars.back().date;
			if (!update_cache_from_lmdb(barsList, barsList._exchg.c_str(), lmdbCode,
				barsList._period, lastBarTime, dailyEndDate, ruleFactor, ruleStartDate))
				continue;
			if (lastBarTime < dailyEndDate)
			{
				WTSBarStruct rtSnapshot;
				const RTBarRead rtRead = get_rt_cache_bar(cInfo._exchg, lmdbCode, barsList._period, rtSnapshot);
				if (rtRead == RTBarRead::Error)
					rtCacheError = true;
				const bool rtInSection = rtRead == RTBarRead::Found && rtSnapshot.date >= ruleStartDate;
				if (rtInSection && ruleFactor != 1.0)
					rule_scale_prices(rtSnapshot, ruleFactor);
				if (rtInSection &&
					rtSnapshot.date > lastBarTime && rtSnapshot.date <= dailyEndDate)
				{
					barsList._bars.push_back(rtSnapshot);
					barsList._last_from_cache = true;
					_sink->on_bar(barsList._code.c_str(), barsList._period, &rtSnapshot);
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @  of {} updated from cache instead of lmdb in {}", PERIOD_NAME[barsList._period], dailyEndDate, barsList._code, __FUNCTION__);
				}
			}
		}
	}

	if (minuteEndRefused)
	{
		pipe_reader_log(_sink, LL_ERROR,
			"Minute end {}.{} exceeds the 32-bit LMDB minute key range; minute bars were not updated",
			uDate, uTime);
	}

	// Earlier callbacks may already have run, so preserve the monotonic gate
	// even on failure; retrying this minute could duplicate those callbacks.
	if (rtCacheError || minuteEndRefused)
	{
		_last_time = nowTime;
		return;
	}
	if (_sink)
		_sink->on_all_bar_updated(uTime);

	_last_time = nowTime;
}

WtDataReaderAD::WtLMDBPtr WtDataReaderAD::get_k_db(const char* exchg, WTSKlinePeriod period)
{
	WtLMDBMap* the_map = NULL;
	std::string subdir;
	if (period == KP_Minute1)
	{
		the_map = &_exchg_m1_dbs;
		subdir = "min1";
	}
	else if (period == KP_Minute5)
	{
		the_map = &_exchg_m5_dbs;
		subdir = "min5";
	}
	else if (period == KP_DAY)
	{
		the_map = &_exchg_d1_dbs;
		subdir = "day";
	}
	else
		return WtLMDBPtr();

	auto it = the_map->find(exchg);
	if (it != the_map->end())
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}{}/{}/", _base_dir.c_str(), subdir.c_str(), exchg);
	fs::path dbPath(path);
	boost::system::error_code fsError;
	fs::create_directories(dbPath, fsError);
	if (fsError)
	{
		pipe_reader_log(_sink, LL_ERROR, "Creating {} db directory {} failed: {}",
			subdir, path, fsError.message());
		return WtLMDBPtr();
	}
	fsError.clear();
	if (!fs::is_directory(dbPath, fsError))
	{
		const std::string reason = fsError ? fsError.message() : "path is not a directory";
		pipe_reader_log(_sink, LL_ERROR, "Creating {} db directory {} failed: {}",
			subdir, path, reason);
		return WtLMDBPtr();
	}
	if (!dbPtr->open(path.c_str()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Opening {} db of {} failed: {}", subdir, exchg, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	else
	{
		pipe_reader_log(_sink, LL_DEBUG, "{} db of {} opened", subdir, exchg);
	}

	(*the_map)[exchg] = dbPtr;
	return dbPtr;
}

WtDataReaderAD::WtLMDBPtr WtDataReaderAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = fmtutil::format<64>("{}.{}", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}ticks/{}/{}", _base_dir.c_str(), exchg, code);
	fs::path dbPath(path);
	boost::system::error_code fsError;
	fs::create_directories(dbPath, fsError);
	if (fsError)
	{
		pipe_reader_log(_sink, LL_ERROR, "Creating tick db directory {} failed: {}",
			path, fsError.message());
		return WtLMDBPtr();
	}
	fsError.clear();
	if (!fs::is_directory(dbPath, fsError))
	{
		const std::string reason = fsError ? fsError.message() : "path is not a directory";
		pipe_reader_log(_sink, LL_ERROR, "Creating tick db directory {} failed: {}",
			path, reason);
		return WtLMDBPtr();
	}
	if (!dbPtr->open(path.c_str()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Opening tick db of {}.{} failed: {}", exchg, code, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	else
	{
		pipe_reader_log(_sink, LL_DEBUG, "Tick db of {}.{} opened", exchg, code);
	}

	_tick_dbs[key] = dbPtr;
	return dbPtr;
}
