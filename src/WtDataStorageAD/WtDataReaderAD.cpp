#include "WtDataReaderAD.h"
#include "LMDBKeys.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"

#include <boost/filesystem.hpp>
#include <limits>
namespace fs = boost::filesystem;

namespace {
thread_local LMDBRangeCursorGetOverride tick_query_cursor_get_override = NULL;
thread_local void* tick_query_cursor_get_context = NULL;
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

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->reader_log(ll, buffer);
}

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
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), endTDate);
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
		WtLMDBQuery query(*db);
		WtLMDBReaderTestAccess::set_cursor_get_override(query,
			tick_query_cursor_get_override, tick_query_cursor_get_context);
		if (reload_flag == 1)
		{
			//增量更新，只在完整扫描成功后追加到缓存。
			last_access_time += 1;
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, (uint32_t)(last_access_time / 1000000000), (uint32_t)(last_access_time % 1000000000));
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000));
			cnt = query.get_range_checked(std::string((const char*)&lKey, sizeof(lKey)),
				std::string((const char*)&rKey, sizeof(rKey)), collect_ticks);
		}
		else
		{
			//首次/扩容全量重载，从结束时间向前限量读取。
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
		return WTSTickSlice::create(stdCode, ayTwo.first + (ayTwo.second - count), count);
	}
	else
	{
		//如果array_two条数不够，需要再从array_one提取
		auto ayOne = tickList._ticks.array_one();
		auto diff = count - cnt_2;
		auto ret = WTSTickSlice::create(stdCode, ayOne.first + (ayOne.second - diff), diff);
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
	// LMDBBarKey uses fixed arrays and unchecked strcpy; reject identities that
	// cannot be represented before constructing keys or looking up the database.
	if (exchg == NULL || code == NULL || strlen(exchg) >= MAX_EXCHANGE_LENGTH ||
		strlen(code) >= MAX_INSTRUMENT_LENGTH)
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
	WTSKlinePeriod period, uint32_t& lastBarTime, uint32_t cutoff)
{
	bool isDay = (period == KP_DAY);
	if (lastBarTime > cutoff)
		return true;

	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;

	WtLMDBQuery query(*db);
	LMDBBarKey lKey(exchg, code, lastBarTime);
	LMDBBarKey rKey(exchg, code, cutoff);
	std::vector<WTSBarStruct> newBars;
	bool valuesValid = true;
	int cnt = query.get_range_checked(std::string((const char*)&lKey, sizeof(lKey)),
		std::string((const char*)&rKey, sizeof(rKey)),
		[&newBars, &valuesValid, isDay, cutoff](const ValueArray& ayKeys, const ValueArray& ayVals) {
			if (ayKeys.size() != ayVals.size())
			{
				valuesValid = false;
				return;
			}

			for (std::size_t idx = 0; idx < ayVals.size(); idx++)
			{
				const std::string& item = ayVals[idx];
				const std::string& key = ayKeys[idx];
				if (key.size() != sizeof(LMDBBarKey) || item.size() != sizeof(WTSBarStruct))
				{
					valuesValid = false;
					return;
				}

				WTSBarStruct bar;
				memcpy(&bar, item.data(), sizeof(bar));
				const uint64_t barTime = isDay ? bar.date : bar.time;
				if (barTime <= cutoff)
					newBars.push_back(bar);
			}
		});
	if (cnt < 0 || !valuesValid)
	{
		pipe_reader_log(_sink, LL_ERROR, "Malformed bar key/value in LMDB while updating {}.{} {} bars",
			exchg, code, PERIOD_NAME[period]);
		return false;
	}

	// The checked range query completes before cache state or callbacks are changed.

	uint32_t committedLastBarTime = lastBarTime;
	for (const WTSBarStruct& curBar : newBars)
	{
		uint64_t curBarTime = isDay ? curBar.date : curBar.time;
		if (curBarTime < committedLastBarTime)
			continue;
		if (curBarTime == committedLastBarTime)
		{
			if (barsList._last_from_cache)
			{
				barsList._last_from_cache = false;
				memcpy(&barsList._bars.back(), &curBar, sizeof(WTSBarStruct));
			}
		}
		else
		{
			barsList._bars.push_back(curBar);
			barsList._last_from_cache = false;
			committedLastBarTime = (uint32_t)curBarTime;
			_sink->on_bar(barsList._code.c_str(), barsList._period, &barsList._bars.back());
		}
	}
	lastBarTime = committedLastBarTime;

	pipe_reader_log(_sink, LL_DEBUG, "{} bars of {}.{} updated to {}",
		PERIOD_NAME[period], exchg, code, lastBarTime);
	return true;
}

