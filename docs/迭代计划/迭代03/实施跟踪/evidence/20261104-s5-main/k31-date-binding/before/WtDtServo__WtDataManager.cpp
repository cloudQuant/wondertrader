/*!
 * \file WtDataManager.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtDataManager.h"
#include "WtDtRunner.h"
#include "WtHelper.h"

#include <memory>

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/DLLHelper.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"


WTSDataFactory g_dataFact;

#if defined(WT_P12_BAR_LIFETIME_TEST_HOOKS)
namespace
{
thread_local wtp::WtDataManagerBarLifetimeHook g_wt_dm_bar_lifetime_hook = NULL;

void notify_wt_dm_bar_lifetime(uintptr_t address, wtp::WtDataManagerBarLifetimeEvent event)
{
	if (g_wt_dm_bar_lifetime_hook != NULL)
		g_wt_dm_bar_lifetime_hook(address, event);
}
}

void wtp::setWtDataManagerBarLifetimeHook(wtp::WtDataManagerBarLifetimeHook hook)
{
	g_wt_dm_bar_lifetime_hook = hook;
}
#endif

WtDataManager::WtDataManager()
	: _reader(NULL)
	, _remover(NULL)
	, _bd_mgr(NULL)
	, _hot_mgr(NULL)
	, _runner(NULL)
	, _rt_bars(NULL)
{
}


WtDataManager::~WtDataManager()
{
	//卡11=B / R123：先释放自有缓存（只释放缓存持有的引用，外部切片通过 owner 保活），
	//再释放实时 K 线 map，最后通过模块提供的 remover 销毁 reader。
	//reader 所在模块不在此卸载：仍被外部持有的切片的 owner 析构代码可能位于该模块。
	{
		StdUniqueLock lock(_mtx_cache);
		release_bar_caches();
	}

	{
		StdUniqueLock lock(_mtx_rtbars);
		if (_rt_bars != NULL)
		{
			_rt_bars->release();
			_rt_bars = NULL;
		}
	}

	if (_reader != NULL)
	{
		if (_remover != NULL)
			_remover(_reader);
		_reader = NULL;
		_remover = NULL;
	}
}

void WtDataManager::release_bar_caches()
{
	for(auto& m : _bars_cache)
	{
		if (m.second._bars != NULL)
			m.second._bars->release();
		m.second._bars = NULL;
	}
	_bars_cache.clear();
}

void WtDataManager::detach_bar_cache(BarCache& barCache)
{
	if (barCache._bars == NULL || barCache._bars->isSingleRefs())
		return;

	WTSKlineData* oldBars = barCache._bars;
	WTSKlineData* newBars = WTSKlineData::create(oldBars->code(), 0);
	newBars->setPeriod(oldBars->period(), oldBars->times());
	newBars->setClosed(oldBars->isClosed());
	newBars->setUnixTime(oldBars->isUnixTime());
	newBars->getDataRef() = oldBars->getDataRef();
	barCache._bars = newBars;
	oldBars->release();
}

bool WtDataManager::initStore(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	std::string module = cfg->getCString("module");
	if (module.empty())
		module = "WtDataStorage";

	module = WtHelper::get_module_path(DLLHelper::wrap_module(module.c_str()));
	DllHandle libParser = DLLHelper::load_library(module.c_str());
	if (libParser)
	{
		FuncCreateRdmDtReader pFuncCreateReader = (FuncCreateRdmDtReader)DLLHelper::get_symbol(libParser, "createRdmDtReader");
		if (pFuncCreateReader == NULL)
		{
			WTSLogger::error("Initializing of random data reader failed: function createRdmDtReader not found...");
		}

		FuncDeleteRdmDtReader pFuncDeleteReader = (FuncDeleteRdmDtReader)DLLHelper::get_symbol(libParser, "deleteRdmDtReader");
		if (pFuncDeleteReader == NULL)
		{
			WTSLogger::error("Initializing of random data reader failed: function deleteRdmDtReader not found...");
		}

		if (pFuncCreateReader && pFuncDeleteReader)
		{
			_reader = pFuncCreateReader();
			_remover = pFuncDeleteReader;
		}

	}
	else
	{
		WTSLogger::error("Initializing of random data reader failed: loading module {} failed...", module);
		return false;
	}

	if (_reader == NULL)
	{
		WTSLogger::error("Initializing of random data reader failed: create/delete reader functions are unavailable or reader creation failed");
		DLLHelper::free_library(libParser);
		_remover = NULL;
		return false;
	}

	_reader->init(cfg, this);
	return true;
}

bool WtDataManager::init(WTSVariant* cfg, WtDtRunner* runner)
{
	if (cfg == NULL)
		return false;

	_runner = runner;
	if (_runner)
	{
		_bd_mgr = &_runner->getBaseDataMgr();
		_hot_mgr = &_runner->getHotMgr();
	}

	_align_by_section = cfg->getBoolean("align_by_section");

	WTSLogger::info("Resampled bars will be aligned by section: {}", _align_by_section ? "yes" : " no");

	return initStore(cfg->get("store"));
}

void WtDataManager::reader_log(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}

WTSTickSlice* WtDataManager::get_tick_slices_by_range(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readTickSliceByRange(stdCode, stime, etime);
}

WTSTickSlice* WtDataManager::get_tick_slice_by_date(const char* stdCode, uint32_t uDate /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	return _reader->readTickSliceByDate(stdCode, uDate);
}

WTSOrdQueSlice* WtDataManager::get_order_queue_slice(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readOrdQueSliceByRange(stdCode, stime, etime);
}

WTSOrdDtlSlice* WtDataManager::get_order_detail_slice(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readOrdDtlSliceByRange(stdCode, stime, etime);
}

WTSTransSlice* WtDataManager::get_transaction_slice(const char* stdCode,uint64_t stime, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	stime = stime * 100000;
	etime = etime * 100000;
	return _reader->readTransSliceByRange(stdCode, stime, etime);
}

WTSSessionInfo* WtDataManager::get_session_info(const char* sid, bool isCode /* = false */)
{
	if (_bd_mgr == NULL)
		return NULL;

	if (!isCode)
		return _bd_mgr->getSession(sid);

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, _hot_mgr);
	WTSCommodityInfo* cInfo = _bd_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

