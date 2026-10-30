#include "WtRdmDtReaderAD.h"
#include "LMDBKeys.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void pipe_rdmreader_log(IRdmDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
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
	if (count == 0)
		return NULL;
	if (etime == 0)
	{
		uint32_t date = 0, time = 0;
		TimeUtils::getDateTime(date, time);
		etime = (uint64_t)date * 1000000000ull + time;
	}

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	const std::string cacheKey = std::string(stdCode) + "#count-history";
	TicksList& tickList = _ticks_cache[cacheKey];
	if (tickList._ticks.empty())
	{
		WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
		if (db == NULL)
			return NULL;

		WtLMDBQuery query(*db);
		LMDBHftKey lKey(cInfo._exchg, cInfo._code, 0, 0);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, UINT32_MAX, UINT32_MAX);
		query.get_range(std::string((const char*)&lKey, sizeof(lKey)),
			std::string((const char*)&rKey, sizeof(rKey)), [&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			tickList._ticks.resize(ayVals.size());
			for (std::size_t i = 0; i < ayVals.size(); i++)
				memcpy(&tickList._ticks[i], ayVals[i].data(), ayVals[i].size());
		});
		std::stable_sort(tickList._ticks.begin(), tickList._ticks.end(), [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			return a.action_time < b.action_time;
		});
	}

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
	return WTSTickSlice::create(stdCode, first, actualCount);
}