WTSBarStruct* WtDataReaderAD::get_rt_cache_bar(const char* exchg, const char* code, WTSKlinePeriod period)
{
	RTBarCacheWrapper* wrapper = NULL;
	if (period == KP_DAY)
		wrapper = &_d1_cache;
	else if (period == KP_Minute1)
		wrapper = &_m1_cache;
	else if (period == KP_Minute5)
		wrapper = &_m5_cache;

	[[maybe_unused]] bool isDay = (period == KP_DAY);

	if (wrapper != NULL)
	{
		if (wrapper->empty())
		{
			//如果缓存为空，则加载缓存
			do
			{

				//缓存为空，先自动加载
				std::string filename = _base_dir + wrapper->_filename;
				if (!StdFile::exists(filename.c_str()))
					break;

				wrapper->_file_ptr.reset(new BoostMappingFile);
				wrapper->_file_ptr->map(filename.c_str());
				wrapper->_cache_block = (RTBarCache*)wrapper->_file_ptr->addr();

				wrapper->_cache_block->_size = min(wrapper->_cache_block->_size, wrapper->_cache_block->_capacity);
				wrapper->_last_size = wrapper->_cache_block->_size;

				for (uint32_t i = 0; i < wrapper->_cache_block->_size; i++)
				{
					const BarCacheItem& item = wrapper->_cache_block->_items[i];
					wrapper->_idx[fmtutil::format<64>("{}.{}", item._exchg, item._code)] = i;
				}
			} while (false);
		}
		else
		{
			//如果缓存不为空，检查一下有没有新的合约进来，如果有的话，就把索引更新一下
			if (wrapper->_last_size != wrapper->_cache_block->_size)
			{
				for (uint32_t i = wrapper->_last_size; i < wrapper->_cache_block->_size; i++)
				{
					const BarCacheItem& item = wrapper->_cache_block->_items[i];
					wrapper->_idx[fmtutil::format<64>("{}.{}", item._exchg, item._code)] = i;
				}
			}
		}

		//最后看缓存里有没有改合约对应的K线缓存
		auto it = wrapper->_idx.find(fmtutil::format<64>("{}.{}", exchg, code));
		if (it != wrapper->_idx.end())
		{
			return &wrapper->_cache_block->_items[it->second]._bar;
		}
	}

	return NULL;
}

WTSKlineSlice* WtDataReaderAD::readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	if (count == 0)
	{
		// Preserve the established empty-slice shape only when this reader already
		// owns a cache buffer. A first zero-count read has no backing block and
		// historically returns NULL.
		const std::string zeroCountKey = fmtutil::format<64>("{}#{}", stdCode, (uint32_t)period);
		auto cached = _bars_cache.find(zeroCountKey);
		if (cached == _bars_cache.end() || cached->second._bars.empty())
			return NULL;
	}

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), endTDate);
		//else if (cInfo.isHot())
		//	curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
		//else if (cInfo.isSecond())
		//	curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);
	}

	const bool isDay = (period == KP_DAY);
	const uint32_t cutoffBarTime = isDay
		? endTDate
		: (curDate - 19900000u) * 10000u + curTime;
	if (count > static_cast<uint32_t>((std::numeric_limits<int>::max)()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Kline request count {} for {} exceeds the supported limit", count, stdCode);
		return NULL;
	}

	std::string key = fmtutil::format<64>("{}#{}", stdCode, (uint32_t)period);
	BarsList& barsList = _bars_cache[key];
	const bool cachedFutureBar = !barsList._bars.empty() &&
		(isDay ? barsList._bars.back().date : barsList._bars.back().time) > cutoffBarTime;
	const bool cutoffMovedBack = barsList._last_req_time != 0 && cutoffBarTime < barsList._last_req_time;
	if (count > 0 && (barsList._bars.capacity() < count ||
		(barsList._bars.empty() && barsList._last_req_time == 0) ||
		cutoffMovedBack || cachedFutureBar))
	{
		// Capacity growth or a cutoff behind the current cache needs a bounded reload.
		if (!cacheBarsFromStorage(key, stdCode, period, count, cutoffBarTime))
			return NULL;
	}
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
			lastBarTime, cutoffBarTime))
			return NULL;
		lastBarTime = barsList._bars.empty()
			? 0
			: (isDay ? barsList._bars.back().date : (uint32_t)barsList._bars.back().time);
	}

	// RT cache can advance between reads with the same cutoff; inspect it on every call.
	WTSBarStruct* rtBar = get_rt_cache_bar(cInfo._exchg, curCode.c_str(), period);
	if (rtBar != NULL)
	{
		uint64_t cacheBarTime = isDay ? rtBar->date : rtBar->time;
		if (cacheBarTime > etime)
		{
			// The writer may have flushed this bar while the RT cache was read.
			if (lastBarTime < etime && !update_cache_from_lmdb(barsList, cInfo._exchg,
				curCode.c_str(), period, lastBarTime, cutoffBarTime))
				return NULL;
		}
		else if (cacheBarTime > lastBarTime)
		{
			barsList._bars.push_back(*rtBar);
			barsList._last_from_cache = true;
			pipe_reader_log(_sink, LL_DEBUG,
				"{} bars at {} of {} updated from cache instead of lmdb in {}",
				PERIOD_NAME[period], etime, stdCode, __FUNCTION__);
		}
		else if (cacheBarTime == lastBarTime && barsList._last_from_cache && !barsList._bars.empty())
		{
			memcpy(&barsList._bars.back(), rtBar, sizeof(WTSBarStruct));
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
		return WTSKlineSlice::create(stdCode, period, 1, ayTwo.first + (ayTwo.second - count), count);
	}
	else
	{
		//如果array_two条数不够，需要再从array_one提取
		auto ayOne = barsList._bars.array_one();
		auto diff = count - cnt_2;
		auto ret = WTSKlineSlice::create(stdCode, period, 1, ayOne.first + (ayOne.second - diff), diff);
		if (ret == NULL)
			return NULL;
		if (cnt_2 > 0 && !ret->appendBlock(ayTwo.first, cnt_2))
		{
			ret->release();
			return NULL;
		}
		return ret;
	}


	return NULL;
}