WTSKlineSlice* WtDataManager::get_skline_slice_by_date(const char* stdCode, uint32_t secs, uint32_t uDate /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	std::string key = fmtutil::format<64>("{}-{}-s{}", stdCode, uDate, secs);

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	StdUniqueLock cacheLock(_mtx_cache);
	BarCache& barCache = _bars_cache[key];
	barCache._period = KP_Tick;
	barCache._times = secs;
	if (barCache._bars == NULL)
	{
		//第一次将全部数据缓存到内存中
		WTSTickSlice* ticks = _reader->readTickSliceByDate(stdCode, uDate);
		if (ticks != NULL)
		{
			WTSKlineData* kData = g_dataFact.extractKlineData(ticks, secs, sInfo, true);
			barCache._bars = kData;		
			ticks->release();
		}
		else
		{
			return NULL;
		}
	}
	
	if (barCache._bars == NULL)
		return NULL;

	WTSBarStruct* rtHead = barCache._bars->at(0);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, KP_Tick, secs, rtHead, barCache._bars->size());
	//卡11=B：切片持有缓存 K 线对象，clear_cache/析构只释放缓存自己的引用
	if (slice != NULL)
		slice->addOwner(makeSliceOwner(barCache._bars));
	return slice;
}

WTSKlineSlice* WtDataManager::get_kline_slice_by_date(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t uDate /* = 0 */)
{
	if (stdCode == NULL || *stdCode == '\0' || _reader == NULL ||
		_runner == NULL || _bd_mgr == NULL || _hot_mgr == NULL ||
		_bd_mgr != &_runner->getBaseDataMgr() || _hot_mgr != &_runner->getHotMgr())
		return NULL;

	// Snapshot the current date once so both boundaries use the same day.
	if (uDate == 0)
	{
		uint32_t clockTime = 0;
		if (!TimeUtils::tryGetDateTime(uDate, clockTime))
			return NULL;
	}

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	DateMappingV1 mapping = _runner->getBaseDataMgr().getDateMappingV1();
	if (mapping.size != sizeof(DateMappingV1) || mapping.version != 1 ||
		mapping.context != &_runner->getBaseDataMgr() || mapping.getBoundaryTime == NULL)
		return NULL;
	const DateMappingResult64 start = mapping.getBoundaryTime(&mapping, codeInfo.stdCommID(), uDate, 0, 1);
	const DateMappingResult64 end = mapping.getBoundaryTime(&mapping, codeInfo.stdCommID(), uDate, 0, 0);
	if (start.status != DateMappingStatus::Ok || end.status != DateMappingStatus::Ok ||
		start.value == 0 || end.value == 0 || start.value > end.value)
		return NULL;
	return get_kline_slice_by_range(stdCode, period, times, start.value, end.value);
}

