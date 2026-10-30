/*!
 * \file WtSimpDataMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief
 */
#include "WtSimpDataMgr.h"
#include "WtExecRunner.h"
#include "../WtCore/WtHelper.h"

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Includes/WTSSessionInfo.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"

USING_NS_WTP;


WTSDataFactory g_dataFact;

WtSimpDataMgr::WtSimpDataMgr()
	: _reader(NULL)
	, _runner(NULL)
	, _s_info(NULL)
	, _bars_cache(NULL)
	, _rt_tick_map(NULL)
	, _cur_date(0)
	, _cur_act_time(0)
	, _cur_raw_time(0)
	, _cur_min_time(0)
	, _cur_secs(0)
	, _cur_tdate(0)
{
}


WtSimpDataMgr::~WtSimpDataMgr()
{
	if (_rt_tick_map)
		_rt_tick_map->release();
}

bool WtSimpDataMgr::initStore(WTSVariant* cfg)
{
	if (cfg == NULL || _runner == NULL)
		return false;

	const char* session = cfg->getCString("session");
	if (session == NULL || session[0] == '\0')
	{
		WTSLogger::error("Data manager requires a trading session");
		return false;
	}
	WTSSessionInfo* sessionInfo = _runner->get_session_info(session, false);
	if (sessionInfo == NULL)
	{
		WTSLogger::error("Data manager session {} is unavailable", session);
		return false;
	}

	std::string module = cfg->getCString("module");
	if (module.empty())
		module = WtHelper::getInstDir() + DLLHelper::wrap_module("WtDataStorage");
	else
		module = WtHelper::getInstDir() + DLLHelper::wrap_module(module.c_str());

	DllHandle hInst = DLLHelper::load_library(module.c_str());
	if (hInst == NULL)
	{
		WTSLogger::error("Data reader {} loading failed", module.c_str());
		return false;
	}

	FuncCreateDataReader funcCreator = (FuncCreateDataReader)DLLHelper::get_symbol(hInst, "createDataReader");
	if (funcCreator == NULL)
	{
		WTSLogger::error("Data reader {} loading failed: entrance function createDataReader not found", module.c_str());
		DLLHelper::free_library(hInst);
		return false;
	}

	_reader = funcCreator();
	if (_reader == NULL)
	{
		WTSLogger::error("Data reader {} creating api failed", module.c_str());
		DLLHelper::free_library(hInst);
		return false;
	}

	_reader->init(cfg, this);
	_s_info = sessionInfo;

	return true;
}

bool WtSimpDataMgr::init(WTSVariant* cfg, WtExecRunner* runner)
{
	// Clear the session gate before every attempt so a failed reinitialization
	// cannot accept quotes using a session from an earlier configuration.
	_s_info = NULL;
	if (cfg == NULL || runner == NULL)
		return false;

	_runner = runner;
	return initStore(cfg->get("store"));
}

void WtSimpDataMgr::on_all_bar_updated(uint32_t /*updateTime*/)
{

}

IBaseDataMgr* WtSimpDataMgr::get_basedata_mgr()
{
	return _runner->get_bd_mgr();
}

IHotMgr* WtSimpDataMgr::get_hot_mgr()
{
	return _runner->get_hot_mgr();
}

uint32_t WtSimpDataMgr::get_date()
{
	return _cur_date;
}

uint32_t WtSimpDataMgr::get_min_time()
{
	return _cur_min_time;
}

uint32_t WtSimpDataMgr::get_secs()
{
	return _cur_secs;
}

void WtSimpDataMgr::reader_log(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}

void WtSimpDataMgr::on_bar(const char* /*code*/, WTSKlinePeriod /*period*/, WTSBarStruct* /*newBar*/)
{

}

void WtSimpDataMgr::handle_push_quote(const char* stdCode, WTSTickData* curTick)
{
	if (curTick == NULL || _s_info == NULL)
		return;

	uint32_t uDate = curTick->actiondate();
	uint32_t uTime = curTick->actiontime();

	if (_cur_date != 0 && (uDate < _cur_date || (uDate == _cur_date && uTime < _cur_act_time)))
	{
		return;
	}

	if (_rt_tick_map == NULL)
		_rt_tick_map = DataCacheMap::create();

	_rt_tick_map->add(stdCode, curTick, true);

	_cur_date = uDate;
	_cur_act_time = uTime;

	_cur_raw_time = _cur_act_time / 100000;
	_cur_secs = _cur_act_time % 100000;
	uint32_t minutes = _s_info->timeToMinutes(_cur_raw_time);
	bool isSecEnd = _s_info->isLastOfSection(_cur_raw_time);
	if (isSecEnd)
	{
		minutes--;
	}
	minutes++;
	_cur_min_time = _s_info->minuteToTime(minutes);
	_cur_tdate = curTick->tradingdate();
}

WTSTickData* WtSimpDataMgr::grab_last_tick(const char* code)
{
	if (_rt_tick_map == NULL)
		return NULL;

	WTSTickData* curTick = (WTSTickData*)_rt_tick_map->get(code);
	if (curTick == NULL)
		return NULL;

	curTick->retain();
	return curTick;
}


WTSTickSlice* WtSimpDataMgr::get_tick_slice(const char* code, uint32_t count, uint64_t etime /*= 0*/)
{
	if (_reader == NULL)
		return NULL;

	return _reader->readTickSlice(code, count, etime);
}


WTSKlineSlice* WtSimpDataMgr::get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime /*= 0*/)
{
	if (_reader == NULL)
		return NULL;

	std::string key = fmtutil::format<64>("{}-{}", stdCode, (uint32_t)period);

	if (times == 1)
	{
		return _reader->readKlineSlice(stdCode, period, count, etime);
	}

	//只有非基础周期的会进到下面的步骤
	WTSSessionInfo* sInfo = _runner->get_session_info(stdCode, true);

	if (_bars_cache == NULL)
		_bars_cache = DataCacheMap::create();

	key = fmtutil::format<64>("{}-{}-{}", stdCode, (uint32_t)period, times);

	WTSKlineData* kData = (WTSKlineData*)_bars_cache->get(key);
	//如果缓存里的K线条数大于请求的条数, 则直接返回
	if (kData == NULL || kData->size() < count)
	{
		uint32_t realCount = count * times + times;
		WTSKlineSlice* rawData = _reader->readKlineSlice(stdCode, period, realCount, etime);
		if (rawData != NULL)
		{
			kData = g_dataFact.extractKlineData(rawData, period, times, sInfo, true);
			rawData->release();
		}
		else
		{
			return NULL;
		}

		// A non-null source slice can still be empty or unsuitable for resampling.
		// In that case extractKlineData returns null and there is no cache to slice.
		if (kData == NULL)
			return NULL;

		_bars_cache->add(key, kData, false);
	}

	int32_t sIdx = 0;
	uint32_t rtCnt = min(kData->size(), count);
	sIdx = kData->size() - rtCnt;
	WTSBarStruct* rtHead = kData->at(sIdx);
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, times, rtHead, rtCnt);
	return slice;
}
