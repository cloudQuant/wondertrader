#include "WtRdmDtReaderAD.h"
#include "LMDBKeys.h"
#include "DayBarContractAD.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/DateMappingClient.hpp"
#include "../Includes/WTSStruct.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

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

namespace {
thread_local LMDBRangeCursorGetOverride rdm_query_cursor_get_override = NULL;
thread_local void* rdm_query_cursor_get_context = NULL;
thread_local LMDBRangeResultsOverride rdm_query_range_results_override = NULL;
thread_local void* rdm_query_range_results_context = NULL;
}

template<typename... Args>
inline void pipe_rdmreader_log(IRdmDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args);

namespace {

// These are historical LMDB value ABIs already supported by the BT reader.
// Keep them reader-local; disk keys and public data structures are unchanged.
#pragma pack(push, 1)
struct WTSBarStructPackedV2
{
	uint32_t date;
	uint64_t time;
	double open;
	double high;
	double low;
	double close;
	double settle;
	double money;
	double vol;
	double hold;
	double add;
};

struct WTSTickStructPackedV2
{
	char exchg[MAX_EXCHANGE_LENGTH];
	char code[MAX_INSTRUMENT_LENGTH];
	double price;
	double open;
	double high;
	double low;
	double settle_price;
	double upper_limit;
	double lower_limit;
	double total_volume;
	double volume;
	double total_turnover;
	double turn_over;
	double open_interest;
	double diff_interest;
	uint32_t trading_date;
	uint32_t action_date;
	uint32_t action_time;
	double pre_close;
	double pre_settle;
	double pre_interest;
	double bid_prices[10];
	double ask_prices[10];
	double bid_qty[10];
	double ask_qty[10];
};
#pragma pack(pop)

static_assert(sizeof(WTSBarStructPackedV2) == 84, "12098b5b packed bar ABI changed");
static_assert(sizeof(WTSTickStructPackedV2) == 508, "12098b5b packed tick ABI changed");

struct MalformedRecord
{
	bool invalid = false;
	std::size_t key_size = 0;
	std::size_t value_size = 0;
	std::string key;
};

template<typename SourceBar>
void copy_bar_fields(const SourceBar& source, WTSBarStruct& target)
{
	target.date = source.date;
	target.time = source.time;
	target.open = source.open;
	target.high = source.high;
	target.low = source.low;
	target.close = source.close;
	target.settle = source.settle;
	target.money = source.money;
	target.vol = source.vol;
	target.hold = source.hold;
	target.add = source.add;
}

template<typename SourceTick>
void copy_tick_fields(const SourceTick& source, WTSTickStruct& target)
{
	const std::size_t exchg_size = (std::min)(sizeof(source.exchg), sizeof(target.exchg));
	const std::size_t code_size = (std::min)(sizeof(source.code), sizeof(target.code));
	std::memcpy(target.exchg, source.exchg, exchg_size);
	std::memcpy(target.code, source.code, code_size);
	target.price = source.price;
	target.open = source.open;
	target.high = source.high;
	target.low = source.low;
	target.settle_price = source.settle_price;
	target.upper_limit = source.upper_limit;
	target.lower_limit = source.lower_limit;
	target.total_volume = source.total_volume;
	target.volume = source.volume;
	target.total_turnover = source.total_turnover;
	target.turn_over = source.turn_over;
	target.open_interest = source.open_interest;
	target.diff_interest = source.diff_interest;
	target.trading_date = source.trading_date;
	target.action_date = source.action_date;
	target.action_time = source.action_time;
	target.pre_close = source.pre_close;
	target.pre_settle = source.pre_settle;
	target.pre_interest = source.pre_interest;
	for (std::size_t level = 0; level < 10; ++level)
	{
		target.bid_prices[level] = source.bid_prices[level];
		target.ask_prices[level] = source.ask_prices[level];
		target.bid_qty[level] = source.bid_qty[level];
		target.ask_qty[level] = source.ask_qty[level];
	}
}

bool decode_records(const ValueArray& keys, const ValueArray& values,
	std::vector<WTSBarStruct>& decoded, MalformedRecord& malformed)
{
	if (keys.size() != values.size())
	{
		malformed.invalid = true;
		malformed.key_size = keys.empty() ? 0 : keys.front().size();
		malformed.value_size = values.empty() ? 0 : values.front().size();
		malformed.key = keys.empty() ? std::string() : keys.front();
		return false;
	}

	if (values.size() > decoded.max_size())
	{
		malformed.invalid = true;
		return false;
	}
	for (std::size_t i = 0; i < values.size(); ++i)
	{
		if (keys[i].size() != sizeof(LMDBBarKey) ||
			(values[i].size() != sizeof(WTSBarStruct) &&
				values[i].size() != sizeof(WTSBarStructPackedV2) &&
				values[i].size() != sizeof(WTSBarStructOld)))
		{
			malformed.invalid = true;
			malformed.key_size = keys[i].size();
			malformed.value_size = values[i].size();
			malformed.key = keys[i];
			return false;
		}
	}

	std::vector<WTSBarStruct> result;
	result.reserve(values.size());
	for (const std::string& item : values)
	{
		WTSBarStruct bar;
		if (item.size() == sizeof(WTSBarStruct))
			std::memcpy(&bar, item.data(), sizeof(bar));
		else if (item.size() == sizeof(WTSBarStructPackedV2))
		{
			WTSBarStructPackedV2 packed = {};
			std::memcpy(&packed, item.data(), sizeof(packed));
			copy_bar_fields(packed, bar);
		}
		else
		{
			WTSBarStructOld old_bar;
			std::memcpy(&old_bar, item.data(), sizeof(old_bar));
			copy_bar_fields(old_bar, bar);
		}
		result.emplace_back(bar);
	}
	decoded.swap(result);
	return true;
}

bool decode_records(const ValueArray& keys, const ValueArray& values,
	std::vector<WTSTickStruct>& decoded, MalformedRecord& malformed)
{
	if (keys.size() != values.size())
	{
		malformed.invalid = true;
		malformed.key_size = keys.empty() ? 0 : keys.front().size();
		malformed.value_size = values.empty() ? 0 : values.front().size();
		malformed.key = keys.empty() ? std::string() : keys.front();
		return false;
	}

	if (values.size() > decoded.max_size())
	{
		malformed.invalid = true;
		return false;
	}
	for (std::size_t i = 0; i < values.size(); ++i)
	{
		if (keys[i].size() != sizeof(LMDBHftKey) ||
			(values[i].size() != sizeof(WTSTickStruct) &&
				values[i].size() != sizeof(WTSTickStructPackedV2) &&
				values[i].size() != sizeof(WTSTickStructOld)))
		{
			malformed.invalid = true;
			malformed.key_size = keys[i].size();
			malformed.value_size = values[i].size();
			malformed.key = keys[i];
			return false;
		}
	}

	std::vector<WTSTickStruct> result;
	result.reserve(values.size());
	for (const std::string& item : values)
	{
		WTSTickStruct tick;
		if (item.size() == sizeof(WTSTickStruct))
			std::memcpy(&tick, item.data(), sizeof(tick));
		else if (item.size() == sizeof(WTSTickStructPackedV2))
		{
			WTSTickStructPackedV2 packed = {};
			std::memcpy(&packed, item.data(), sizeof(packed));
			copy_tick_fields(packed, tick);
		}
		else
		{
			WTSTickStructOld old_tick;
			std::memcpy(&old_tick, item.data(), sizeof(old_tick));
			copy_tick_fields(old_tick, tick);
		}
		result.emplace_back(tick);
	}
	decoded.swap(result);
	return true;
}

template<typename Key, typename Value>
bool read_checked_range(WtLMDB& db, const Key& lower_key, const Key& upper_key,
	std::vector<Value>& decoded, MalformedRecord& malformed)
{
	WtLMDBQuery query(db);
	if (rdm_query_cursor_get_override != NULL)
		WtLMDBReaderTestAccess::set_cursor_get_override(query, rdm_query_cursor_get_override, rdm_query_cursor_get_context);
	std::vector<Value> result;
	bool values_valid = true;
	const int count = query.get_range_checked(
		std::string((const char*)&lower_key, sizeof(lower_key)),
		std::string((const char*)&upper_key, sizeof(upper_key)),
		[&result, &values_valid, &malformed](const ValueArray& keys, const ValueArray& values) {
			if (rdm_query_range_results_override == NULL)
				values_valid = decode_records(keys, values, result, malformed);
			else
			{
				// Keep fault injection isolated to private copies of the checked query result.
				ValueArray test_keys(keys);
				ValueArray test_values(values);
				rdm_query_range_results_override(rdm_query_range_results_context, test_keys, test_values);
				values_valid = decode_records(test_keys, test_values, result, malformed);
			}
		});
	if (count < 0 || !values_valid || (std::size_t)count != result.size())
		return false;
	decoded.swap(result);
	return true;
}

template<typename Value>
void log_malformed_range(IRdmDtReaderSink* sink, const char* kind,
	const char* exchg, const char* code, const MalformedRecord& malformed)
{
	pipe_rdmreader_log(sink, LL_ERROR,
		"Malformed LMDB {} record for {}.{}: key bytes={}, value size={} (expected key size {}, value size {}, {} or {})",
		kind, exchg, code, malformed.key_size, malformed.value_size,
		std::is_same<Value, WTSBarStruct>::value ? sizeof(LMDBBarKey) : sizeof(LMDBHftKey),
		sizeof(Value), std::is_same<Value, WTSBarStruct>::value ? sizeof(WTSBarStructPackedV2) : sizeof(WTSTickStructPackedV2),
		std::is_same<Value, WTSBarStruct>::value ? sizeof(WTSBarStructOld) : sizeof(WTSTickStructOld));
}

template<typename Value>
void log_range_failure(IRdmDtReaderSink* sink, const char* kind,
	const char* exchg, const char* code, WtLMDB& db, const MalformedRecord& malformed)
{
	if (malformed.invalid)
		log_malformed_range<Value>(sink, kind, exchg, code, malformed);
	else
		pipe_rdmreader_log(sink, LL_ERROR, "LMDB {} range read failed for {}.{}: {}",
			kind, exchg, code, db.errmsg());
}

} // namespace

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void pipe_rdmreader_log(IRdmDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
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
		constexpr char fallback[] = "WtRdmDtReaderAD log formatting failed";
		std::memcpy(buffer, fallback, sizeof(fallback));
	}

	sink->reader_log(ll, buffer);
}