WTSKlineSlice* WtDataManager::get_kline_slice_by_range(const char* stdCode, WTSKlinePeriod period, uint32_t times,uint64_t stime, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	if (times == 1)
	{
		return _reader->readKlineSliceByRange(stdCode, period, stime, etime);
	}

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	std::string key = fmtutil::format<64>("{}-{}-{}", stdCode, (uint32_t)period, times);
	StdUniqueLock cacheLock(_mtx_cache);
	BarCache& barCache = _bars_cache[key];
	barCache._period = period;
	barCache._times = times;
	if(barCache._bars == NULL)
	{
		//第一次将全部数据缓存到内存中
		WTSKlineSlice* rawData = _reader->readKlineSliceByCount(stdCode, period, UINT_MAX, 0);
		if (rawData != NULL)
		{
			WTSKlineData* kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, false);
			barCache._bars = kData;

			//不管如何，都删除最后一条K线
			//不能通过闭合标记判断，因为读取的基础周期可能本身没有闭合
			if (barCache._bars->size() > 0)
			{
				auto& bars = barCache._bars->getDataRef();
				bars.erase(bars.begin() + bars.size() - 1, bars.end());
			}

			if (period == KP_DAY)
				barCache._last_bartime = kData->date(-1);
			else
			{
				uint64_t lasttime = kData->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}

			rawData->release();
		}
		else
		{
			return NULL;
		}
	}
	else
	{
		//后面则增量更新
		WTSKlineSlice* rawData = _reader->readKlineSliceByRange(stdCode, period, barCache._last_bartime, 0);
		if (rawData != NULL)
		{
			//卡11=B：已借出的切片引用旧对象，增量更新前写时复制
			detach_bar_cache(barCache);
			for(int32_t idx = 0; idx < rawData->size(); idx ++)
			{
				WTSBarStruct* rawBar = rawData->at(idx);
				uint64_t barTime = 0;
				if (period == KP_DAY)
					barTime = rawBar->date;
				else
					barTime = 199000000000 + rawBar->time;
				
				//只有时间上次记录的最后一条时间，才可以用于更新K线
				if(barTime <= barCache._last_bartime)
					continue;

				g_dataFact.updateKlineData(barCache._bars, rawBar, sInfo, _align_by_section);
			}

			//不管如何，都删除最后一条K线
			//不能通过闭合标记判断，因为读取的基础周期可能本身没有闭合
			if(barCache._bars->size() > 0)
			{
				auto& bars = barCache._bars->getDataRef();
				bars.erase(bars.begin() + bars.size() - 1, bars.end());
			}

			if (period == KP_DAY)
				barCache._last_bartime = barCache._bars->date(-1);
			else
			{
				uint64_t lasttime = barCache._bars->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}
			

			rawData->release();
		}
	}

	//最后到缓存中定位
	bool isDay = period == KP_DAY;
	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;

	WTSBarStruct sBar;
	sBar.date = lDate;
	sBar.time = (lDate - 19900000) * 10000 + lTime;

	uint32_t eIdx, sIdx;
	auto& bars = barCache._bars->getDataRef();
	auto eit = std::lower_bound(bars.begin(), bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	if (eit == bars.end())
		eIdx = bars.size() - 1;
	else
	{
		if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
		{
			eit--;
		}

		eIdx = eit - bars.begin();
	}

	auto sit = std::lower_bound(bars.begin(), eit, sBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});
	sIdx = sit - bars.begin();
	uint32_t rtCnt = eIdx - sIdx + 1;
	WTSBarStruct* rtHead = barCache._bars->at(sIdx);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	if (slice != NULL)
		slice->addOwner(makeSliceOwner(barCache._bars));
	return slice;
}