WTSTickSlice* WtRdmDtReaderAD::readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), rDate, rTime, false);
	uint32_t beginTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), lDate, lTime, false);

	std::string key = stdCode;

	//先检查缓存
	TicksList& tickList = _ticks_cache[key];

	bool isEmpty = tickList._ticks.empty();
	bool bNeedOlder = stime < tickList._first_tick_time;
	bool bNeedNewer = etime > tickList._last_tick_time;

	//这里要改成从lmdb读取
	WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
	if (db == NULL)
		return NULL;

	if (isEmpty)
	{
		//按照区间加载即可
		WtLMDBQuery query(*db);
		LMDBHftKey lKey(cInfo._exchg, cInfo._code, beginTDate, lTime * 100000 + lSecs);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, endTDate, rTime * 100000 + rSecs);
		int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)),
			std::string((const char*)&rKey, sizeof(rKey)), [&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			tickList._ticks.resize(ayVals.size());
			std::size_t idx = 0;
			for (const std::string& item : ayVals)
			{
				memcpy(&tickList._ticks[idx], item.data(), item.size());
				idx++;
			}
		});

		if (cnt > 0)
		{
			const WTSTickStruct& firstTick = tickList._ticks.front();
			const WTSTickStruct& lastTick = tickList._ticks.back();
			tickList._first_tick_time = (uint64_t)firstTick.trading_date * 1000000000ull
				+ (uint64_t)sInfo->offsetTime(firstTick.action_time / 100000, true) * 100000ull
				+ firstTick.action_time % 100000;
			tickList._last_tick_time = (uint64_t)lastTick.trading_date * 1000000000ull
				+ (uint64_t)sInfo->offsetTime(lastTick.action_time / 100000, true) * 100000ull
				+ lastTick.action_time % 100000;

			pipe_rdmreader_log(_sink, LL_DEBUG, "{} ticks between [{},{}] of {} loaded to cache", cnt, tickList._first_tick_time, tickList._last_tick_time, stdCode);
		}
	}
	else
	{
		if (bNeedOlder)
		{
			//读取更早的数据
			WtLMDBQuery query(*db);
			uint32_t firstKeyDate = (uint32_t)(tickList._first_tick_time / 1000000000ull);
			uint32_t firstKeyTime = (uint32_t)(tickList._first_tick_time % 1000000000ull);
			if (firstKeyTime > 0)
				--firstKeyTime;
			else if (firstKeyDate > 0)
			{
				--firstKeyDate;
				firstKeyTime = UINT32_MAX;
			}
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, firstKeyDate, firstKeyTime);
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, beginTDate, lTime * 100000 + lSecs);
			int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
				[&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
				std::vector<WTSTickStruct> ayTicks;
				ayTicks.resize(ayVals.size() + tickList._ticks.size());
				std::size_t idx = 0;
				for (const std::string& item : ayVals)
				{
					[[maybe_unused]] WTSTickStruct* curTick = (WTSTickStruct*)item.data();
					memcpy(&ayTicks[idx], item.data(), item.size());
					idx++;
				}

				//将原来的数据拷贝到后面，再做一个swap即可
				memcpy(&ayTicks[idx], tickList._ticks.data(), sizeof(WTSTickStruct)*tickList._ticks.size());
				tickList._ticks.swap(ayTicks);
			});

			if(cnt > 0)
			{
				const WTSTickStruct& curTs = tickList._ticks.front();
				tickList._first_tick_time = (uint64_t)curTs.trading_date * 1000000000ull
					+ (uint64_t)sInfo->offsetTime(curTs.action_time / 100000, true) * 100000ull
					+ curTs.action_time % 100000;

				pipe_rdmreader_log(_sink, LL_DEBUG, "{} prev ticks of {} loaded to cache", cnt, stdCode);
			}
		}

		if (bNeedNewer)
		{
			//读取更新的数据
			WtLMDBQuery query(*db);
			uint32_t lastKeyDate = (uint32_t)(tickList._last_tick_time / 1000000000ull);
			uint32_t lastKeyTime = (uint32_t)(tickList._last_tick_time % 1000000000ull);
			if (lastKeyTime < UINT32_MAX)
				++lastKeyTime;
			else
			{
				++lastKeyDate;
				lastKeyTime = 0;
			}
			LMDBHftKey lKey(cInfo._exchg, cInfo._code, lastKeyDate, lastKeyTime);
			LMDBHftKey rKey(cInfo._exchg, cInfo._code, endTDate, rTime * 100000 + rSecs);
			int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
				[&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
				for (const std::string& item : ayVals)
				{
					WTSTickStruct* curTick = (WTSTickStruct*)item.data();
					tickList._ticks.emplace_back(*curTick);
				}
			});

			if (cnt > 0)
			{
				const WTSTickStruct& curTs = tickList._ticks.back();
				tickList._last_tick_time = (uint64_t)curTs.trading_date * 1000000000ull
					+ (uint64_t)sInfo->offsetTime(curTs.action_time / 100000, true) * 100000ull
					+ curTs.action_time % 100000;

				pipe_rdmreader_log(_sink, LL_DEBUG, "{} newer ticks of {} loaded to cache", cnt, stdCode);
			}
		}
	}

	//全部读取完成以后，再生成切片
	{
		//比较时间的对象
		WTSTickStruct sTick, eTick;
		sTick.action_date = lDate;
		sTick.action_time = (uint32_t)(stime % 1000000000);
		eTick.action_date = rDate;
		eTick.action_time = (uint32_t)(etime % 1000000000);

		std::size_t cnt = 0;

		if (tickList._ticks.empty())
			return NULL;

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
		return slice;
	}
}

WTSKlineSlice* WtRdmDtReaderAD::readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	if (count == 0)
		return NULL;
	if (etime == 0)
	{
		uint32_t date = 0, time = 0;
		TimeUtils::getDateTime(date, time);
		etime = (uint64_t)date * 10000 + time / 100000;
	}

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	const std::string key = fmtutil::format<64>("{}#{}", stdCode, (uint32_t)period);
	BarsList& barsList = _bars_cache[key];
	if (barsList._bars.empty())
	{
		WtLMDBPtr db = get_k_db(cInfo._exchg, period);
		if (db == NULL)
			return NULL;

		WtLMDBQuery query(*db);
		LMDBBarKey lKey(cInfo._exchg, cInfo._code, 0);
		LMDBBarKey rKey(cInfo._exchg, cInfo._code, UINT32_MAX);
		query.get_range(std::string((const char*)&lKey, sizeof(lKey)),
			std::string((const char*)&rKey, sizeof(rKey)), [&barsList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			barsList._bars.resize(ayVals.size());
			for (std::size_t i = 0; i < ayVals.size(); i++)
				memcpy(&barsList._bars[i], ayVals[i].data(), ayVals[i].size());
		});
	}

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
	return WTSKlineSlice::create(stdCode, period, 1, first, (int32_t)actualCount);
}