// Decision card 1 (A+C): trading dates come from the typed client over the
// guarded legacy IBaseDataMgr virtuals; a failure stops the read.
static bool ad_rdm_trading_date(IRdmDtReaderSink* sink, wtp::DateMappingClient& mapping, const char* stdPID,
	uint32_t uDate, uint32_t uTime, uint32_t& tradingDate)
{
	const wtp::DateMappingResult32 result = mapping.calcTradingDate(stdPID, uDate, uTime, false);
	if (result.status != wtp::DateMappingStatus::Ok)
	{
		pipe_rdmreader_log(sink, LL_ERROR, "Mapping trading date of {} at {}.{} failed: {}",
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

// BUG-L04-004: reject identities the fixed LMDB key cannot represent before any
// cache, directory or database side effect, and make the rejection observable.
bool accept_key_identity(IRdmDtReaderSink* sink, const char* exchg, const char* code, const char* entry)
{
	if (lmdbKeyIdentityValid(exchg, code))
		return true;
	pipe_rdmreader_log(sink, LL_ERROR,
		"{} rejected storage identity {}.{}: exchange and code must be non-empty and shorter than {} and {} bytes",
		entry, key_field_for_log(exchg, MAX_EXCHANGE_LENGTH),
		key_field_for_log(code, MAX_INSTRUMENT_LENGTH), MAX_EXCHANGE_LENGTH, MAX_INSTRUMENT_LENGTH);
	return false;
}

// R21-BUG-07: last committed LMDB transaction of the environment. LMDB keeps it
// in the shared meta pages, so commits by other processes are observed too.
bool lmdb_last_txnid(WtLMDB& db, uint64_t& txnid)
{
	if (db.env() == NULL)
		return false;
	MDB_envinfo info;
	if (mdb_env_info(db.env(), &info) != MDB_SUCCESS)
		return false;
	txnid = (uint64_t)info.me_last_txnid;
	return true;
}

// R100-003: minute bar time (date-19900000)*10000+HHMM computed in 64 bits; a
// date before the 1990 epoch maps to 0 instead of wrapping.
uint64_t minute_bar_time(uint32_t date, uint32_t hhmm)
{
	if (date < 19900000u)
		return 0;
	return (uint64_t)(date - 19900000u) * 10000u + hhmm;
}

} // namespace

extern "C"
{
	EXPORT_FLAG IRdmDtReader* createRdmDtReader()
	{
		IRdmDtReader* ret = new WtRdmDtReaderAD();
		return ret;
	}

	EXPORT_FLAG void deleteRdmDtReader(IRdmDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};


WtRdmDtReaderAD::WtRdmDtReaderAD()
	: _base_data_mgr(NULL)
	, _hot_mgr(NULL)
{
}


WtRdmDtReaderAD::~WtRdmDtReaderAD()
{
}

void WtRdmDtReaderAD::set_test_cursor_get_override(LMDBRangeCursorGetOverride callback, void* context)
{
	rdm_query_cursor_get_override = callback;
	rdm_query_cursor_get_context = context;
}

void WtRdmDtReaderAD::set_test_range_results_override(LMDBRangeResultsOverride callback, void* context)
{
	rdm_query_range_results_override = callback;
	rdm_query_range_results_context = context;
}

void WtRdmDtReaderAD::init(WTSVariant* cfg, IRdmDtReaderSink* sink)
{
	IRdmDtReader::init(cfg, sink);

	_base_data_mgr = sink->get_basedata_mgr();
	_hot_mgr = sink->get_hot_mgr();

	if (cfg == NULL)
		return;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	pipe_rdmreader_log(sink, LL_INFO, "WtRdmDtReaderAD initialized, root data folder is {}", _base_dir);
}

WTSTickSlice* WtRdmDtReaderAD::readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (stdCode == NULL)
		return NULL;
	if (count == 0)
		return NULL;
	if (etime == 0)
	{
		uint32_t date = 0, time = 0;
		TimeUtils::getDateTime(date, time);
		etime = (uint64_t)date * 1000000000ull + time;
	}

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readTickSliceByCount"))
		return NULL;
	const std::string cacheKey = std::string(stdCode) + "#count-history";
	auto cacheIt = _ticks_cache.find(cacheKey);
	if (cacheIt == _ticks_cache.end() || cacheIt->second._ticks.empty())
	{
		WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
		if (db == NULL)
			return NULL;

		LMDBHftKey lKey(cInfo._exchg, cInfo._code, 0, 0);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, UINT32_MAX, UINT32_MAX);
		std::vector<WTSTickStruct> loaded;
		MalformedRecord malformed;
		if (!read_checked_range(*db, lKey, rKey, loaded, malformed))
		{
			log_range_failure<WTSTickStruct>(_sink, "tick", cInfo._exchg, cInfo._code, *db, malformed);
			return NULL;
		}
		if (loaded.empty())
			return NULL;

		std::stable_sort(loaded.begin(), loaded.end(), [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			return a.action_time < b.action_time;
		});
		TicksList candidate;
		candidate._ticks.swap(loaded);
		if (cacheIt == _ticks_cache.end())
			cacheIt = _ticks_cache.emplace(cacheKey, std::move(candidate)).first;
		else
			cacheIt->second = std::move(candidate);
	}

	TicksList& tickList = cacheIt->second;
	if (tickList._ticks.empty())
		return NULL;

	WTSTickStruct endTick = {};
	endTick.action_date = (uint32_t)(etime / 1000000000ull);
	endTick.action_time = (uint32_t)(etime % 1000000000ull);
	auto end = std::upper_bound(tickList._ticks.begin(), tickList._ticks.end(), endTick,
		[](const WTSTickStruct& value, const WTSTickStruct& item) {
			if (value.action_date != item.action_date)
				return value.action_date < item.action_date;
			return value.action_time < item.action_time;
		});

	const std::size_t available = (std::size_t)(end - tickList._ticks.begin());
	if (available == 0)
		return NULL;
	const uint32_t actualCount = (uint32_t)std::min<std::size_t>(available, count);
	WTSTickStruct* first = tickList._ticks.data() + available - actualCount;
	WTSTickSlice* slice = WTSTickSlice::create(stdCode, first, actualCount);
	if (slice != NULL)
		slice->addOwner(tickList._ticks.owner());
	return slice;
}

WTSTickSlice* WtRdmDtReaderAD::readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readTickSliceByRange"))
		return NULL;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	WTSSessionInfo* sInfo = commInfo->getSessionInfo();
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = sInfo->offsetTime((uint32_t)(etime % 1000000000) / 100000, false);
	rSecs = (uint32_t)(etime % 100000);

	uint32_t lDate, lTime, lSecs;
	//20190807124533900
	lDate = (uint32_t)(stime / 1000000000);
	lTime = sInfo->offsetTime((uint32_t)(stime % 1000000000) / 100000, true);
	lSecs = (uint32_t)(stime % 100000);

	wtp::DateMappingClient dateMapping(_base_data_mgr);
	uint32_t endTDate = 0, beginTDate = 0;
	if (!ad_rdm_trading_date(_sink, dateMapping, stdPID.c_str(), rDate, rTime, endTDate) ||
		!ad_rdm_trading_date(_sink, dateMapping, stdPID.c_str(), lDate, lTime, beginTDate))
		return NULL;

	std::string key = stdCode;

	// Stage cache data and both possible LMDB reads before publishing any change.
	auto cacheIt = _ticks_cache.find(key);
	TicksList candidate;
	if (cacheIt != _ticks_cache.end())
		candidate = cacheIt->second;
	const bool isEmpty = candidate._ticks.empty();
	const bool bNeedOlder = stime < candidate._first_tick_time;
	const bool bNeedNewer = etime > candidate._last_tick_time;

	//这里要改成从lmdb读取
	WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
	if (db == NULL)
		return NULL;

	std::vector<WTSTickStruct> initialTicks;
	std::vector<WTSTickStruct> olderTicks;
	std::vector<WTSTickStruct> newerTicks;
	if (isEmpty)
	{
		//按照区间加载即可
		LMDBHftKey lKey(cInfo._exchg, cInfo._code, beginTDate, lTime * 100000 + lSecs);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, endTDate, rTime * 100000 + rSecs);
		MalformedRecord malformed;
		if (!read_checked_range(*db, lKey, rKey, initialTicks, malformed))
		{
			log_range_failure<WTSTickStruct>(_sink, "tick", cInfo._exchg, cInfo._code, *db, malformed);
			return NULL;
		}
	}
	else
	{
		if (bNeedOlder)
		{
			//读取更早的数据
			uint32_t firstKeyDate = (uint32_t)(candidate._first_tick_time / 1000000000ull);
			uint32_t firstKeyTime = (uint32_t)(candidate._first_tick_time % 1000000000ull);
			if (firstKeyTime > 0)
				--firstKeyTime;
			else if (firstKeyDate > 0)
			{
				--firstKeyDate;
				firstKeyTime = UINT32_MAX;
			}
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, firstKeyDate, firstKeyTime);
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, beginTDate, lTime * 100000 + lSecs);
			MalformedRecord malformed;
			if (!read_checked_range(*db, lKey, rKey, olderTicks, malformed))
			{
				log_range_failure<WTSTickStruct>(_sink, "tick", cInfo._exchg, cInfo._code, *db, malformed);
				return NULL;
			}
		}

		if (bNeedNewer)
		{
			//读取更新的数据
			uint32_t lastKeyDate = (uint32_t)(candidate._last_tick_time / 1000000000ull);
			uint32_t lastKeyTime = (uint32_t)(candidate._last_tick_time % 1000000000ull);
			if (lastKeyTime < UINT32_MAX)
				++lastKeyTime;
			else
			{
				++lastKeyDate;
				lastKeyTime = 0;
			}
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, lastKeyDate, lastKeyTime);
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, endTDate, rTime * 100000 + rSecs);
			MalformedRecord malformed;
			if (!read_checked_range(*db, lKey, rKey, newerTicks, malformed))
			{
				log_range_failure<WTSTickStruct>(_sink, "tick", cInfo._exchg, cInfo._code, *db, malformed);
				return NULL;
			}
		}
	}

	if (isEmpty)
		candidate._ticks.swap(initialTicks);
	else if (!olderTicks.empty() || !newerTicks.empty())
	{
		std::vector<WTSTickStruct> updated;
		const std::size_t max_size = updated.max_size();
		if (candidate._ticks.size() > max_size ||
			olderTicks.size() > max_size - candidate._ticks.size() ||
			newerTicks.size() > max_size - candidate._ticks.size() - olderTicks.size())
		{
			pipe_rdmreader_log(_sink, LL_ERROR, "Tick cache range is too large for {}", stdCode);
			return NULL;
		}
		updated.reserve(olderTicks.size() + candidate._ticks.size() + newerTicks.size());
		updated.insert(updated.end(), olderTicks.begin(), olderTicks.end());
		updated.insert(updated.end(), candidate._ticks.begin(), candidate._ticks.end());
		updated.insert(updated.end(), newerTicks.begin(), newerTicks.end());
		candidate._ticks.swap(updated);
	}

	if (!candidate._ticks.empty())
	{
		auto cache_time = [sInfo](const WTSTickStruct& tick) {
			return (uint64_t)tick.trading_date * 1000000000ull
				+ (uint64_t)sInfo->offsetTime(tick.action_time / 100000, true) * 100000ull
				+ tick.action_time % 100000;
		};
		if (isEmpty)
		{
			candidate._first_tick_time = cache_time(candidate._ticks.front());
			candidate._last_tick_time = cache_time(candidate._ticks.back());
			pipe_rdmreader_log(_sink, LL_DEBUG, "{} ticks between [{},{}] of {} loaded to cache",
				candidate._ticks.size(), candidate._first_tick_time, candidate._last_tick_time, stdCode);
		}
		else
		{
			if (!olderTicks.empty())
			{
				candidate._first_tick_time = cache_time(candidate._ticks.front());
				pipe_rdmreader_log(_sink, LL_DEBUG, "{} prev ticks of {} loaded to cache", olderTicks.size(), stdCode);
			}
			if (!newerTicks.empty())
			{
				candidate._last_tick_time = cache_time(candidate._ticks.back());
				pipe_rdmreader_log(_sink, LL_DEBUG, "{} newer ticks of {} loaded to cache", newerTicks.size(), stdCode);
			}
		}
	}

	if (candidate._ticks.empty())
		return NULL;
	if (cacheIt == _ticks_cache.end())
		cacheIt = _ticks_cache.emplace(key, std::move(candidate)).first;
	else if (isEmpty || !olderTicks.empty())
	{
		// The old path swapped the vector after loading older ticks.
		cacheIt->second._ticks.swap(candidate._ticks);
		cacheIt->second._first_tick_time = candidate._first_tick_time;
		cacheIt->second._last_tick_time = candidate._last_tick_time;
	}
	else if (!newerTicks.empty())
	{
		// 卡11=B：candidate 已是“旧缓存+新增”完整副本，整体替换缓存缓冲区；
		// 已借出的切片持有旧缓冲区 owner，不再依赖原地追加不重分配。
		cacheIt->second._ticks.swap(candidate._ticks);
		cacheIt->second._first_tick_time = candidate._first_tick_time;
		cacheIt->second._last_tick_time = candidate._last_tick_time;
	}
	TicksList& tickList = cacheIt->second;

	//全部读取完成以后，再生成切片
	{
		//比较时间的对象
		WTSTickStruct sTick, eTick;
		sTick.action_date = lDate;
		sTick.action_time = (uint32_t)(stime % 1000000000);
		eTick.action_date = rDate;
		eTick.action_time = (uint32_t)(etime % 1000000000);

		std::size_t cnt = 0;

		auto lessTick = [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			return a.action_time < b.action_time;
		};
		auto firstTick = std::lower_bound(tickList._ticks.begin(), tickList._ticks.end(), sTick, lessTick);
		auto afterLastTick = std::lower_bound(firstTick, tickList._ticks.end(), eTick, lessTick);
		if (firstTick == afterLastTick)
			return NULL;

		cnt = (std::size_t)(afterLastTick - firstTick);
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, &*firstTick, (uint32_t)cnt);
		if (slice != NULL)
			slice->addOwner(tickList._ticks.owner());
		return slice;
	}
}