void WtDataReaderAD::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */)
{
	//这里应该触发检查
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	if (nowTime <= _last_time)
		return;

	const uint64_t minuteEndBarTime = (uDate - 19900000) * 10000 + uTime;
	const uint32_t dailyEndDate = endTDate;

	for (auto it = _bars_cache.begin(); it != _bars_cache.end(); it++)
	{
		BarsList& barsList = (BarsList&)it->second;
		if (barsList._bars.empty())
			continue;

		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(barsList._code.c_str(), _hot_mgr);
		if (barsList._period != KP_DAY)
		{
			uint32_t lastBarTime = (uint32_t)barsList._bars.back().time;
			pipe_reader_log(_sink, LL_DEBUG,
				"Updating {} bars of {} in section ({},{}]", PERIOD_NAME[barsList._period], barsList._code, lastBarTime, minuteEndBarTime);
			if (!update_cache_from_lmdb(barsList, barsList._exchg.c_str(), cInfo._code,
				barsList._period, lastBarTime, (uint32_t)minuteEndBarTime))
				continue;
			if(lastBarTime < minuteEndBarTime)
			{
				WTSBarStruct* rtBar = get_rt_cache_bar(cInfo._exchg, cInfo._code, barsList._period);
				if (rtBar != NULL && rtBar->time > lastBarTime && rtBar->time <= minuteEndBarTime)
				{
					barsList._bars.push_back(*rtBar);
					barsList._last_from_cache = true;
					_sink->on_bar(barsList._code.c_str(), barsList._period, rtBar);
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @ {} of {} updated from cache instead of lmdb in {}", PERIOD_NAME[barsList._period], minuteEndBarTime, barsList._code, __FUNCTION__);
				}
			}
		}
		else if(endTDate != 0)
		{
			uint32_t lastBarTime = barsList._bars.back().date;
			if (!update_cache_from_lmdb(barsList, barsList._exchg.c_str(), cInfo._code,
				barsList._period, lastBarTime, dailyEndDate))
				continue;
			if (lastBarTime < dailyEndDate)
			{
				WTSBarStruct* rtBar = get_rt_cache_bar(cInfo._exchg, cInfo._code, barsList._period);
				if (rtBar != NULL && rtBar->date > lastBarTime && rtBar->date <= dailyEndDate)
				{
					barsList._bars.push_back(*rtBar);
					barsList._last_from_cache = true;
					_sink->on_bar(barsList._code.c_str(), barsList._period, rtBar);
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @  of {} updated from cache instead of lmdb in {}", PERIOD_NAME[barsList._period], dailyEndDate, barsList._code, __FUNCTION__);
				}
			}
		}
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