WTSTickSlice* WtRdmDtReaderAD::readTickSliceByDate(const char* stdCode, uint32_t uDate )
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (uDate == 0)
	{
		const std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);
		uDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), 0, 0, false);
	}

	const std::string cacheKey = std::string(stdCode) + "#date#" + std::to_string(uDate);
	TicksList& tickList = _ticks_cache[cacheKey];
	if (tickList._ticks.empty())
	{
		WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
		if (db == NULL)
			return NULL;

		WtLMDBQuery query(*db);
		LMDBHftKey lKey(cInfo._exchg, cInfo._code, uDate, 0);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, uDate, UINT32_MAX);
		query.get_range(std::string((const char*)&lKey, sizeof(lKey)),
			std::string((const char*)&rKey, sizeof(rKey)), [&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			tickList._ticks.resize(ayVals.size());
			for (std::size_t i = 0; i < ayVals.size(); i++)
				memcpy(&tickList._ticks[i], ayVals[i].data(), ayVals[i].size());
		});
		std::stable_sort(tickList._ticks.begin(), tickList._ticks.end(), [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			return a.action_time < b.action_time;
		});
	}

	if (tickList._ticks.empty())
		return NULL;
	return WTSTickSlice::create(stdCode, tickList._ticks.data(), (uint32_t)tickList._ticks.size());
}

WTSKlineSlice* WtRdmDtReaderAD::readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	[[maybe_unused]] WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);

	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), rDate, rTime, false);
	[[maybe_unused]] uint32_t beginTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), lDate, lTime, false);

	bool isDay = period == KP_DAY;
	//转换成K线时间
	etime = isDay ? endTDate : (etime - 19000000);

	//暂时不考虑HOT之类的，只针对7×24小时品种做一个实现
	std::string key = fmtutil::format<64>("{}#{}", stdCode, (uint32_t)period);
	BarsList& barsList = _bars_cache[key];

	bool bNeedNewer = (etime > barsList._last_bar_time);

	//全部重载
	WtLMDBPtr db = get_k_db(cInfo._exchg, period);
	if (db == NULL)
		return NULL;

	if (barsList._bars.empty())
	{
		pipe_rdmreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], cInfo._exchg, cInfo._code);
		WtLMDBQuery query(*db);
		LMDBBarKey rKey(cInfo._exchg, cInfo._code, 0xffffffff);
		LMDBBarKey lKey(cInfo._exchg, cInfo._code, 0);
		[[maybe_unused]] int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
			[&barsList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			if (ayVals.empty())
				return;

			std::size_t cnt = ayVals.size();
			barsList._bars.resize(cnt);
			std::size_t idx = 0;
			for (const std::string& item : ayVals)
			{
				memcpy(&barsList._bars[idx], item.data(), item.size());
				idx++;
			}
		});
	}
	else if(bNeedNewer)
	{
		//加载更新的数据
		pipe_rdmreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], cInfo._exchg, cInfo._code);
		WtLMDBQuery query(*db);
		LMDBBarKey rKey(cInfo._exchg, cInfo._code, 0xffffffff);
		LMDBBarKey lKey(cInfo._exchg, cInfo._code, (uint32_t)barsList._last_bar_time);
		[[maybe_unused]] int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
			[&barsList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals) {
			if (ayVals.empty())
				return;

			std::size_t cnt = ayVals.size();
			barsList._bars.resize(cnt);
			std::size_t idx = 0;
			for (const std::string& item : ayVals)
			{
				memcpy(&barsList._bars[idx], item.data(), item.size());
				idx++;
			}
		});
	}

	//
	{
		WTSBarStruct eBar;
		eBar.date = rDate;
		eBar.time = (rDate - 19900000) * 10000 + rTime;

		WTSBarStruct sBar;
		sBar.date = lDate;
		sBar.time = (lDate - 19900000) * 10000 + lTime;

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
		pipe_rdmreader_log(_sink, LL_ERROR, "Opening {} db if {} failed: {}", subdir, exchg, dbPtr->errmsg());
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