WTSKlineSlice* WtRdmDtReaderAD::readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	if (stdCode == NULL)
		return NULL;
	if (count == 0)
		return NULL;
	if (etime == 0)
	{
		uint32_t date = 0, time = 0;
		TimeUtils::getDateTime(date, time);
		etime = (uint64_t)date * 10000 + time / 100000;
	}

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readKlineSliceByCount"))
		return NULL;
	const std::string key = fmtutil::format<64>("{}#{}", stdCode, (uint32_t)period);
	auto cacheIt = _bars_cache.find(key);
	if (cacheIt == _bars_cache.end() || cacheIt->second._bars.empty())
	{
		WtLMDBPtr db = get_k_db(cInfo._exchg, period);
		if (db == NULL)
			return NULL;

		LMDBBarKey lKey(cInfo._exchg, cInfo._code, 0);
		LMDBBarKey rKey(cInfo._exchg, cInfo._code, UINT32_MAX);
		std::vector<WTSBarStruct> loaded;
		MalformedRecord malformed;
		if (!read_checked_range(*db, lKey, rKey, loaded, malformed))
		{
			log_range_failure<WTSBarStruct>(_sink, "bar", cInfo._exchg, cInfo._code, *db, malformed);
			return NULL;
		}
		if (period == KP_DAY)
			wt_ad_daybar::normalize(loaded);	//卡3 3c=C：旧 day 记录兼容读取，time 归一为 0
		if (loaded.empty())
			return NULL;

		BarsList candidate;
		candidate._bars.swap(loaded);
		if (cacheIt == _bars_cache.end())
			cacheIt = _bars_cache.emplace(key, std::move(candidate)).first;
		else
			cacheIt->second = std::move(candidate);
	}

	BarsList& barsList = cacheIt->second;
	if (barsList._bars.empty())
		return NULL;

	const bool isDay = period == KP_DAY;
	const uint32_t endDate = (uint32_t)(etime / 10000);
	const uint32_t endHHMM = (uint32_t)(etime % 10000);
	const uint64_t endTime = isDay ? UINT64_MAX : (uint64_t)(endDate - 19900000) * 10000 + endHHMM;

	auto end = std::upper_bound(barsList._bars.begin(), barsList._bars.end(), endDate,
		[isDay, endTime](uint32_t date, const WTSBarStruct& bar) {
			return isDay ? date < bar.date : endTime < bar.time;
		});
	const std::size_t available = (std::size_t)(end - barsList._bars.begin());
	if (available == 0)
		return NULL;
	const uint32_t actualCount = (uint32_t)std::min<std::size_t>(available, count);
	WTSBarStruct* first = barsList._bars.data() + available - actualCount;
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, first, (int32_t)actualCount);
	if (slice != NULL)
		slice->addOwner(barsList._bars.owner());
	return slice;
}