WTSKlineSlice* WtDataManager::get_kline_slice_by_count(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	if (times == 1)
	{
		return _reader->readKlineSliceByCount(stdCode, period, count, etime);
	}

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	std::string key = fmtutil::format<64>("{}-{}-{}", stdCode, (uint32_t)period, times);
	StdUniqueLock cacheLock(_mtx_cache);
	BarCache& barCache = _bars_cache[key];
	barCache._period = period;
	barCache._times = times;

	const char* tag = PERIOD_NAME[period-KP_Tick];

	if (barCache._bars == NULL)
	{
		//第一次将全部数据缓存到内存中
		WTSLogger::info("Caching all {} bars of {}...", tag, stdCode);
		WTSKlineSlice* rawData = _reader->readKlineSliceByCount(stdCode, period, UINT_MAX, 0);
		if (rawData != NULL)
		{
			WTSLogger::info("Resampling {} {} bars by {}-TO-1 of {}...", rawData->size(), tag, times, stdCode);
			WTSKlineData* kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true);
			barCache._bars = kData;

			//如果不是日线，要考虑最后一条K线是否闭合的情况
			//这里采用保守的方案，如果本地时间大于最后一条K线的时间，则认为真正闭合了
			if (period != KP_DAY)
			{
				uint64_t last_bartime = 0;
				last_bartime = 199000000000 + kData->time(-1);

				uint64_t now = TimeUtils::getYYYYMMDDhhmmss() / 100;
				if (now <= last_bartime && barCache._bars->size() > 0)
				{
					auto& bars = barCache._bars->getDataRef();
					bars.erase(bars.begin() + bars.size() - 1, bars.end());
				}
			}


			if (period == KP_DAY)
				barCache._last_bartime = kData->date(-1);
			else
			{
				uint64_t lasttime = kData->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}

			rawData->release();
		}
		else
		{
			return NULL;
		}
	}
	else
	{
		//后面则增量更新
		WTSKlineSlice* rawData = _reader->readKlineSliceByRange(stdCode, period, barCache._last_bartime, 0);
		if (rawData != NULL)
		{
			//卡11=B：已借出的切片引用旧对象，增量更新前写时复制
			detach_bar_cache(barCache);
			WTSLogger::info("{} {} bars of {} updated, adding to cache...", rawData->size(), tag, stdCode);
			for (int32_t idx = 0; idx < rawData->size(); idx++)
			{
				WTSBarStruct* rawBar = rawData->at(idx);
				uint64_t barTime = 0;
				if (period == KP_DAY)
					barTime = rawBar->date;
				else
					barTime = 199000000000 + rawBar->time;

				//只有时间上次记录的最后一条时间，才可以用于更新K线
				if (barTime <= barCache._last_bartime)
					continue;

				g_dataFact.updateKlineData(barCache._bars, rawBar, sInfo, _align_by_section);
			}

			//如果不是日线，要考虑最后一条K线是否闭合的情况
			//这里采用保守的方案，如果本地时间大于最后一条K线的时间，则认为真正闭合了
			if (period != KP_DAY)
			{
				uint64_t last_bartime = 0;
				last_bartime = 199000000000 + barCache._bars->time(-1);

				uint64_t now = TimeUtils::getYYYYMMDDhhmmss() / 100;
				if (now <= last_bartime && barCache._bars->size() > 0)
				{
					auto& bars = barCache._bars->getDataRef();
					bars.erase(bars.begin() + bars.size() - 1, bars.end());
				}
			}

			if (period == KP_DAY)
				barCache._last_bartime = barCache._bars->date(-1);
			else
			{
				uint64_t lasttime = barCache._bars->time(-1);
				barCache._last_bartime = 199000000000 + lasttime;
			}


			rawData->release();
		}
	}

	//最后到缓存中定位
	bool isDay = period == KP_DAY;
	uint32_t rDate, rTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);

	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;

	uint32_t eIdx, sIdx;
	auto& bars = barCache._bars->getDataRef();
	auto eit = std::lower_bound(bars.begin(), bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	if (eit == bars.end())
		eIdx = bars.size() - 1;
	else
	{
		if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
		{
			eit--;
		}

		eIdx = eit - bars.begin();
	}

	sIdx = (eIdx + 1 >= count) ? (eIdx + 1 - count) : 0;
	uint32_t rtCnt = eIdx - sIdx + 1;
	WTSBarStruct* rtHead = barCache._bars->at(sIdx);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	if (slice != NULL)
		slice->addOwner(makeSliceOwner(barCache._bars));
	return slice;
}