WTSTickSlice* WtRdmDtReaderAD::readTickSliceByDate(const char* stdCode, uint32_t uDate )
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readTickSliceByDate"))
		return NULL;
	if (uDate == 0)
	{
		const std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);
		wtp::DateMappingClient dateMapping(_base_data_mgr);
		if (!ad_rdm_trading_date(_sink, dateMapping, stdPID.c_str(), 0, 0, uDate))
			return NULL;
	}

	const std::string cacheKey = std::string(stdCode) + "#date#" + std::to_string(uDate);
	auto cacheIt = _ticks_cache.find(cacheKey);
	if (cacheIt == _ticks_cache.end() || cacheIt->second._ticks.empty())
	{
		WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
		if (db == NULL)
			return NULL;

		LMDBHftKey lKey(cInfo._exchg, cInfo._code, uDate, 0);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, uDate, UINT32_MAX);
		std::vector<WTSTickStruct> loaded;
		MalformedRecord malformed;
		if (!read_checked_range(*db, lKey, rKey, loaded, malformed))
		{
			log_range_failure<WTSTickStruct>(_sink, "tick", cInfo._exchg, cInfo._code, *db, malformed);
			return NULL;
		}
		if (loaded.empty())
			return NULL;

		std::stable_sort(loaded.begin(), loaded.end(), [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			return a.action_time < b.action_time;
		});
		TicksList candidate;
		candidate._ticks.swap(loaded);
		if (cacheIt == _ticks_cache.end())
			cacheIt = _ticks_cache.emplace(cacheKey, std::move(candidate)).first;
		else
			cacheIt->second = std::move(candidate);
	}

	TicksList& tickList = cacheIt->second;
	if (tickList._ticks.empty())
		return NULL;
	WTSTickSlice* slice = WTSTickSlice::create(stdCode, tickList._ticks.data(), (uint32_t)tickList._ticks.size());
	if (slice != NULL)
		slice->addOwner(tickList._ticks.owner());
	return slice;
}

WTSKlineSlice* WtRdmDtReaderAD::readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (!accept_key_identity(_sink, cInfo._exchg, cInfo._code, "readKlineSliceByRange"))
		return NULL;
	[[maybe_unused]] WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);

	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	wtp::DateMappingClient dateMapping(_base_data_mgr);
	uint32_t endTDate = 0, beginTDate = 0;
	if (!ad_rdm_trading_date(_sink, dateMapping, stdPID.c_str(), rDate, rTime, endTDate) ||
		!ad_rdm_trading_date(_sink, dateMapping, stdPID.c_str(), lDate, lTime, beginTDate))
		return NULL;
	(void)beginTDate;

	bool isDay = period == KP_DAY;
	//转换成K线时间
	etime = isDay ? endTDate : (etime - 19000000);

	//暂时不考虑HOT之类的，只针对7×24小时品种做一个实现
	std::string key = fmtutil::format<64>("{}#{}", stdCode, (uint32_t)period);
	auto cacheIt = _bars_cache.find(key);
	BarsList candidate;
	if (cacheIt != _bars_cache.end())
		candidate = cacheIt->second;
	const bool isEmpty = candidate._bars.empty();
	bool bNeedNewer = (etime > candidate._last_bar_time);

	//全部重载
	WtLMDBPtr db = get_k_db(cInfo._exchg, period);
	if (db == NULL)
		return NULL;

	std::vector<WTSBarStruct> loaded;
	// R21-BUG-07 (determinable part): the cached bars come from a full scan of one
	// LMDB snapshot. While no transaction has been committed to this environment
	// since that snapshot, a rescan returns identical records (same-key revisions
	// included), so it is skipped. Any commit keeps the historical full rescan:
	// revision visibility is unchanged and the incremental-cursor contract stays
	// pending a decision.
	uint64_t txnBefore = 0;
	const bool txnKnown = lmdb_last_txnid(*db, txnBefore);
	const bool snapshotCurrent = !isEmpty && candidate._snapshot_valid && txnKnown &&
		txnBefore == candidate._snapshot_txnid;
	const bool rescan = isEmpty || (bNeedNewer && !snapshotCurrent);
	if (isEmpty)
	{
		pipe_rdmreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], cInfo._exchg, cInfo._code);
		LMDBBarKey rKey(cInfo._exchg, cInfo._code, 0xffffffff);
		LMDBBarKey lKey(cInfo._exchg, cInfo._code, 0);
		MalformedRecord malformed;
		if (!read_checked_range(*db, lKey, rKey, loaded, malformed))
		{
			log_range_failure<WTSBarStruct>(_sink, "bar", cInfo._exchg, cInfo._code, *db, malformed);
			return NULL;
		}
	}
	else if(rescan)
	{
		//加载更新的数据
		pipe_rdmreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], cInfo._exchg, cInfo._code);
		LMDBBarKey rKey(cInfo._exchg, cInfo._code, 0xffffffff);
		LMDBBarKey lKey(cInfo._exchg, cInfo._code, (uint32_t)candidate._last_bar_time);
		MalformedRecord malformed;
		if (!read_checked_range(*db, lKey, rKey, loaded, malformed))
		{
			log_range_failure<WTSBarStruct>(_sink, "bar", cInfo._exchg, cInfo._code, *db, malformed);
			return NULL;
		}
	}
	if (period == KP_DAY)
		wt_ad_daybar::normalize(loaded);	//卡3 3c=C
	bool snapshotValid = false;
	if (rescan)
	{
		// Record the snapshot only when no commit landed while scanning.
		uint64_t txnAfter = 0;
		snapshotValid = txnKnown && lmdb_last_txnid(*db, txnAfter) && txnAfter == txnBefore;
		candidate._snapshot_valid = snapshotValid;
		candidate._snapshot_txnid = txnBefore;
	}
	if (!loaded.empty())
		candidate._bars.swap(loaded);
	if (!candidate._bars.empty())
	{
		if (cacheIt == _bars_cache.end())
			cacheIt = _bars_cache.emplace(key, std::move(candidate)).first;
		else
		{
			if (isEmpty || !loaded.empty())
			{
				// 卡11=B：原实现整体覆盖缓存内容，这里改为整体替换缓冲区，内容相同；
				// 已借出的切片持有旧缓冲区 owner。
				cacheIt->second._bars.swap(candidate._bars);
			}
			if (rescan)
			{
				cacheIt->second._snapshot_valid = snapshotValid;
				cacheIt->second._snapshot_txnid = txnBefore;
			}
		}
	}
	if (cacheIt == _bars_cache.end())
		return NULL;
	BarsList& barsList = cacheIt->second;

	//
	{
		WTSBarStruct eBar;
		eBar.date = rDate;
		eBar.time = minute_bar_time(rDate, rTime);	// R100-003: no 32-bit wrap from 2033 on

		WTSBarStruct sBar;
		sBar.date = lDate;
		sBar.time = minute_bar_time(lDate, lTime);

		WTSBarStruct* pHead = NULL;
		std::size_t cnt = 0;

		if (barsList._bars.empty())
			return NULL;

		auto lessBar = [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (isDay)
				return a.date < b.date;
			return a.time < b.time;
		};
		auto firstBar = std::lower_bound(barsList._bars.begin(), barsList._bars.end(), sBar, lessBar);
		auto afterLastBar = std::upper_bound(firstBar, barsList._bars.end(), eBar,
			[lessBar](const WTSBarStruct& value, const WTSBarStruct& item) { return lessBar(value, item); });
		if (firstBar == afterLastBar)
			return NULL;

		pHead = &*firstBar;
		cnt = (std::size_t)(afterLastBar - firstBar);

		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, pHead, cnt);
		if (slice != NULL)
			slice->addOwner(barsList._bars.owner());
		return slice;
	}
}