WTSTickSlice* WtDataManager::get_tick_slice_by_count(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_reader == NULL)
		return NULL;

	etime = etime * 100000;
	return _reader->readTickSliceByCount(stdCode, count, etime);
}

double WtDataManager::get_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo /* = NULL */)
{
	if (commInfo == NULL || _reader == NULL)
		return 1.0;

	if (commInfo->isStock())
		return _reader->getAdjFactorByDate(stdCode, 0);
	else
	{
		const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
		if (strlen(ruleTag) > 0)
			return _hot_mgr->getRuleFactor(ruleTag, commInfo->getFullPid(), 0);
	}

	return 1.0;
}

void WtDataManager::subscribe_bar(const char* stdCode, WTSKlinePeriod period, uint32_t times)
{
	if (_reader == NULL)
		return;

	std::string key = fmtutil::format("{}-{}-{}", stdCode, (uint32_t)period, times);

	uint32_t curDate = TimeUtils::getCurDate();
	uint64_t etime = (uint64_t)curDate * 10000 + 2359;

	if (times == 1)
	{
		WTSKlineSlice* slice = _reader->readKlineSliceByCount(stdCode, period, 10, etime);
		if (slice == NULL)
			return;

		WTSKlineData* kline = WTSKlineData::create(stdCode, slice->size());
		kline->setPeriod(period);
		uint32_t offset = 0;
		for(uint32_t blkIdx = 0; blkIdx < slice->get_block_counts(); blkIdx++)
		{
			memcpy(kline->getDataRef().data() + offset, slice->get_block_addr(blkIdx), sizeof(WTSBarStruct)*slice->get_block_size(blkIdx));
			offset += slice->get_block_size(blkIdx);
		}
		
		{
			StdUniqueLock lock(_mtx_rtbars);
			if (_rt_bars == NULL)
				_rt_bars = RtBarMap::create();

			_rt_bars->add(key, kline, false);
		}

		slice->release();
	}
	else
	{
		//只有非基础周期的会进到下面的步骤
		WTSSessionInfo* sInfo = get_session_info(stdCode, true);
		WTSKlineSlice* rawData = _reader->readKlineSliceByCount(stdCode, period, 10*times, 0);
		if (rawData != NULL)
		{
			WTSKlineData* kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true);
			{
				StdUniqueLock lock(_mtx_rtbars);
				if (_rt_bars == NULL)
					_rt_bars = RtBarMap::create();
				_rt_bars->add(key, kData, false);
			}
			rawData->release();
		}
	}

	WTSLogger::info("Realtime bar {} has subscribed", key);
}