WtRdmDtReaderAD::WtLMDBPtr WtRdmDtReaderAD::get_k_db(const char* exchg, WTSKlinePeriod period)
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
	if (_base_dir.empty())
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Cannot open {} db: base data path is not configured", subdir);
		return WtLMDBPtr();
	}

	auto it = the_map->find(exchg);
	if (it != the_map->end())
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}{}/{}/", _base_dir.c_str(), subdir.c_str(), exchg);
	boost::system::error_code createError;
	fs::create_directories(path, createError);
	if (createError)
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Creating {} db directory {} failed: {}",
			subdir, path, createError.message());
		return WtLMDBPtr();
	}
	if (!dbPtr->open(path.c_str()))
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Opening {} db of {} failed: {}", subdir, exchg, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	else
	{
		pipe_rdmreader_log(_sink, LL_DEBUG, "{} db of {} opened", subdir, exchg);
	}

	(*the_map)[exchg] = dbPtr;
	return dbPtr;
}

WtRdmDtReaderAD::WtLMDBPtr WtRdmDtReaderAD::get_t_db(const char* exchg, const char* code)
{
	if (_base_dir.empty())
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Cannot open tick db: base data path is not configured");
		return WtLMDBPtr();
	}

	std::string key = fmtutil::format<64>("{}.{}", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		return it->second;

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = fmtutil::format("{}ticks/{}/{}", _base_dir.c_str(), exchg, code);
	boost::system::error_code createError;
	fs::create_directories(path, createError);
	if (createError)
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Creating tick db directory {} failed: {}",
			path, createError.message());
		return WtLMDBPtr();
	}
	if (!dbPtr->open(path.c_str()))
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Opening tick db of {}.{} failed: {}", exchg, code, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	else
	{
		pipe_rdmreader_log(_sink, LL_DEBUG, "Tick db of {}.{} opened", exchg, code);
	}

	_tick_dbs[key] = dbPtr;
	return dbPtr;
}