void WtDataManager::clear_subbed_bars()
{
	StdUniqueLock lock(_mtx_rtbars);
	if (_rt_bars)
		_rt_bars->clear();
}

void WtDataManager::update_bars(const char* stdCode, WTSTickData* newTick)
{
	StdUniqueLock lock(_mtx_rtbars);
	if (_rt_bars == NULL)
		return;
	auto it = _rt_bars->begin();
	for(; it != _rt_bars->end(); it++)
	{
		WTSKlineData* kData = (WTSKlineData*)it->second;
		if (strcmp(kData->code(), stdCode) != 0)
			continue;

		WTSSessionInfo* sInfo = NULL;
		if (newTick->getContractInfo())
			sInfo = newTick->getContractInfo()->getCommInfo()->getSessionInfo();
		else
			sInfo = get_session_info(kData->code(), true);
		if (kData->period() == KP_DAY || kData->period() == KP_Tick)
		{
			// Day and second updates return a detached heap bar when a new bucket
			// starts. Copy it into the subscribed cache before notifying listeners.
#if defined(WT_P12_BAR_LIFETIME_TEST_HOOKS)
			WTSBarStruct* detachedBar =
				g_dataFact.updateKlineData(kData, newTick, sInfo, _align_by_section);
			if (detachedBar != NULL)
				notify_wt_dm_bar_lifetime((uintptr_t)detachedBar, WT_DM_BAR_FACTORY_RETURNED);
			auto trackedDeleter = [](WTSBarStruct* bar) {
				const uintptr_t address = (uintptr_t)bar;
				delete bar;
				notify_wt_dm_bar_lifetime(address, WT_DM_BAR_DELETED);
			};
			std::unique_ptr<WTSBarStruct, decltype(trackedDeleter)> newBar(detachedBar, trackedDeleter);
#else
			std::unique_ptr<WTSBarStruct> newBar(
				g_dataFact.updateKlineData(kData, newTick, sInfo, _align_by_section));
#endif
			if (newBar != NULL)
				kData->appendBar(*newBar);
		}
		else
		{
			g_dataFact.updateKlineData(kData, newTick, sInfo, _align_by_section);
		}
		WTSBarStruct* lastBar = kData->at(-1);
		if (lastBar == NULL)
			continue;

		std::string speriod;
		uint32_t times = kData->times();
		switch (kData->period())
		{
		case KP_Minute1:
			speriod = fmtutil::format("m{}", times);
			break;
		case KP_Minute5:
			speriod = fmtutil::format("m{}", times*5);
			break;
		default:
			speriod = fmtutil::format("d{}", times);
			break;
		}

		_runner->trigger_bar(stdCode, speriod.c_str(), lastBar);
	}
}

void WtDataManager::clear_cache()
{
	if (_reader == NULL)
	{
		WTSLogger::warn("DataReader not initialized, clearing canceled");
		return;
	}

	//卡11=B / R102：先清理自有的重采样缓存与实时 K 线 map，再清理 reader 缓存。
	//缓存只释放自己的引用，仍被调用者持有的切片在 release 前保持有效。
	{
		StdUniqueLock lock(_mtx_cache);
		release_bar_caches();
	}
	{
		StdUniqueLock lock(_mtx_rtbars);
		if (_rt_bars != NULL)
			_rt_bars->clear();
	}

	_reader->clearCache();
	WTSLogger::warn("All cache cleared");
}
