/*!
 * \file HisDataReplayer.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "HisDataReplayer.h"
#include "EventNotifier.h"
#include "WtHelper.h"
#include "ReplayTimeRange.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <queue>
#include <tuple>

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/Converter.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSTools/WTSDataFactory.h"
#include "../WTSTools/CsvHelper.h"

#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Share/CodeHelper.hpp"

#include <filesystem>
namespace fs = std::filesystem;

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

using namespace std;

namespace
{
	using wtp::replay::CivilDateTime;
	using wtp::replay::ReplayTimeRange;
	using wtp::replay::TimeRangeParseError;

	const char* time_range_error_text(TimeRangeParseError error)
	{
		switch (error)
		{
		case TimeRangeParseError::InvalidLength: return "expected 12 or 14 decimal digits";
		case TimeRangeParseError::NonDigit: return "timestamp contains a non-digit";
		case TimeRangeParseError::InvalidDate: return "invalid Gregorian date";
		case TimeRangeParseError::InvalidTime: return "invalid clock time";
		case TimeRangeParseError::ReversedRange: return "begin is at or after the exclusive end";
		case TimeRangeParseError::None: return "none";
		}
		return "unknown timestamp error";
	}

	uint64_t timestamp_value(const char* text)
	{
		if (text == NULL)
			return 0;

		uint64_t value = 0;
		for (const char* p = text; *p != '\0'; ++p)
		{
			if (*p < '0' || *p > '9')
				return 0;
			value = value * 10 + static_cast<uint64_t>(*p - '0');
		}
		return value;
	}

	std::string base_tick_code(const std::string& code)
	{
		if (!code.empty() && (code.back() == SUFFIX_QFQ || code.back() == SUFFIX_HFQ))
			return code.substr(0, code.size() - 1);
		return code;
	}

	bool advance_civil_minute(CivilDateTime& value)
	{
		if (++value.minute < 60)
			return true;
		value.minute = 0;
		if (++value.hour < 24)
			return true;
		value.hour = 0;
		if (++value.day <= wtp::replay::detail::daysInMonth(value.year, value.month))
			return true;
		value.day = 1;
		if (++value.month <= 12)
			return true;
		value.month = 1;
		if (++value.year <= 9999)
			return true;
		return false;
	}

	bool advance_civil_second(CivilDateTime& value)
	{
		if (++value.second < 60)
			return true;
		value.second = 0;
		return advance_civil_minute(value);
	}

	uint64_t civil_minute_value(const CivilDateTime& value)
	{
		const uint64_t date = static_cast<uint64_t>(value.year) * 10000 + value.month * 100 + value.day;
		return date * 10000 + value.hour * 100 + value.minute;
	}

	uint64_t civil_hft_time_value(const CivilDateTime& value)
	{
		const uint64_t date = static_cast<uint64_t>(value.year) * 10000 + value.month * 100 + value.day;
		const uint64_t hhmm = static_cast<uint64_t>(value.hour) * 100 + value.minute;
		const uint64_t actionTime = hhmm * 100000 + static_cast<uint64_t>(value.second) * 1000;
		return date * 1000000000 + actionTime;
	}

	bool continuous_session_natural_bounds(uint32_t tradingDate, int32_t offsetMins,
		uint64_t& beginTime, uint64_t& endTime)
	{
		beginTime = 0;
		endTime = 0;
		const int64_t offset = offsetMins;
		if (tradingDate == 0 || offset <= -1440 || offset >= 1440)
			return false;

		const int64_t cutoffMinute = offset > 0 ? 1440 - offset : -offset;
		const uint32_t cutoffTime = static_cast<uint32_t>((cutoffMinute / 60) * 100 + cutoffMinute % 60);
		const uint32_t beginDate = offset > 0 ? TimeUtils::getNextDate(tradingDate, -1) : tradingDate;
		const uint32_t endDate = offset < 0 ? TimeUtils::getNextDate(tradingDate, 1)
			: (offset > 0 ? tradingDate : TimeUtils::getNextDate(tradingDate, 1));
		if (beginDate == 0 || endDate <= beginDate)
			return false;
		beginTime = static_cast<uint64_t>(beginDate) * 1000000000ull +
			static_cast<uint64_t>(cutoffTime) * 100000ull;
		endTime = static_cast<uint64_t>(endDate) * 1000000000ull +
			static_cast<uint64_t>(cutoffTime) * 100000ull;
		return beginTime < endTime;
	}

	template<typename T>
	class WTSReleaseGuard
	{
	public:
		explicit WTSReleaseGuard(T* object) : _object(object) {}
		~WTSReleaseGuard()
		{
			if (_object != NULL)
				_object->release();
		}
		T* get() const { return _object; }
		WTSReleaseGuard(const WTSReleaseGuard&) = delete;
		WTSReleaseGuard& operator=(const WTSReleaseGuard&) = delete;

	private:
		T* _object;
	};
}

/*
 *	处理块数据
 */
bool proc_block_data(const char* tag, std::string& content, bool isBar, bool bKeepHead = true)
{
	BlockHeader* header = (BlockHeader*)content.data();

	bool bCmped = header->is_compressed();
	bool bOldVer = header->is_old_version();

	//如果既没有压缩，也不是老版本结构体，则直接返回
	if (!bCmped && !bOldVer)
	{
		if (!bKeepHead)
			content.erase(0, BLOCK_HEADER_SIZE);
		return true;
	}

	std::string buffer;
	if (bCmped)
	{
		BlockHeaderV2* blkV2 = (BlockHeaderV2*)content.c_str();

		if (content.size() != (sizeof(BlockHeaderV2) + blkV2->_size))
		{
			WTSLogger::error("Size check failed while processing {} data of {}", isBar ? "bar" : "tick", tag);
			return false;
		}

		//将文件头后面的数据进行解压
		buffer = WTSCmpHelper::uncompress_data(content.data() + BLOCK_HEADERV2_SIZE, blkV2->_size);
	}
	else
	{
		if (!bOldVer)
		{
			//如果不是老版本，直接返回
			if (!bKeepHead)
				content.erase(0, BLOCK_HEADER_SIZE);
			return true;
		}
		else
		{
			buffer.append(content.data() + BLOCK_HEADER_SIZE, content.size() - BLOCK_HEADER_SIZE);
		}
	}

	if (bOldVer)
	{
		if (isBar)
		{
			std::string bufV2;
			uint32_t barcnt = buffer.size() / sizeof(WTSBarStructOld);
			bufV2.resize(barcnt * sizeof(WTSBarStruct));
			WTSBarStruct* newBar = (WTSBarStruct*)bufV2.data();
			WTSBarStructOld* oldBar = (WTSBarStructOld*)buffer.data();
			for (uint32_t idx = 0; idx < barcnt; idx++)
			{
				newBar[idx] = oldBar[idx];
			}
			buffer.swap(bufV2);

			WTSLogger::debug("{} bars of {} transferd to new version...", barcnt, tag);
		}
		else
		{
			uint32_t tick_cnt = buffer.size() / sizeof(WTSTickStructOld);
			std::string bufv2;
			bufv2.resize(sizeof(WTSTickStruct)*tick_cnt);
			WTSTickStruct* newTick = (WTSTickStruct*)bufv2.data();
			WTSTickStructOld* oldTick = (WTSTickStructOld*)buffer.data();
			for (uint32_t i = 0; i < tick_cnt; i++)
			{
				newTick[i] = oldTick[i];
			}
			buffer.swap(bufv2);

			WTSLogger::debug("{} ticks of {} transferd to new version...", tick_cnt, tag);
		}
	}

	if (bKeepHead)
	{
		//原来的缓存，resize到文件头大小，再追加最终的数据
		content.resize(BLOCK_HEADER_SIZE);
		content.append(buffer);

		//修改数据块的版本号
		header = (BlockHeader*)content.data();
		header->_version = BLOCK_VERSION_RAW_V2;
	}
	else
	{
		//不保留块头，直接跟数据做一个swap
		content.swap(buffer);
	}

	return true;
}

HisDataReplayer::HisDataReplayer()
	: _listener(NULL)
	, _bt_loader(NULL)
	, _min_period("d")
	, _tick_enabled(true)
	, _tick_simulated(true)
	, _align_by_section(false)
, _cur_date(0)
	, _cur_time(0)
	, _cur_secs(0)
	, _cur_tdate(0)
	, _closed_tdate(0)
	, _opened_tdate(0)
	, _begin_time(0)
	, _end_time(0)
	, _bar_begin_time(0)
	, _bar_end_time(0)
	, _tick_begin_time(0)
	, _tick_end_exclusive_time(0)
	, _range_begin_date(0)
	, _range_begin_hhmm(0)
	, _range_end_date(0)
	, _range_end_hhmm(0)
	, _time_range_valid(false)
	, _time_range_explicit(false)
	, _has_bar_minutes(false)
	, _cache_clear_days(0)
	, _running(false)
	, _notifier(NULL)
	{
}

void HisDataReplayer::set_time_range(uint64_t stime, uint64_t etime)
{
	_time_range_explicit = true;
	const std::string beginText = std::to_string(stime);
	const std::string endText = std::to_string(etime);
	apply_time_range(beginText.c_str(), endText.c_str(), "set_time_range");
}

bool HisDataReplayer::apply_time_range(const char* stime, const char* etime, const char* source)
{
	const char* beginText = stime == NULL ? "" : stime;
	const char* endText = etime == NULL ? "" : etime;
	_begin_time = timestamp_value(beginText);
	_end_time = timestamp_value(endText);
	_bar_begin_time = 0;
	_bar_end_time = 0;
	_tick_begin_time = 0;
	_tick_end_exclusive_time = 0;
	_range_begin_date = 0;
	_range_begin_hhmm = 0;
	_range_end_date = 0;
	_range_end_hhmm = 0;
	_time_range_valid = false;
	_has_bar_minutes = false;

	ReplayTimeRange range;
	const TimeRangeParseError error = wtp::replay::parseReplayTimeRange(beginText, endText, range);
	if (error != TimeRangeParseError::None)
	{
		WTSLogger::error("Invalid replay time range from {}: [{},{}] ({})",
			source, beginText, endText, time_range_error_text(error));
		return false;
	}

	_range_begin_date = range.begin.year * 10000 + range.begin.month * 100 + range.begin.day;
	_range_begin_hhmm = range.begin.hour * 100 + range.begin.minute;
	_range_end_date = range.end.year * 10000 + range.end.month * 100 + range.end.day;
	_range_end_hhmm = range.end.hour * 100 + range.end.minute;
	_tick_begin_time = civil_hft_time_value(range.begin);
	CivilDateTime tickEndExclusive = range.end;
	const bool hasRepresentableTickEnd = range.end.precisionDigits == 12
		? advance_civil_minute(tickEndExclusive)
		: advance_civil_second(tickEndExclusive);
	_tick_end_exclusive_time = hasRepresentableTickEnd
		? civil_hft_time_value(tickEndExclusive)
		: UINT64_MAX;
	_bar_end_time = civil_minute_value(range.end);
	_has_bar_minutes = range.hasBarMinutes();

	CivilDateTime barBegin = range.begin;
	if (_has_bar_minutes && barBegin.second != 0 && !advance_civil_minute(barBegin))
	{
		_has_bar_minutes = false;
		WTSLogger::error("Replay start {} cannot be rounded to a supported bar minute", beginText);
		return false;
	}
	_bar_begin_time = civil_minute_value(barBegin);
	if (_has_bar_minutes && _bar_begin_time > _bar_end_time)
	{
		_has_bar_minutes = false;
		WTSLogger::error("Replay range [{},{}] has inconsistent minute bounds", beginText, endText);
		return false;
	}

	_time_range_valid = true;
	return true;
}


HisDataReplayer::~HisDataReplayer()
{
}


bool HisDataReplayer::init(WTSVariant* cfg, EventNotifier* notifier /* = NULL */, IBtDataLoader* dataLoader /* = NULL */)
{
	if (cfg == NULL || !cfg->isObject())
	{
		WTSLogger::error("Invalid replayer config: expected an object");
		return false;
	}

	WTSVariant* cfgBF = cfg->get("basefiles");
	if (cfgBF == NULL || !cfgBF->isObject())
	{
		WTSLogger::error("Invalid replayer config: required object 'basefiles' is missing or invalid");
		return false;
	}

	if (!_time_range_explicit)
	{
		if (!apply_time_range(cfg->getCString("stime"), cfg->getCString("etime"), "config"))
			return false;
	}
	else if (!_time_range_valid)
	{
		WTSLogger::error("Cannot initialize replayer with an invalid explicit time range");
		return false;
	}

	_notifier = notifier;
	_bt_loader = dataLoader;

	_mode = cfg->getCString("mode");
	/*
	 *	By Wesley @ 2022.01.11
	 *	因为store可能会变复杂，所以这里做一个兼容处理
	 *	如果有store就读取store的path，如果没有store，就还读取root的path
	 */
	if (cfg->has("store"))
	{
		_base_dir = StrUtil::standardisePath(cfg->get("store")->getCString("path"));
	}
	else
	{
		_base_dir = StrUtil::standardisePath(cfg->getCString("path"));
	}
	
	if(_mode == "storage" || _mode == "bin" || _mode == "wtp")
	{
		if (cfg->has("store"))
		{
			_his_dt_mgr.init(cfg->get("store"));
		}
		else
		{
			WTSVariant* item = WTSVariant::createObject();
			item->append("path", _base_dir.c_str());
			_his_dt_mgr.init(item);
			item->release();
		}
	}
	
	WTSLogger::info("Backtest time range is set to be [{},{}] via {}",
		_begin_time, _end_time, _time_range_explicit ? "set_time_range" : "config");

	_cache_clear_days = cfg->getUInt32("cache_clear_days");
	WTSLogger::info("Unused cache data will be cleard in {} days", _cache_clear_days);
	

	_tick_enabled = cfg->getBoolean("tick");
	WTSLogger::info("Tick data replaying is {}", _tick_enabled ? "enabled" : "disabled");

	_adjust_flag = cfg->getUInt32("adjust_flag");
	WTSLogger::info("adjust_flag is {}", _adjust_flag);

	_align_by_section = cfg->getBoolean("align_by_section");
	WTSLogger::info("Resampled bars will be aligned by section: {}", _align_by_section ? "yes" : " no");

	_nosim_if_notrade = cfg->getBoolean("dont_simtick_if_notrade");
	WTSLogger::info("nosim_if_notrade is {}", _nosim_if_notrade);

	//基础数据文件
	if (cfgBF->get("session"))
		_bd_mgr.loadSessions(cfgBF->getCString("session"));

	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for(uint32_t i = 0; i < cfgItem->size(); i ++)
			{
				_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
			}
		}
	}

	cfgItem = cfgBF->get("contract");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadContracts(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
			{
				_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
			}
		}
	}

	if (cfgBF->get("holiday"))
		_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));

	if (cfgBF->get("hot"))
		_hot_mgr.loadHots(cfgBF->getCString("hot"));

	if (cfgBF->get("second"))
		_hot_mgr.loadSeconds(cfgBF->getCString("second"));

	if (cfgBF->has("rules"))
	{
		auto cfgRules = cfgBF->get("rules");
		auto tags = cfgRules->memberNames();
		for (const std::string& ruleTag : tags)
		{
			_hot_mgr.loadCustomRules(ruleTag.c_str(), cfgRules->getCString(ruleTag.c_str()));
			WTSLogger::info("{} rules loaded from {}", ruleTag, cfgRules->getCString(ruleTag.c_str()));
		}
	}

	loadFees(cfg->getCString("fees"));

	/*
	 *	By Wesley @ 2021.12.20
	 *	先从extloader加载除权因子
	 *	如果加载失败，并且配置了除权因子文件，再加载除权因子文件
	 */
	bool bLoaded = loadStkAdjFactorsFromLoader();

	if (!bLoaded && cfg->has("adjfactor"))
		loadStkAdjFactorsFromFile(cfg->getCString("adjfactor"));

	return true;
}

bool HisDataReplayer::loadStkAdjFactorsFromLoader()
{
	if (NULL == _bt_loader)
		return false;

	bool ret = _bt_loader->loadAllAdjFactors(this, [](void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count) {
		HisDataReplayer* replayer = (HisDataReplayer*)obj;
		AdjFactorList& fctrLst = replayer->_adj_factors[stdCode];

		for (uint32_t i = 0; i < count; i++)
		{
			AdjFactor adjFact;
			adjFact._date = dates[i];
			adjFact._factor = factors[i];

			fctrLst.emplace_back(adjFact);
		}

		//一定要把第一条加进去，不然如果是前复权的话，可能会漏处理最早的数据
		AdjFactor adjFact;
		adjFact._date = 19900101;
		adjFact._factor = 1;
		fctrLst.emplace_back(adjFact);

		std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
			return left._date < right._date;
		});
	});

	if (ret) WTSLogger::info("Adjusting factors of {} contracts loaded via extended loader", _adj_factors.size());
	return ret;
}

bool HisDataReplayer::loadStkAdjFactorsFromFile(const char* adjfile)
{
	if (!StdFile::exists(adjfile))
	{
		WTSLogger::error("Adjust factor file {} not exists, skipped", adjfile);
		return false;
	}

	std::string content;
	StdFile::read_file_content(adjfile, content);

	WTSVariant* doc = WTSCfgLoader::load_from_file(adjfile);
	if (doc == NULL)
	{
		WTSLogger::error("Parsing adjust factor file {} faield", adjfile);
		return false;
	}

	uint32_t stk_cnt = 0;
	uint32_t fct_cnt = 0;
	for (const std::string& exchg : doc->memberNames())
	{
		WTSVariant* itemExchg = doc->get(exchg);
		for (const std::string& code : itemExchg->memberNames())
		{
			WTSVariant* ayFacts = itemExchg->get(code);
			if (!ayFacts->isArray())
				continue;

			/*
			 *	By Wesley @ 2021.12.21
			 *	先检查code的格式是不是包含PID，如STK.600000
			 *	如果包含PID，则直接格式化，如果不包含，则强制为STK
			 */
			bool bHasPID = (code.find('.') != std::string::npos);

			std::string key;
			if(bHasPID)
				key = fmt::format("{}.{}", exchg, code);
			else
				key = fmt::format("{}.STK.{}", exchg, code);
			stk_cnt++;

			AdjFactorList& fctrLst = _adj_factors[key];
			for (uint32_t i = 0; i < ayFacts->size(); i++)
			{
				WTSVariant* fItem = ayFacts->get(i);
				AdjFactor adjFact;
				adjFact._date = fItem->getUInt32("date");
				adjFact._factor = fItem->getDouble("factor");

				fctrLst.emplace_back(adjFact);
				fct_cnt++;
			}

			//一定要把第一条加进去，不然如果是前复权的话，可能会漏处理最早的数据
			AdjFactor adjFact;
			adjFact._date = 19900101;
			adjFact._factor = 1;
			fctrLst.emplace_back(adjFact);

			std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
				return left._date < right._date;
			});
		}
	}

	WTSLogger::info("{} items of adjust factors for {} tickers loaded from {}", fct_cnt, stk_cnt, adjfile);
	doc->release();
	return true;
}

void HisDataReplayer::register_task(uint32_t taskid, uint32_t date, uint32_t time, const char* period, const char* trdtpl /* = "CHINA" */, const char* session /* = "TRADING" */)
{
	TaskPeriodType ptype;
	if (wt_stricmp(period, "d") == 0)
		ptype = TPT_Daily;
	else if (wt_stricmp(period, "w") == 0)
		ptype = TPT_Weekly;
	else if (wt_stricmp(period, "m") == 0)
		ptype = TPT_Monthly;
	else if (wt_stricmp(period, "y") == 0)
		ptype = TPT_Yearly;
	else if (wt_stricmp(period, "min") == 0)
		ptype = TPT_Minute;
	else
		ptype = TPT_None;

	_task.reset(new TaskInfo);
	strcpy(_task->_name, "sel");
	strcpy(_task->_trdtpl, trdtpl);
	strcpy(_task->_session, session);
	_task->_day = date;
	_task->_time = time;
	_task->_id = taskid;
	_task->_period = ptype;
	_task->_strict_time = true;

	WTSLogger::info("Timed task registration succeed, frequency: {}", period);
}

void HisDataReplayer::clear_cache()
{
	_ticks_cache.clear();
	_orddtl_cache.clear();
	_ordque_cache.clear();
	_trans_cache.clear();
	_replay_ticks_cache.clear();
	_replay_orddtl_cache.clear();
	_replay_ordque_cache.clear();
	_replay_trans_cache.clear();

	_bars_cache.clear();
	_unbars_cache.clear();
	_unsubbed_in_need.clear();

	_main_key = "";
	_main_period = "";
	_tick_sub_map.clear();
	_min_period = "";
	_day_cache.clear();
	_ticker_keys.clear();
	_bar_real_tick_codes.clear();
	_last_replay_hft_time = 0;
	_has_last_replay_hft_time = false;
	_replay_completed_natural_date = 0;
	_replay_completed_natural_ranges.clear();
	_has_replay_completed_natural_date = false;
	_sim_tick_batch_active = false;
	_sim_tick_batch_time = 0;
	_sim_tick_batch_close_keys.clear();
	_sim_tick_batch_ticker_keys.clear();

	_price_map.clear();

	WTSLogger::log_raw(LL_WARN, "All cached data cleared");
}

void HisDataReplayer::reset()
{
	// Replay buckets own their vectors. Drop them on reset so reruns restart at
	// the configured range without touching vectors borrowed by history slices.
	_replay_ticks_cache.clear();
	_replay_orddtl_cache.clear();
	_replay_ordque_cache.clear();
	_replay_trans_cache.clear();
	_last_replay_hft_time = 0;
	_has_last_replay_hft_time = false;
	_replay_completed_natural_date = 0;
	_replay_completed_natural_ranges.clear();
	_has_replay_completed_natural_date = false;

	//重置不会清除掉缓存，而是将读取的标记还原，这样不用重复加载主句
	for(auto& m : _ticks_cache)
	{
		HftDataList<WTSTickStruct>& cacheItem = (HftDataList<WTSTickStruct>&)m.second;
		cacheItem._cursor = UINT_MAX;
	}

	for (auto& m : _orddtl_cache)
	{
		HftDataList<WTSOrdDtlStruct>& cacheItem = (HftDataList<WTSOrdDtlStruct>&)m.second;
		cacheItem._cursor = UINT_MAX;
	}

	for (auto& m : _ordque_cache)
	{
		HftDataList<WTSOrdQueStruct>& cacheItem = (HftDataList<WTSOrdQueStruct>&)m.second;
		cacheItem._cursor = UINT_MAX;
	}

	for (auto& m : _trans_cache)
	{
		HftDataList<WTSTransStruct>& cacheItem = (HftDataList<WTSTransStruct>&)m.second;
		cacheItem._cursor = UINT_MAX;
	}

	for (auto& m : _bars_cache)
	{
		BarsListPtr& cacheItem = (BarsListPtr&)m.second;
		cacheItem->_cursor = UINT_MAX;
		cacheItem->_notify_close = false;

		WTSLogger::info("Reading flag of {} has been reset", m.first.c_str());
	}

	_unbars_cache.clear();
	_codes_in_subbed.clear();
	_codes_in_unsubbed.clear();
	_unsubbed_in_need.clear();

	_day_cache.clear();
	_ticker_keys.clear();
	_bar_real_tick_codes.clear();
	_sim_tick_batch_active = false;
	_sim_tick_batch_time = 0;
	_sim_tick_batch_close_keys.clear();
	_sim_tick_batch_ticker_keys.clear();

	_tick_sub_map.clear();
	_ordque_sub_map.clear();
	_orddtl_sub_map.clear();
	_trans_sub_map.clear();

	_price_map.clear();

	_main_key = "";
	_min_period = "";

	_cur_date = 0;
	_cur_time = 0;
	_cur_secs = 0;
	_cur_tdate = 0;
	_opened_tdate = 0;
	_closed_tdate = 0;
	_tick_simulated = true;
}

void HisDataReplayer::dump_btstate(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t stime, uint64_t etime, double progress, int64_t elapse)
{
	std::string output;
	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		root.AddMember("code", rj::Value(stdCode, allocator), allocator);

		std::stringstream ss;
		if (period == KP_DAY)
			ss << "d";
		else if (period == KP_Minute1)
			ss << "m" << times;
		else
			ss << "m" << times * 5;

		root.AddMember("period", rj::Value(ss.str().c_str(), allocator), allocator);
		
		root.AddMember("stime", stime, allocator);
		root.AddMember("etime", etime, allocator);
		root.AddMember("progress", progress, allocator);
		root.AddMember("elapse", elapse, allocator);

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);

		output = sb.GetString();
	}

	std::string folder = WtHelper::getOutputDir();
	folder += _stra_name;
	folder += "/";
	fs::create_directories(folder.c_str());
	std::string filename = folder + "btenv.json";
	StdFile::write_file_content(filename.c_str(), output.c_str(), output.size());
}

void HisDataReplayer::notify_state(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t stime, uint64_t etime, double progress)
{
	if (!_notifier)
		return;

	std::string output;
	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		root.AddMember("code", rj::Value(stdCode, allocator), allocator);

		std::stringstream ss;
		if (period == KP_DAY)
			ss << "d";
		else if (period == KP_Minute1)
			ss << "m" << times;
		else
			ss << "m" << times * 5;

		root.AddMember("period", rj::Value(ss.str().c_str(), allocator), allocator);

		root.AddMember("stime", stime, allocator);
		root.AddMember("etime", etime, allocator);
		root.AddMember("progress", progress, allocator);

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);

		output = sb.GetString();
	}

	_notifier->notifyData("BT_STATE", (void*)output.c_str(), output.size());
}

uint32_t HisDataReplayer::locate_barindex(const std::string& key, uint64_t now, bool bUpperBound /* = false */)
{
	uint32_t curDate, curTime;
	curDate = (uint32_t)(now / 10000);
	curTime = (uint32_t)(now % 10000);

	BarsListPtr& barsList = _bars_cache[key];
	if (barsList == NULL || barsList->_bars.empty())
		return UINT32_MAX;

	bool isDay = (barsList->_period == KP_DAY);

	WTSBarStruct bar;
	bar.date = curDate;
	bar.time = (curDate - 19900000) * 10000 + curTime;
	auto it = std::lower_bound(barsList->_bars.begin(), barsList->_bars.end(), bar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});

	std::size_t idx;
	if (it == barsList->_bars.end())
		idx = barsList->_bars.size() - 1;
	else
	{
		if(bUpperBound)
		{//如果是找上边界，则要比较时间向下修正，因为lower_bound函数找的是大于等于curTime的K线
			if ((isDay && it->date > bar.date) || (!isDay && it->time > bar.time))
			{
				if (it == barsList->_bars.begin())
					return UINT32_MAX;
				it--;
			}
		}

		idx = it - barsList->_bars.begin();
	}
	
	return idx;
}

uint64_t HisDataReplayer::get_daily_bar_close_time(const BarsListPtr& barsList, const WTSBarStruct& bar)
{
	if (barsList == NULL)
		return UINT64_MAX;

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(barsList->_code.c_str(), &_hot_mgr);
	const char* commId = codeInfo.stdCommID();
	const uint64_t boundary = _bd_mgr.getBoundaryTime(commId, bar.date, false, false);
	if (boundary != 0)
		return boundary;

	WTSSessionInfo* session = get_session_info(barsList->_code.c_str(), true);
	if (session == NULL)
		return UINT64_MAX;

	uint32_t closeDate = bar.date;
	if (session->getOffsetMins() < 0)
		closeDate = TimeUtils::getNextDate(closeDate);
	return static_cast<uint64_t>(closeDate) * 10000 + session->getCloseTime();
}

void HisDataReplayer::stop()
{
	if(!_running)
	{
		WTSLogger::log_raw(LL_ERROR, "Backtesting is not running, no need to stop");
		return;
	}

	if (_terminated)
		return;

	_terminated = true;
	WTSLogger::log_raw(LL_WARN, "Terminating flag reset to true, backtesting will quit at next round");
}

bool HisDataReplayer::prepare()
{
	if (!_time_range_valid)
	{
		WTSLogger::error("Cannot prepare replayer with an invalid time range");
		return false;
	}

	if (_running)
	{
		WTSLogger::log_raw(LL_ERROR, "Cannot run more than one backtesting task at the same time");
		return false;
	}

	_running = true;
	_terminated = false;
	reset();

	_cur_date = _range_begin_date;
	_cur_time = _range_begin_hhmm;
	_cur_secs = 0;
	_cur_tdate = _bd_mgr.calcTradingDate(DEFAULT_SESSIONID, _cur_date, _cur_time, true);

	try
	{
		if (_notifier)
			_notifier->notifyEvent("BT_START");

		_listener->handle_init();

		if (!_tick_enabled)
			checkUnbars();
	}
	catch (...)
	{
		// Initialization callbacks are part of the replay lifecycle. Leave the
		// replayer retryable when user code throws during prepare().
		_terminated = true;
		_running = false;
		throw;
	}

	return true;
}

void HisDataReplayer::run(bool bNeedDump/* = false*/)
{
	if (!_time_range_valid)
	{
		WTSLogger::error("Cannot run replayer with an invalid time range");
		return;
	}

	try
	{
	if(_task == NULL)
	{
		// A requested main series is committed only after its bars have loaded.
		// Still validate the cache here because callers may retain a stale key
		// after a failed load or cache reset.
		if (!_main_key.empty())
		{
			auto mainIt = _bars_cache.find(_main_key);
			if (mainIt == _bars_cache.end() || mainIt->second == NULL || mainIt->second->_bars.empty())
			{
				WTSLogger::error("Main K bars {} are unavailable; selecting another replay source", _main_key.c_str());
				_main_key.clear();
				_main_period.clear();
			}
		}

		//如果没有时间调度任务,则采用主K线回放的模式

		//如果没有确定主K线,则确定一个周期最短的主K线
		if (_main_key.empty() && !_bars_cache.empty())
		{
			WTSKlinePeriod minPeriod = KP_DAY;
			uint32_t minTimes = 1;
			for(auto& m : _bars_cache)
			{
				const BarsListPtr& barsList = m.second;
				if (barsList == NULL || !barsList->_notify_close || barsList->_bars.empty())
					continue;

				if (barsList->_period < minPeriod)
				{
					minPeriod = barsList->_period;
					minTimes = barsList->_times;
					_main_key = m.first;
				}
				else if(barsList->_period == minPeriod)
				{
					if(barsList->_times < minTimes)
					{
						_main_key = m.first;
						minTimes = barsList->_times;
					}
					//By Wesley @ 2022.11.03
					//这里主要修复了只用日线的时候不能正确判断主K线的bug
					else if(_main_key.empty())
					{
						_main_key = m.first;
					}
				}
			}

			WTSLogger::info("Main K bars automatic determined: {}", _main_key.c_str());
		}

		if(!_main_key.empty())
		{
			//如果订阅了K线，则按照主K线进行回放
			run_by_bars(bNeedDump);
		}
		else if(_tick_enabled)
		{
			run_by_ticks(bNeedDump);
		}
		else
		{
			WTSLogger::log_raw(LL_INFO, "Main K bars not subscribed and backtesting of tick data not available , replaying done");
			_listener->handle_replay_done();
			if (_notifier)
				_notifier->notifyEvent("BT_END");
		}
	}
	else //if(_task != NULL)
	{
		run_by_tasks(bNeedDump);
	}
	}
	catch (...)
	{
		// Sink callback exceptions abort this replay. Keep the failure visible to
		// the caller, and do not leave the replayer permanently marked as running.
		_terminated = true;
		_running = false;
		throw;
	}

	_running = false;
}

void HisDataReplayer::run_by_ticks(bool /*bNeedDump*/ /* = false */)
{
	// If no bars are subscribed, replay each trade-date file that can contain
	// the natural-time range. A night-session action date may differ from its
	// file's trading date; the precise action timestamp is filtered separately.
	uint32_t beginTDate = 0;
	uint32_t endTDate = 0;
	if (!get_hft_trading_date_range(beginTDate, endTDate))
	{
		_listener->handle_replay_done();
		if (_notifier)
			_notifier->notifyEvent("BT_END");
		return;
	}
	_cur_tdate = beginTDate;

	while (_cur_tdate <= endTDate && !_terminated)
	{
		const bool hasData = checkAllHftData(_cur_tdate);
		if (_terminated)
			break;
		if (hasData)
		{
			WTSLogger::info("Start to replay high-frequency data of {}...", _cur_tdate);
			uint64_t naturalTime = _tick_begin_time;
			bool hasReplayableEvents = false;
			if (!resolve_hft_session_begin_clock(_cur_tdate, beginTDate, naturalTime, hasReplayableEvents))
			{
				_terminated = true;
				break;
			}
			_cur_date = static_cast<uint32_t>(naturalTime / 1000000000);
			const uint32_t actionTime = static_cast<uint32_t>(naturalTime % 1000000000);
			_cur_time = actionTime / 100000;
			_cur_secs = actionTime % 100000;
			uint64_t sessionEnd = next_hft_session_boundary(_cur_tdate, naturalTime);
			if (_terminated)
				break;
			if (sessionEnd == std::numeric_limits<uint64_t>::max())
				sessionEnd = _tick_end_exclusive_time;
			else
				sessionEnd = std::min(sessionEnd, _tick_end_exclusive_time);
			if (sessionEnd <= naturalTime)
			{
				WTSLogger::error("Cannot advance pure-tick replay session window for trading date {}", _cur_tdate);
				_terminated = true;
				break;
			}
			_listener->handle_session_begin(_cur_tdate);
			check_cache_days();
			replayHftDatasByDay(_cur_tdate, naturalTime, sessionEnd);
			_listener->handle_session_end(_cur_tdate);
		}

		_cur_tdate = TimeUtils::getNextDate(_cur_tdate);
	}

	if (_terminated)
		WTSLogger::debug("Replaying by ticks terminated forcely");

	WTSLogger::log_raw(LL_INFO, "All back data replayed, replaying done");
	_listener->handle_replay_done();
	if (_notifier)
		_notifier->notifyEvent("BT_END");
}

bool HisDataReplayer::get_hft_trading_date_range(uint32_t& beginTDate, uint32_t& endTDate)
{
	bool hasSubscription = false;
	beginTDate = UINT32_MAX;
	endTDate = 0;
	auto includeSubscriptions = [&](const auto& subscriptions)
	{
		for (const auto& subscription : subscriptions)
		{
			CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(subscription.first.c_str(), &_hot_mgr);
			const char* commId = codeInfo.stdCommID();
			const char* codeSession = commId == NULL ? "" : commId;
			const uint32_t codeBegin = _bd_mgr.calcTradingDate(codeSession, _range_begin_date, _range_begin_hhmm, false);
			const uint32_t codeEnd = _bd_mgr.calcTradingDate(codeSession, _range_end_date, _range_end_hhmm, false);
			beginTDate = std::min(beginTDate, codeBegin == 0 ? _range_begin_date : codeBegin);
			endTDate = std::max(endTDate, codeEnd == 0 ? _range_end_date : codeEnd);
			hasSubscription = true;
		}
	};
	includeSubscriptions(_tick_sub_map);
	includeSubscriptions(_orddtl_sub_map);
	includeSubscriptions(_ordque_sub_map);
	includeSubscriptions(_trans_sub_map);
	if (!hasSubscription)
		return false;
	if (beginTDate == UINT32_MAX || endTDate < beginTDate)
	{
		WTSLogger::error("Cannot map replay natural-time range to high-frequency trading dates");
		return false;
	}
	return true;
}

bool HisDataReplayer::resolve_hft_session_begin_clock(uint32_t tradingDate, uint32_t firstTradingDate,
	uint64_t& naturalTime, bool& hasReplayableEvents)
{
	naturalTime = _tick_begin_time;
	hasReplayableEvents = false;
	uint64_t earliestBoundary = std::numeric_limits<uint64_t>::max();
	bool validBoundaries = true;
	auto hasInRangeEvent = [this](const auto& dataList)
	{
		if (dataList._cursor == 0)
			return false;
		const std::size_t count = std::min(dataList._count, dataList._items.size());
		for (std::size_t index = 0; index < count; ++index)
		{
			if (dataList._cursor != UINT_MAX && index + 1 < dataList._cursor)
				continue;
			const auto& item = dataList._items[index];
			const uint64_t itemTime = static_cast<uint64_t>(item.action_date) * 1000000000 + item.action_time;
			if (itemTime >= _tick_begin_time && itemTime < _tick_end_exclusive_time)
				return true;
		}
		return false;
	};
	auto collectBoundaries = [&](const auto& subscriptions, const auto& cache)
	{
		if (!validBoundaries)
			return;
		for (const auto& subscription : subscriptions)
		{
			auto dataIt = cache.find(subscription.first);
			if (dataIt == cache.end())
				continue;
			auto dateIt = dataIt->second.find(tradingDate);
			if (dateIt == dataIt->second.end() || !hasInRangeEvent(dateIt->second))
				continue;
			hasReplayableEvents = true;
			if (get_session_info(subscription.first.c_str(), true) == NULL)
			{
				WTSLogger::error("Cannot replay in-range HFT data for {} on trading date {} without a session",
					subscription.first.c_str(), tradingDate);
				validBoundaries = false;
				return;
			}
			if (tradingDate == firstTradingDate)
				continue;

			CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(subscription.first.c_str(), &_hot_mgr);
			const char* commId = codeInfo.stdCommID();
			if (commId == NULL || *commId == '\0')
			{
				WTSLogger::error("Cannot determine session-begin clock for trading date {}: no commodity for {}",
					tradingDate, subscription.first.c_str());
				validBoundaries = false;
				return;
			}

			WTSSessionInfo* session = get_session_info(subscription.first.c_str(), true);
			uint64_t boundary = 0;
			uint64_t candidate = 0;
			if (session != NULL && session->getTradingMins() == 1440)
			{
				uint64_t endBoundary = 0;
				if (!continuous_session_natural_bounds(tradingDate, session->getOffsetMins(), boundary, endBoundary) ||
					boundary == 0 || endBoundary <= boundary)
				{
					WTSLogger::error("Cannot determine 24-hour HFT session boundaries for {} on trading date {} with offset {}",
						subscription.first.c_str(), tradingDate, session->getOffsetMins());
					validBoundaries = false;
					return;
				}
				// continuous_session_natural_bounds returns the natural timestamp
				// encoding used by the replay clock, not getBoundaryTime's packed
				// YYYYMMDDHHMM encoding. Keep it in that domain and do not feed it
				// through packed-date validation or the market-calendar boundary check.
				candidate = boundary;
			}
			else
			{
				boundary = _bd_mgr.getBoundaryTime(commId, tradingDate, false, true);
				const uint32_t boundaryDate = static_cast<uint32_t>(boundary / 10000);
				const uint32_t boundaryTime = static_cast<uint32_t>(boundary % 10000);
				const uint32_t year = boundaryDate / 10000;
				const uint32_t month = boundaryDate / 100 % 100;
				const uint32_t day = boundaryDate % 100;
				const bool validDate = year >= 1 && month >= 1 && month <= 12 && day >= 1 &&
					day <= static_cast<uint32_t>(wtp::replay::detail::daysInMonth(year, month));
				if (boundary < 101010000ull || boundary > 999912312359ull || !validDate ||
					boundaryTime / 100 >= 24 || boundaryTime % 100 >= 60)
				{
					WTSLogger::error("Cannot determine session-begin clock for trading date {}: invalid boundary {} for {}",
						tradingDate, boundary, subscription.first.c_str());
					validBoundaries = false;
					return;
				}

				if (_bd_mgr.calcTradingDate(commId, boundaryDate, boundaryTime, false) != tradingDate)
				{
					WTSLogger::error("Session-begin boundary {} for {} does not map to trading date {}",
						boundary, subscription.first.c_str(), tradingDate);
					validBoundaries = false;
					return;
				}

				candidate = static_cast<uint64_t>(boundaryDate) * 1000000000 +
					static_cast<uint64_t>(boundaryTime) * 100000;
			}
			earliestBoundary = std::min(earliestBoundary, candidate);
		}
	};

	// Presence and boundary probes must use replay-owned buckets. The query
	// caches back borrowed WTSTick/Level2 slices, and changing their date here
	// would invalidate those slices before the caller has released them.
	collectBoundaries(_tick_sub_map, _replay_ticks_cache);
	collectBoundaries(_orddtl_sub_map, _replay_orddtl_cache);
	collectBoundaries(_ordque_sub_map, _replay_ordque_cache);
	collectBoundaries(_trans_sub_map, _replay_trans_cache);
	if (!validBoundaries)
		return false;
	const uint64_t currentTime = static_cast<uint64_t>(_cur_date) * 1000000000 +
		static_cast<uint64_t>(_cur_time) * 100000 + _cur_secs;
	if (tradingDate == firstTradingDate || !hasReplayableEvents)
	{
		if (!hasReplayableEvents)
		{
			naturalTime = std::max(_tick_begin_time, currentTime);
		}
		else
			naturalTime = std::max(_tick_begin_time, currentTime);
		return true;
	}
	if (earliestBoundary == std::numeric_limits<uint64_t>::max())
	{
		WTSLogger::error("Cannot determine session-begin clock for trading date {}: no boundary for replayable HFT data",
			tradingDate);
		return false;
	}
	// Session callbacks share one observable clock across products. A session
	// boundary from a later trading date can map to an earlier natural date for
	// an overnight instrument; never move the public clock backwards.
	naturalTime = std::max(std::max(earliestBoundary, _tick_begin_time), currentTime);
	return true;
}

template<typename T>
void HisDataReplayer::clamp_hft_cursor(HftDataList<T>& itemList)
{
	if (!_time_range_valid || itemList._cursor == 0 || itemList._cursor == UINT_MAX)
		return;

	const std::size_t validCount = std::min(itemList._count, itemList._items.size());
	while (itemList._cursor <= validCount)
	{
		const T& item = itemList._items[itemList._cursor - 1];
		const uint64_t itemTime = static_cast<uint64_t>(item.action_date) * 1000000000 + item.action_time;
		if (itemTime < _tick_begin_time)
		{
			++itemList._cursor;
			continue;
		}
		if (itemTime >= _tick_end_exclusive_time)
			itemList._cursor = validCount + 1;
		return;
	}
}

void HisDataReplayer::run_by_bars(bool bNeedDump /* = false */)
{
	TimeUtils::Ticker ticker;

	auto mainIt = _bars_cache.find(_main_key);
	if (_main_key.empty() || mainIt == _bars_cache.end() || mainIt->second == NULL || mainIt->second->_bars.empty())
	{
		WTSLogger::error("Cannot replay unavailable main K bars {}", _main_key.c_str());
		if (_listener)
			_listener->handle_replay_done();
		if (_notifier)
			_notifier->notifyEvent("BT_END");
		return;
	}

	BarsListPtr barsList = mainIt->second;
	WTSSessionInfo* sInfo = get_session_info(barsList->_code.c_str(), true);
	if (sInfo == NULL)
	{
		WTSLogger::error("Cannot replay main K bars {} without a session", _main_key.c_str());
		if (_listener)
			_listener->handle_replay_done();
		if (_notifier)
			_notifier->notifyEvent("BT_END");
		return;
	}
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(barsList->_code.c_str(), NULL);
	std::string commId = codeInfo.stdCommID();

	uint32_t sIdx = UINT32_MAX;
	uint32_t eIdx = UINT32_MAX;
	if (_has_bar_minutes)
	{
		if (barsList->_period == KP_DAY)
		{
			auto beginIt = std::lower_bound(barsList->_bars.begin(), barsList->_bars.end(), _bar_begin_time,
				[this, &barsList](const WTSBarStruct& bar, uint64_t time) {
					return get_daily_bar_close_time(barsList, bar) < time;
				});
			sIdx = static_cast<uint32_t>(beginIt - barsList->_bars.begin());

			auto endIt = std::upper_bound(barsList->_bars.begin(), barsList->_bars.end(), _bar_end_time,
				[this, &barsList](uint64_t time, const WTSBarStruct& bar) {
					return time < get_daily_bar_close_time(barsList, bar);
				});
			if (endIt != barsList->_bars.begin())
				eIdx = static_cast<uint32_t>(endIt - barsList->_bars.begin() - 1);
		}
		else
		{
			sIdx = locate_barindex(_main_key, _bar_begin_time, false);
			eIdx = locate_barindex(_main_key, _bar_end_time, true);
		}
	}

	bool hasBarRange = _has_bar_minutes && sIdx != UINT32_MAX && eIdx != UINT32_MAX && sIdx <= eIdx;
	if (hasBarRange)
	{
		const WTSBarStruct& firstMainBar = barsList->_bars[sIdx];
		if (barsList->_period == KP_DAY)
		{
			const uint64_t closeTime = get_daily_bar_close_time(barsList, firstMainBar);
			hasBarRange = closeTime >= _bar_begin_time && closeTime <= _bar_end_time;
		}
		else
			hasBarRange = 199000000000ull + firstMainBar.time >= _bar_begin_time;
	}
	if (!hasBarRange && _tick_enabled)
	{
		// A requested main series may have no close inside the selected range.
		// Continue with the exact HFT range without emitting or synthesizing bars.
		run_by_ticks(bNeedDump);
		return;
	}
	uint32_t total_barcnt = hasBarRange ? eIdx - sIdx + 1 : 0;
	uint32_t replayed_barcnt = 0;
	uint64_t lastMainBarTime = 0;
	bool finalSessionEndDeferred = false;

	// History queries in handle_init also advance each BarsList cursor. Start
	// every queried series at its first bar in the replay range so those reads
	// cannot consume the first replay callback. Raw source caches remain at
	// UINT_MAX and are intentionally left untouched.
	if (hasBarRange)
	{
		for (auto& entry : _bars_cache)
		{
			BarsListPtr& cachedBars = entry.second;
			if (cachedBars == NULL || cachedBars->_bars.empty())
				continue;
			if (entry.first != _main_key && cachedBars->_cursor == UINT_MAX)
				continue;

			uint32_t firstIdx;
			if (entry.first == _main_key)
				firstIdx = sIdx;
			else if (cachedBars->_period == KP_DAY)
			{
				auto firstIt = std::lower_bound(cachedBars->_bars.begin(), cachedBars->_bars.end(), _bar_begin_time,
					[this, &cachedBars](const WTSBarStruct& bar, uint64_t time) {
						return get_daily_bar_close_time(cachedBars, bar) < time;
					});
				firstIdx = static_cast<uint32_t>(firstIt - cachedBars->_bars.begin());
			}
			else
				firstIdx = locate_barindex(entry.first, _bar_begin_time, false);
			if (firstIdx == UINT32_MAX || firstIdx >= cachedBars->_bars.size())
			{
				cachedBars->_cursor = (uint32_t)cachedBars->_bars.size();
				continue;
			}

			const WTSBarStruct& firstBar = cachedBars->_bars[firstIdx];
			const uint64_t firstBarTime = cachedBars->_period == KP_DAY
				? get_daily_bar_close_time(cachedBars, firstBar)
				: 199000000000ull + firstBar.time;
			const bool startsInRange = firstBarTime >= _bar_begin_time && firstBarTime <= _bar_end_time;
			cachedBars->_cursor = startsInRange ? firstIdx : (uint32_t)cachedBars->_bars.size();
		}
	}

	notify_state(barsList->_code.c_str(), barsList->_period, barsList->_times, _begin_time, _end_time, 0);

	if (bNeedDump)
		dump_btstate(barsList->_code.c_str(), barsList->_period, barsList->_times, _begin_time, _end_time, 100.0, ticker.nano_seconds());

	WTSLogger::info("Start to replay back data from {}...", _begin_time);

	for (; hasBarRange && !_terminated;)
	{
		bool isDay = barsList->_period == KP_DAY;
		if (barsList->_cursor != UINT_MAX)
		{
			uint64_t nextBarTime = 0;
			if (isDay)
				nextBarTime = get_daily_bar_close_time(barsList, barsList->_bars[barsList->_cursor]);
			else
			{
				nextBarTime = (uint64_t)barsList->_bars[barsList->_cursor].time;
				nextBarTime += 199000000000;
			}

			if (nextBarTime > _bar_end_time)
			{
				WTSLogger::info("{} is beyond ending time {},replaying done", nextBarTime, _end_time);
				break;
			}
			lastMainBarTime = nextBarTime;
			const bool isLastReplayBar = barsList->_cursor >= eIdx;

			uint32_t nextDate = (uint32_t)(nextBarTime / 10000);
			uint32_t nextTime = (uint32_t)(nextBarTime % 10000);

			//By Wesley @ 2022.01.10
			//如果和收盘时间一样，进行这个判断
			//主要针对7*24小时的品种，其他的品种不需要
			uint32_t nextTDate = _opened_tdate;
			if(isDay || (!isDay && sInfo->offsetTime(nextTime, false) != sInfo->getCloseTime(true)))
			{
				nextTDate = _bd_mgr.calcTradingDate(commId.c_str(), nextDate, nextTime, false);
				if (_opened_tdate != nextTDate)
				{
					if(_closed_tdate != _opened_tdate)
					{
						WTSLogger::debug("Tradingday {} ends", _cur_tdate);
						_listener->handle_session_end(_cur_tdate);
						_closed_tdate = _cur_tdate;
						_day_cache.clear();
					}

					/*
					 *	By Wesley @ 2022.06.23
					 *	因为可能会有人在on_session_begin下单，所以这里把时间戳改成开盘时间
					 *	这样signals里看起来比较容易理解一些
					 */
					uint32_t sessionDate = 0;
					uint32_t sessionTime = 0;
					uint32_t sessionSecs = 0;
					bool hasHftEvents = false;
					bool useHftClock = false;
					if (_tick_enabled)
					{
						uint32_t hftBeginTDate = 0;
						uint32_t hftEndTDate = 0;
						if (get_hft_trading_date_range(hftBeginTDate, hftEndTDate) &&
							nextTDate >= hftBeginTDate && nextTDate <= hftEndTDate)
						{
							// Preload replay-owned buckets before choosing the session clock;
							// this must not resize query-cache vectors backing borrowed slices.
							checkAllHftData(nextTDate);
							if (_terminated)
								break;
							uint64_t hftNaturalTime = _tick_begin_time;
							if (!resolve_hft_session_begin_clock(nextTDate, hftBeginTDate,
								hftNaturalTime, hasHftEvents))
							{
								_terminated = true;
								break;
							}
							useHftClock = hasHftEvents;
							if (useHftClock)
							{
								sessionDate = static_cast<uint32_t>(hftNaturalTime / 1000000000);
								const uint32_t actionTime = static_cast<uint32_t>(hftNaturalTime % 1000000000);
								sessionTime = actionTime / 100000;
								sessionSecs = actionTime % 100000;
							}
						}
					}

					if (!useHftClock)
					{
						const uint64_t beginTimeofDay = _bd_mgr.getBoundaryTime(sInfo->id(), nextTDate, true, true);
						sessionDate = static_cast<uint32_t>(beginTimeofDay / 10000);
						sessionTime = static_cast<uint32_t>(beginTimeofDay % 10000);
					}
					_cur_date = sessionDate;
					_cur_time = sessionTime;
					_cur_secs = sessionSecs;

					WTSLogger::debug("Tradingday {} begins", nextTDate);
					_cur_tdate = nextTDate;
					_listener->handle_session_begin(nextTDate);
					check_cache_days();
					_opened_tdate = nextTDate;
				}
			}			

			uint64_t curBarTime = (uint64_t)_cur_date * 10000 + _cur_time;
			if (_tick_enabled)
			{
				//如果开启了tick回放,则直接回放tick数据
				//如果tick回放失败，说明tick数据不存在，则需要模拟tick
				// Freeze close eligibility before real HFT callbacks can add a new
				// series for this main-bar close.
				freeze_sim_tick_batch(nextDate, nextTime);
				_bar_real_tick_codes.clear();
				_tick_simulated = true;
				const bool dispatchedRealTick = replayHftDatas(curBarTime, nextBarTime);
				_tick_simulated = !dispatchedRealTick;
				// A Level2 callback may stop the replay. Do not synthesize OHLC
				// ticks or close bars for the interrupted interval.
				if (_terminated)
					break;
			}

			if (!_tick_enabled)
			{
				checkUnbars();
			}

			_cur_date = nextDate;
			_cur_time = nextTime;
			_cur_secs = 0;

			bool isEndTDate = (sInfo->offsetTime(_cur_time, false) >= sInfo->getCloseTime(true));

			/*
			 *	By Wesley @ 2022.06.23
			 *	tick数据模拟的机制完善
			 *	主要将所有当前应该闭合的bar，按照开高低收的顺序同步模拟tick
			 *	但是这样也是有漏洞的，那就是如果K线周期不统一，如m1和m5同时订阅
			 *	会出现m5在最后一分钟才模拟tick的问题
			 *	不过，真的要精确回测，请使用逐tick回测
			 *	目前这个方案已经算是比较好的了
			 */
			for(int i = 0; i < 4; i++)
			{
				if (_tick_enabled || _tick_simulated)
					simTicks(nextDate, nextTime, (isDay || isEndTDate) ? nextTDate : 0, i);

				if (!_tick_enabled)
					simTickWithUnsubBars(curBarTime, nextBarTime, (isDay || isEndTDate) ? nextTDate : 0, i);
			}

			onMinuteEnd(nextDate, nextTime, (isDay || isEndTDate) ? nextTDate : 0, _tick_simulated);

			replayed_barcnt += 1;

			if(sInfo->isLastOfSection(nextTime))
				_listener->handle_section_end(nextDate, nextTime);

			if (isEndTDate && _closed_tdate != _cur_tdate && !isLastReplayBar)
			{
				WTSLogger::debug("Tradingday {} ends", _cur_tdate);
				_listener->handle_session_end(_cur_tdate);
				_closed_tdate = _cur_tdate;
				_day_cache.clear();
			}
			else if (isEndTDate && _closed_tdate != _cur_tdate && isLastReplayBar)
				finalSessionEndDeferred = true;

			notify_state(barsList->_code.c_str(), barsList->_period, barsList->_times, _begin_time, _end_time, replayed_barcnt*100.0 / total_barcnt);

			if (barsList->_cursor >= barsList->_bars.size())
			{
				WTSLogger::log_raw(LL_INFO, "All back data replayed, replaying done");
				break;
			}
		}
		else
		{
			WTSLogger::log_raw(LL_ERROR, "No back data initialized, replaying canceled");
			break;
		}
	}

	// The final main bar is a close boundary, not the end of the requested tick
	// interval. Drain the remaining in-range HFT events through the requested
	// trading date without advancing the bar series or synthesizing another bar.
	if (replayed_barcnt > 0 && !_terminated && _tick_enabled && lastMainBarTime != 0)
	{
		uint32_t beginTDate = 0;
		uint32_t endTDate = 0;
		if (get_hft_trading_date_range(beginTDate, endTDate))
		{
			auto cacheHftDataForDate = [&](uint32_t tDate)
			{
				// checkAllHftData is replay-bucket-only; historical query slices
				// keep their original cache storage and lifetime contract.
				return checkAllHftData(tDate);
			};

			uint32_t flushTDate = std::max(_cur_tdate, beginTDate);
			uint64_t flushStartTime = lastMainBarTime;
			for (; flushTDate <= endTDate && !_terminated; flushTDate = TimeUtils::getNextDate(flushTDate))
			{
				const bool hasData = cacheHftDataForDate(flushTDate);
				if (_terminated)
					break;
				if (hasData)
				{
					if (flushTDate != _cur_tdate)
					{
						if (_closed_tdate != _cur_tdate)
						{
							_listener->handle_session_end(_cur_tdate);
							_closed_tdate = _cur_tdate;
							_day_cache.clear();
						}
						_cur_tdate = flushTDate;
						uint64_t beginTime = _bd_mgr.getBoundaryTime(sInfo->id(), flushTDate, true, true);
						if (beginTime == 0)
							beginTime = static_cast<uint64_t>(flushTDate) * 10000 + _range_begin_hhmm;
						uint32_t sessionDate = static_cast<uint32_t>(beginTime / 10000);
						uint32_t sessionTime = static_cast<uint32_t>(beginTime % 10000);
						uint32_t sessionSecs = 0;
						if (_tick_enabled)
						{
							uint64_t hftNaturalTime = _tick_begin_time;
							bool hasHftEvents = false;
							if (!resolve_hft_session_begin_clock(flushTDate, beginTDate,
								hftNaturalTime, hasHftEvents))
							{
								_terminated = true;
								break;
							}
							if (hasHftEvents)
							{
								sessionDate = static_cast<uint32_t>(hftNaturalTime / 1000000000);
								const uint32_t actionTime = static_cast<uint32_t>(hftNaturalTime % 1000000000);
								sessionTime = actionTime / 100000;
								sessionSecs = actionTime % 100000;
							}
						}
						_cur_date = sessionDate;
						_cur_time = sessionTime;
						_cur_secs = sessionSecs;
						_listener->handle_session_begin(flushTDate);
						_opened_tdate = flushTDate;
						check_cache_days();
					}

					_tick_simulated = true;
					_bar_real_tick_codes.clear();
					replayHftDatas(flushStartTime, UINT64_MAX);
					if (flushTDate < endTDate)
					{
						_listener->handle_session_end(flushTDate);
						_closed_tdate = flushTDate;
						_day_cache.clear();
					}
				}

				flushStartTime = static_cast<uint64_t>(_range_begin_date) * 10000 + _range_begin_hhmm;
				if (flushTDate == endTDate)
					break;
			}
		}
	}
	if (finalSessionEndDeferred && _closed_tdate != _cur_tdate)
	{
		_listener->handle_session_end(_cur_tdate);
		_closed_tdate = _cur_tdate;
		_day_cache.clear();
	}

	if (_terminated)
		WTSLogger::debug("Replaying by bars terminated forcely");

	notify_state(barsList->_code.c_str(), barsList->_period, barsList->_times, _begin_time, _end_time, 100);
	if (_notifier)
		_notifier->notifyEvent("BT_END");

	if (_closed_tdate != _cur_tdate)
	{
		WTSLogger::debug("Tradingday {} ends", _cur_tdate);
		_listener->handle_session_end(_cur_tdate);
	}

	if (bNeedDump)
	{
		dump_btstate(barsList->_code.c_str(), barsList->_period, barsList->_times, _begin_time, _end_time, 100.0, ticker.nano_seconds());
	}

	_listener->handle_replay_done();
}

void HisDataReplayer::run_by_tasks(bool /*bNeedDump*/ /* = false */)
{
	if (!_has_bar_minutes)
	{
		WTSLogger::info("Replay range [{},{}] contains no whole minute; task replay is done", _begin_time, _end_time);
		if (_listener)
			_listener->handle_replay_done();
		if (_notifier)
			_notifier->notifyEvent("BT_END");
		return;
	}

	_cur_date = static_cast<uint32_t>(_bar_begin_time / 10000);
	_cur_time = static_cast<uint32_t>(_bar_begin_time % 10000);
	_cur_secs = 0;
	_cur_tdate = _bd_mgr.calcTradingDate(
		(strlen(_task->_session) == 0) ? DEFAULT_SESSIONID : _task->_session,
		_cur_date, _cur_time, true);

	//时间调度任务不为空,则按照时间调度任务回放
	WTSSessionInfo* sInfo = NULL;
	const char* DEF_SESS = (strlen(_task->_session) == 0) ? DEFAULT_SESSIONID : _task->_session;
	sInfo = _bd_mgr.getSession(DEF_SESS);
	WTSLogger::info("Start to backtest with task frequency from {}...", _begin_time);

	//分钟即任务和日级别任务分开写
	if (_task->_period != TPT_Minute)
	{
		uint32_t endtime = TimeUtils::getNextMinute(_task->_time, -1);
		bool bIsPreDay = endtime > _task->_time;
		if (bIsPreDay)
			_cur_date = TimeUtils::getNextDate(_cur_date, -1);

		for (; !_terminated;)
		{
			bool fired = false;
			//获取上一个交易日的日期
			uint32_t preTDate = TimeUtils::getNextDate(_cur_tdate, -1);
			if (_cur_time == endtime)
			{
				if (!_bd_mgr.isHoliday(_task->_trdtpl, _cur_date, true))
				{
					uint32_t weekDay = TimeUtils::getWeekDay(_cur_date);


					bool bHasHoliday = false;
					uint32_t days = 1;
					while (_bd_mgr.isHoliday(_task->_trdtpl, preTDate, true))
					{
						bHasHoliday = true;
						preTDate = TimeUtils::getNextDate(preTDate, -1);
						days++;
					}
					uint32_t preWD = TimeUtils::getWeekDay(preTDate);

					switch (_task->_period)
					{
					case TPT_None:
						break;
					case TPT_Daily:
						fired = true;
						break;
					case TPT_Minute:
						break;
					case TPT_Monthly:
						// Monthly task day is a day-of-month (1..31), not a YYMMDD value.
						if (_cur_date % 100 == _task->_day)
							fired = true;
						else if (bHasHoliday)
						{
							// If the scheduled calendar date fell in the closed interval
							// after the previous trading date, run it on the first open date.
							for (uint32_t holidayDate = TimeUtils::getNextDate(preTDate, 1);
								holidayDate < _cur_date;
								holidayDate = TimeUtils::getNextDate(holidayDate, 1))
							{
								if (holidayDate % 100 == _task->_day &&
									_bd_mgr.isHoliday(_task->_trdtpl, holidayDate, true))
								{
									fired = true;
									break;
								}
							}
						}
						break;
					case TPT_Weekly:
						//if (preWD < _task->_day && weekDay >= _task->_day)
						//	fired = true;
						if (weekDay == _task->_day)
							fired = true;
						else if (bHasHoliday)
						{
							if (days >= 7 && weekDay > _task->_day)
							{
								fired = true;
							}
							else if (preWD > weekDay && weekDay > _task->_day)
							{
								//上一个交易日的星期大于今天的星期,说明换了一周了
								fired = true;
							}
							else if (preWD < _task->_day && weekDay > _task->_day)
							{
								fired = true;
							}
						}
						break;
					case TPT_Yearly:
						// Yearly task day is MMDD. Compare actual calendar dates so a
						// holiday at the year boundary can be shifted to the next session.
						if (_cur_date % 10000 == _task->_day)
							fired = true;
						else if (bHasHoliday)
						{
							for (uint32_t holidayDate = TimeUtils::getNextDate(preTDate, 1);
								holidayDate < _cur_date;
								holidayDate = TimeUtils::getNextDate(holidayDate, 1))
							{
								if (holidayDate % 10000 == _task->_day &&
									_bd_mgr.isHoliday(_task->_trdtpl, holidayDate, true))
								{
									fired = true;
									break;
								}
							}
						}
						break;
					}
				}
			}

			if (fired)
			{
				const uint64_t fireTime = static_cast<uint64_t>(_cur_date) * 10000 + endtime;
				if (fireTime < _bar_begin_time || fireTime > _bar_end_time)
					fired = false;
			}

			if (!fired)
			{
				//调整时间
				//如果当前时间小于任务时间,则直接赋值即可
				//如果当前时间大于任务时间,则至少要等下一天
				if (_cur_time < endtime)
				{
					_cur_time = endtime;
					continue;
				}

				uint32_t newTDate = _bd_mgr.calcTradingDate(DEF_SESS, _cur_date, _cur_time, true);

				if (newTDate != _cur_tdate)
				{
					_cur_tdate = newTDate;
					if (_listener)
						_listener->handle_session_begin(newTDate);
					check_cache_days();
					if (_listener)
						_listener->handle_session_end(newTDate);
				}
			}
			else
			{
				//用前一分钟作为结束时间
				uint32_t curDate = _cur_date;
				uint32_t curTime = endtime;
				bool bEndSession = sInfo->offsetTime(curTime, true) >= sInfo->getCloseTime(true);
				if (_listener)
					_listener->handle_session_begin(_cur_tdate);
				check_cache_days();

				/*
				 *	日频及以上周期任务(TPT_Daily/Monthly/Weekly/Yearly)触发时, 也按开高低收同步模拟tick
				 *	否则撮合器拿不到任何价格, 策略信号将永不成交
				 *	与下方 TPT_Minute 分支的 tick 模拟机制保持一致
				 */
				for (int i = 0; i < 4; i++)
				{
					simTicks(curDate, curTime, (bEndSession ? _cur_tdate : preTDate), i);
				}
				onMinuteEnd(curDate, curTime, bEndSession ? _cur_tdate : preTDate);
				if (_listener)
					_listener->handle_session_end(_cur_tdate);
			}

			_cur_date = TimeUtils::getNextDate(_cur_date);
			_cur_time = endtime;
			_cur_tdate = _bd_mgr.calcTradingDate(DEF_SESS, _cur_date, _cur_time, true);

			uint64_t nextTime = (uint64_t)_cur_date * 10000 + _cur_time;
			if (nextTime > _bar_end_time)
			{
				WTSLogger::log_raw(LL_INFO, "Backtesting with task frequency is done");
				if (_listener)
				{
					_listener->handle_session_end(_cur_tdate);
					_listener->handle_replay_done();
					if (_notifier)
						_notifier->notifyEvent("BT_END");
				}

				break;
			}
		}
	}
	else
	{
		if (_task->_time == 0)
		{
			WTSLogger::error("Minute task interval must be greater than zero");
			if (_listener)
				_listener->handle_replay_done();
			if (_notifier)
				_notifier->notifyEvent("BT_END");
			return;
		}

		if (_listener)
			_listener->handle_session_begin(_cur_tdate);

		check_cache_days();

		for (; !_terminated;)
		{
			//要考虑到跨日的情况
			uint32_t mins = sInfo->timeToMinutes(_cur_time);
			const uint32_t tradingMins = sInfo->getTradingMins();
			if (mins == INVALID_UINT32)
			{
				// Start times inside a session break are moved to the next section
				// boundary before step alignment. After the final section, keep the
				// existing close/next-trading-day path below.
				const uint32_t adjustedMins = sInfo->timeToMinutes(_cur_time, true);
				if (adjustedMins != INVALID_UINT32)
				{
					mins = adjustedMins;
					_cur_time = sInfo->minuteToTime(mins, true);
				}
			}

			if (mins != INVALID_UINT32 && mins < tradingMins)
			{
				const uint32_t remainder = mins % _task->_time;
				if (remainder != 0)
				{
					const uint64_t step = _task->_time;
					const uint64_t alignedMins =
						(static_cast<uint64_t>(mins) / step + 1) * step;
					mins = alignedMins >= tradingMins
						? tradingMins
						: static_cast<uint32_t>(alignedMins);
					_cur_time = sInfo->minuteToTime(mins);
				}
			}

			bool bNewTDate = false;
			if (mins < sInfo->getTradingMins())
			{
				const uint64_t dispatchTime = static_cast<uint64_t>(_cur_date) * 10000 + _cur_time;
				if (dispatchTime > _bar_end_time)
				{
					WTSLogger::log_raw(LL_INFO, "Backtesting with task frequency is done");
					if (_listener)
					{
						_listener->handle_session_end(_cur_tdate);
						_listener->handle_replay_done();
						if (_notifier)
							_notifier->notifyEvent("BT_END");
					}
					break;
				}
				if (dispatchTime >= _bar_begin_time)
				{
				/*
				 *	By Wesley @ 2022.06.23
				 *	tick数据模拟的机制完善
				 *	主要将所有当前应该闭合的bar，按照开高低收的顺序同步模拟tick
				 *	但是这样也是有漏洞的，那就是如果K线周期不统一，如m1和m5同时订阅
				 *	会出现m5在最后一分钟才模拟tick的问题
				 *	不过，真的要精确回测，请使用逐tick回测
				 *	目前这个方案已经算是比较好的了
				 */
				for (int i = 0; i < 4; i++)
				{
					simTicks(_cur_date, _cur_time, _cur_tdate, i);
				}

				onMinuteEnd(_cur_date, _cur_time, 0);
				}
			}
			else
			{
				bNewTDate = true;
				mins = sInfo->getTradingMins();
				_cur_time = sInfo->getCloseTime();
				const uint64_t dispatchTime = static_cast<uint64_t>(_cur_date) * 10000 + _cur_time;
				if (dispatchTime > _bar_end_time)
				{
					WTSLogger::log_raw(LL_INFO, "Backtesting with task frequency is done");
					if (_listener)
					{
						_listener->handle_session_end(_cur_tdate);
						_listener->handle_replay_done();
						if (_notifier)
							_notifier->notifyEvent("BT_END");
					}
					break;
				}

				if (dispatchTime >= _bar_begin_time)
				{

				/*
				 *	By Wesley @ 2022.06.23
				 *	tick数据模拟的机制完善
				 *	主要将所有当前应该闭合的bar，按照开高低收的顺序同步模拟tick
				 *	但是这样也是有漏洞的，那就是如果K线周期不统一，如m1和m5同时订阅
				 *	会出现m5在最后一分钟才模拟tick的问题
				 *	不过，真的要精确回测，请使用逐tick回测
				 *	目前这个方案已经算是比较好的了
				 */
				for (int i = 0; i < 4; i++)
				{
					simTicks(_cur_date, _cur_time, _cur_tdate, i);
				}
				onMinuteEnd(_cur_date, _cur_time, _cur_tdate);

				if (_listener)
					_listener->handle_session_end(_cur_tdate);
				}
			}


			if (bNewTDate)
			{
				//换日了
				mins = std::min(_task->_time, tradingMins);
				uint32_t nextTDate = _bd_mgr.getNextTDate(_task->_trdtpl, _cur_tdate, 1, true);

				if (sInfo->getOffsetMins() != 0)
				{
					if (sInfo->getOffsetMins() > 0)
					{
						//真实时间后移,说明夜盘算作下一天的
						_cur_date = _cur_tdate;
						_cur_tdate = nextTDate;
					}
					else
					{
						//真实时间前移,说明夜盘是上一天的,这种情况就不需要动了
						_cur_tdate = nextTDate;
						_cur_date = _cur_tdate;
					}
				}
				else
				{
					//白盘会话(时间偏移为0), 夜盘算作当天, 这里必须整体推进交易日
					//否则 _cur_date 会停留在旧交易日, 导致 nextTime 永不前进, 回测死循环
					_cur_date = nextTDate;
					_cur_tdate = nextTDate;
				}

				_cur_time = sInfo->minuteToTime(mins);

				if (_listener)
					_listener->handle_session_begin(nextTDate);

				check_cache_days();
			}
			else
			{
				if (_task->_time > tradingMins - mins)
					mins = tradingMins;
				else
					mins += _task->_time;

				uint32_t newTime = sInfo->minuteToTime(mins);
				bool bNewDay = newTime < _cur_time;
				if (bNewDay)
					_cur_date = TimeUtils::getNextDate(_cur_date);

				uint32_t dayMins = _cur_time / 100 * 60 + _cur_time % 100;
				uint32_t nextDMins = newTime / 100 * 60 + newTime % 100;

				//是否到了一个新的小节
				bool bNewSec = (nextDMins - dayMins > _task->_time) && !bNewDay;

				while (bNewSec && _bd_mgr.isHoliday(_task->_trdtpl, _cur_date, true))
					_cur_date = TimeUtils::getNextDate(_cur_date);

				_cur_time = newTime;
			}

			uint64_t nextTime = (uint64_t)_cur_date * 10000 + _cur_time;
			if (nextTime > _bar_end_time)
			{
				WTSLogger::log_raw(LL_INFO, "Backtesting with task frequency is done");
				if (_listener)
				{
					_listener->handle_session_end(_cur_tdate);
					_listener->handle_replay_done();
					if (_notifier)
						_notifier->notifyEvent("BT_END");
				}
				break;
			}
		}
	}
}

void HisDataReplayer::freeze_sim_tick_batch(uint32_t uDate, uint32_t uTime)
{
	const uint64_t nowTime = static_cast<uint64_t>(uDate) * 10000 + uTime;
	if (_sim_tick_batch_active && _sim_tick_batch_time == nowTime)
		return;

	_bar_real_tick_codes.clear();
	_sim_tick_batch_active = true;
	_sim_tick_batch_time = nowTime;
	_sim_tick_batch_close_keys.clear();
	_sim_tick_batch_ticker_keys.clear();
	for (const auto& entry : _bars_cache)
	{
		if (entry.second != NULL && entry.second->_notify_close)
			_sim_tick_batch_close_keys.insert(entry.first);
	}
	for (const auto& entry : _ticker_keys)
	{
		if (_sim_tick_batch_close_keys.find(entry.second) != _sim_tick_batch_close_keys.end())
			_sim_tick_batch_ticker_keys[entry.first] = entry.second;
	}
}

void HisDataReplayer::simTicks(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */, int pxType /* = 0 */)
{
	//这里应该触发检查
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	freeze_sim_tick_batch(uDate, uTime);
	// The four OHLC callbacks for one bar share a frozen subscription view.
	// A callback may register another series, but it cannot receive a partial
	// OHLC sequence or the close event for the bar currently being simulated.
	struct SimBarsSnapshot
	{
		std::string key;
		BarsListPtr bars;
		uint32_t cursor;
	};
	std::vector<SimBarsSnapshot> barsSnapshot;
	barsSnapshot.reserve(_bars_cache.size());
	for (const auto& entry : _bars_cache)
	{
		if (entry.second != NULL)
			barsSnapshot.push_back({ entry.first, entry.second, entry.second->_cursor });
	}
	auto position_at_or_after_now = [&](const BarsListPtr& barsList)
	{
		if (barsList == NULL)
			return;
		if (barsList->_period == KP_DAY)
		{
			const uint32_t targetDate = endTDate == 0 ? _cur_tdate : endTDate;
			auto it = std::lower_bound(barsList->_bars.begin(), barsList->_bars.end(), targetDate,
				[](const WTSBarStruct& bar, uint32_t date) { return bar.date < date; });
			barsList->_cursor = static_cast<uint32_t>(it - barsList->_bars.begin());
		}
		else
		{
			const uint64_t targetTime = nowTime - 199000000000ull;
			auto it = std::lower_bound(barsList->_bars.begin(), barsList->_bars.end(), targetTime,
				[](const WTSBarStruct& bar, uint64_t time) { return bar.time < time; });
			barsList->_cursor = static_cast<uint32_t>(it - barsList->_bars.begin());
		}
	};
	auto restore_event_cursors = [&]()
	{
		for (const SimBarsSnapshot& saved : barsSnapshot)
		{
			if (saved.cursor == UINT_MAX && saved.bars->_cursor != UINT_MAX)
			{
				if (saved.bars->_notify_close)
					position_at_or_after_now(saved.bars);
				else
					saved.bars->_cursor = UINT_MAX;
			}
			else
				saved.bars->_cursor = saved.cursor == UINT_MAX
					? UINT_MAX
					: static_cast<uint32_t>(std::min<std::size_t>(saved.cursor, saved.bars->_bars.size()));
		}
		for (const auto& entry : _bars_cache)
		{
			if (entry.second == NULL)
				continue;
			const auto wasPresent = std::find_if(barsSnapshot.begin(), barsSnapshot.end(),
				[&entry](const SimBarsSnapshot& saved) { return saved.key == entry.first; });
			if (wasPresent == barsSnapshot.end() && entry.second->_notify_close)
				position_at_or_after_now(entry.second);
		}
	};
	auto dispatch_sim_tick = [&](const char* stdCode, WTSTickData* curTick)
	{
		WTSReleaseGuard<WTSTickData> tickGuard(curTick);
		try
		{
			if (curTick != NULL)
			{
				_tick_simulated = true;
				_listener->handle_tick(stdCode, curTick, pxType);
			}
		}
		catch (...)
		{
			restore_event_cursors();
			throw;
		}
		restore_event_cursors();
	};

	// Strategy callbacks can insert new cached series and rehash _bars_cache.
	// Iterate a stable snapshot and keep history-view reads from consuming the
	// independent close-event cursor.
	for (const SimBarsSnapshot& entry : barsSnapshot)
	{
		const std::string& cacheKey = entry.key;
		BarsListPtr barsList = entry.bars;
		if (_sim_tick_batch_close_keys.find(cacheKey) == _sim_tick_batch_close_keys.end())
			continue;
		// A real Tick suppresses synthetic OHLC for every series of the same
		// contract in this main-bar interval, while other contracts retain fallback.
		if (_bar_real_tick_codes.find(base_tick_code(barsList->_code)) != _bar_real_tick_codes.end())
			continue;
		auto tickerIt = _sim_tick_batch_ticker_keys.find(barsList->_code);
		const bool isFrozenTicker = tickerIt != _sim_tick_batch_ticker_keys.end()
			&& tickerIt->second == cacheKey;
		if (barsList->_period != KP_DAY)
		{
			// Simulation walks to the current source bar, but the replay cursor
			// belongs to onMinuteEnd's close-event stream. Advancing it here can
			// consume bars (including several bars due at one timestamp) before
			// their close callbacks are collected.
			std::size_t simCursor = barsList->_cursor;
			if (simCursor != UINT_MAX && barsList->_bars.size() > simCursor)
			{
				for(;;)
				{
					WTSBarStruct& nextBar = barsList->_bars[simCursor];

					/*
					 *	By Wesley @ 2023.05.05
					 *	如果没有禁止0成交模拟tick，或者K线成交量不为0，就可以模拟tick
					 */
					bool bCanSim = !_nosim_if_notrade || !decimal::eq(nextBar.vol, 0.0);

					uint64_t barTime = 199000000000 + nextBar.time;
					if (barTime == nowTime && bCanSim)
					{
						if (isFrozenTicker)
						{
							//开高低收
							WTSTickStruct& curTS = _day_cache[barsList->_code];
							strcpy(curTS.code, barsList->_code.c_str());
							curTS.action_date = _cur_date;
							curTS.action_time = _cur_time * 100000;

							double newPx = 0.0;
							if (pxType == 0)
								newPx = nextBar.open;
							else if (pxType == 1)
								newPx = nextBar.high;
							else if (pxType == 2)
								newPx = nextBar.low;
							else if (pxType == 3)
								newPx = nextBar.close;

							curTS.price = newPx;
							curTS.volume = nextBar.vol;
							curTS.total_volume += nextBar.vol;

							//更新开高低三个字段
							if (decimal::eq(curTS.open, 0))
								curTS.open = curTS.price;
							curTS.high = max(curTS.price, curTS.high);
							if (decimal::eq(curTS.low, 0))
								curTS.low = curTS.price;
							else
								curTS.low = min(curTS.price, curTS.low);

							update_price(barsList->_code.c_str(), curTS.price);
							dispatch_sim_tick(barsList->_code.c_str(), WTSTickData::create(curTS));
						}

						break;
					}
					else if (barTime < nowTime)
					{
						simCursor++;

						if (simCursor == barsList->_bars.size())
							break;

						continue;
					}
					else
					{
						break;
					}
				} 
			}
		}
		else
		{
			if (barsList->_bars.size() > barsList->_cursor)
			{
				for (;;)
				{
					WTSBarStruct& nextBar = barsList->_bars[barsList->_cursor];

					if (nextBar.date == endTDate)
					{
						if (isFrozenTicker)
						{
							CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(barsList->_code.c_str(), &_hot_mgr);
							[[maybe_unused]] WTSCommodityInfo* commInfo = _bd_mgr.getCommodity(cInfo._exchg, cInfo._product);

							std::string realCode = barsList->_code;
							if (cInfo.isExright())
								realCode = realCode.substr(0, realCode.size() - 1);

							WTSSessionInfo* sInfo = get_session_info(realCode.c_str(), true);
							uint32_t curTime = sInfo->getCloseTime();
							//开高低收
							WTSTickStruct curTS;
							strcpy(curTS.code, realCode.c_str());
							curTS.action_date = _cur_date;
							curTS.action_time = curTime * 100000;

							double newPx = 0.0;
							if (pxType == 0)
								newPx = nextBar.open;
							else if (pxType == 1)
								newPx = nextBar.high;
							else if (pxType == 2)
								newPx = nextBar.low;
							else if (pxType == 3)
								newPx = nextBar.close;

							curTS.price = newPx;
							curTS.volume = nextBar.vol;
							update_price(barsList->_code.c_str(), curTS.price);
							dispatch_sim_tick(realCode.c_str(), WTSTickData::create(curTS));
						}

						break;
					}
					else if (nextBar.date < endTDate)
					{
						barsList->_cursor++;

						if (barsList->_cursor == barsList->_bars.size())
							break;
					}
					else
					{
						break;
					}
				}
			}
		}
	}
}

void HisDataReplayer::simTickWithUnsubBars(uint64_t stime, uint64_t nowTime, uint32_t endTDate /* = 0 */, int pxType /* = 0 */)
{
	//uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	[[maybe_unused]] uint32_t uDate = (uint32_t)(stime / 10000);
	struct CallbackCursorSnapshot
	{
		BarsListPtr bars;
		uint32_t cursor;
	};
	std::vector<CallbackCursorSnapshot> cursorSnapshot;
	cursorSnapshot.reserve(_bars_cache.size());
	for (const auto& entry : _bars_cache)
	{
		if (entry.second != NULL)
			cursorSnapshot.push_back({ entry.second, entry.second->_cursor });
	}
	auto position_at_or_after_now = [&](const BarsListPtr& barsList)
	{
		if (barsList == NULL)
			return;
		if (barsList->_period == KP_DAY)
		{
			const uint32_t targetDate = endTDate == 0 ? _cur_tdate : endTDate;
			auto it = std::lower_bound(barsList->_bars.begin(), barsList->_bars.end(), targetDate,
				[](const WTSBarStruct& bar, uint32_t date) { return bar.date < date; });
			barsList->_cursor = static_cast<uint32_t>(it - barsList->_bars.begin());
		}
		else
		{
			const uint64_t targetTime = nowTime - 199000000000ull;
			auto it = std::lower_bound(barsList->_bars.begin(), barsList->_bars.end(), targetTime,
				[](const WTSBarStruct& bar, uint64_t time) { return bar.time < time; });
			barsList->_cursor = static_cast<uint32_t>(it - barsList->_bars.begin());
		}
	};
	auto restore_event_cursors = [&]()
	{
		for (const CallbackCursorSnapshot& saved : cursorSnapshot)
		{
			if (saved.cursor == UINT_MAX && saved.bars->_cursor != UINT_MAX)
			{
				if (saved.bars->_notify_close)
					position_at_or_after_now(saved.bars);
				else
					saved.bars->_cursor = UINT_MAX;
			}
			else
				saved.bars->_cursor = saved.cursor == UINT_MAX
					? UINT_MAX
					: static_cast<uint32_t>(std::min<std::size_t>(saved.cursor, saved.bars->_bars.size()));
		}
	};
	auto dispatch_unsub_tick = [&](const char* stdCode, WTSTickData* curTick)
	{
		WTSReleaseGuard<WTSTickData> tickGuard(curTick);
		try
		{
			_listener->handle_tick(stdCode, curTick, pxType);
		}
		catch (...)
		{
			restore_event_cursors();
			throw;
		}
		restore_event_cursors();
	};

	for (auto& item : _unbars_cache)
	{
		BarsListPtr& barsList = (BarsListPtr&)item.second;
		if (barsList->_period != KP_DAY)
		{
			//如果历史数据指标不在尾部, 说明是回测模式, 要继续回放历史数据
			if (barsList->_bars.size() > barsList->_cursor)
			{
				for (;;)
				{
					WTSBarStruct& nextBar = barsList->_bars[barsList->_cursor];

					/*
					 *	By Wesley @ 2023.05.05
					 *	如果没有禁止0成交模拟tick，或者K线成交量不为0，就可以模拟tick
					 */
					bool bCanSim = !_nosim_if_notrade || !decimal::eq(nextBar.vol, 0.0);

					uint64_t barTime = 199000000000 + nextBar.time;
					if (barTime == nowTime && bCanSim)
					{
						//开高低收
						WTSTickStruct& curTS = _day_cache[barsList->_code];
						strcpy(curTS.code, barsList->_code.c_str());
						curTS.action_date = _cur_date;
						curTS.action_time = _cur_time * 100000;

						curTS.volume = nextBar.vol;
						double newPx = 0.0;
						if (pxType == 0)
							newPx = nextBar.open;
						else if (pxType == 1)
							newPx = nextBar.high;
						else if (pxType == 2)
							newPx = nextBar.low;
						else if (pxType == 3)
							newPx = nextBar.close;

						curTS.price = newPx;
						//更新开高低三个字段
						if (decimal::eq(curTS.open, 0))
							curTS.open = curTS.price;
						curTS.high = max(curTS.price, curTS.high);
						if (decimal::eq(curTS.low, 0))
							curTS.low = curTS.price;
						else
							curTS.low = min(curTS.price, curTS.low);


						WTSTickData* curTick = WTSTickData::create(curTS);
						dispatch_unsub_tick(barsList->_code.c_str(), curTick);
						break;
					}
					else if (barTime < nowTime)
					{
						barsList->_cursor++;

						if (barsList->_cursor == barsList->_bars.size())
							break;

						continue;
					}
					else
					{
						break;
					}
				}
			}
		}
		else
		{
			if (barsList->_bars.size() > barsList->_cursor)
			{
				for (;;)
				{
					WTSBarStruct& nextBar = barsList->_bars[barsList->_cursor];

					if (nextBar.date == endTDate)
					{
						CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(barsList->_code.c_str(), &_hot_mgr);
						WTSCommodityInfo* commInfo = _bd_mgr.getCommodity(cInfo._exchg, cInfo._product);
						if (commInfo == NULL)
						{
							WTSLogger::warn("Skipping simulated daily tick for {}: commodity metadata is unavailable", barsList->_code);
							barsList->_cursor++;
							break;
						}

						std::string realCode = barsList->_code;
						if (commInfo->isStock() && cInfo.isExright())
							realCode = realCode.substr(0, realCode.size() - 1);

						WTSSessionInfo* sInfo = get_session_info(realCode.c_str(), true);
						if (sInfo == NULL)
						{
							WTSLogger::warn("Skipping simulated daily tick for {}: session metadata is unavailable", realCode);
							barsList->_cursor++;
							break;
						}
						uint32_t curTime = sInfo->getOpenTime();
						//开高低收
						WTSTickStruct curTS;
						strcpy(curTS.code, realCode.c_str());
						curTS.action_date = _cur_date;
						curTS.action_time = curTime * 100000;

						curTS.volume = nextBar.vol;
						double newPx = 0.0;
						if (pxType == 0)
							newPx = nextBar.open;
						else if (pxType == 1)
							newPx = nextBar.high;
						else if (pxType == 2)
							newPx = nextBar.low;
						else if (pxType == 3)
							newPx = nextBar.close;

						curTS.price = newPx;
						//更新开高低三个字段
						if (decimal::eq(curTS.open, 0))
							curTS.open = curTS.price;
						curTS.high = max(curTS.price, curTS.high);
						if (decimal::eq(curTS.low, 0))
							curTS.low = curTS.price;
						else
							curTS.low = min(curTS.price, curTS.low);

						WTSTickData* curTick = WTSTickData::create(curTS);
						dispatch_unsub_tick(realCode.c_str(), curTick);

						break;
					}
					else if (nextBar.date < endTDate)
					{
						barsList->_cursor++;

						if (barsList->_cursor == barsList->_bars.size())
							break;

						continue;
					}
					else
					{
						break;
					}

				}
			}
		}
	}
}

uint64_t HisDataReplayer::getNextTickTime(uint32_t curTDate, uint64_t stime /* = UINT64_MAX */)
{
	uint64_t nextTime = UINT64_MAX;
	for (auto& v : _tick_sub_map)
	{
		const char* stdCode = v.first.c_str();
		if (!checkTicks(stdCode, curTDate))
			continue;
		auto& tickList = _ticks_cache[stdCode];
		if (tickList._cursor == 0 || tickList._count == 0 || tickList._items.empty())
			continue;

		uint64_t lowerBound = _tick_begin_time;
		if (stime != UINT64_MAX)
		{
			const uint32_t startDate = static_cast<uint32_t>(stime / 10000);
			const uint32_t startTime = static_cast<uint32_t>(stime % 10000);
			lowerBound = std::max(lowerBound, static_cast<uint64_t>(startDate) * 1000000000 +
				static_cast<uint64_t>(startTime) * 100000);
		}
		bool hasReplayableTick = false;
		const std::size_t count = std::min(tickList._count, tickList._items.size());
		const std::size_t first = tickList._cursor == UINT_MAX ? 0 : tickList._cursor - 1;
		for (std::size_t index = first; index < count; ++index)
		{
			const WTSTickStruct& item = tickList._items[index];
			const uint64_t itemTime = static_cast<uint64_t>(item.action_date) * 1000000000 + item.action_time;
			if (itemTime >= lowerBound && itemTime < _tick_end_exclusive_time)
			{
				hasReplayableTick = true;
				break;
			}
		}
		if (!hasReplayableTick)
			continue;

		WTSSessionInfo* sInfo = get_session_info(stdCode, true);
		if (sInfo == NULL)
		{
			WTSLogger::error("Cannot replay in-range ticks for {} on trading date {} without a session",
				stdCode, curTDate);
			_terminated = true;
			return UINT64_MAX;
		}

		if (tickList._cursor == UINT_MAX)
		{
			if (stime == UINT64_MAX)
			{
				/*
				 *	如果stime为UINT64_MAX
				 *	则说明还没有初始化
				 *	所以要确定第一笔是什么
				 */
				for(tickList._cursor = 1; ; tickList._cursor++)
				{
					//如果时间一直不满足，则直接跳出循环
					if (tickList._cursor > tickList._count || tickList._cursor > tickList._items.size())
						break;

					uint32_t tickMin = tickList._items[tickList._cursor-1].action_time / 100000;
					if (sInfo->isInTradingTime(tickMin))
					{
						break;
					}
				}
			}
			else
			{
				uint32_t uDate = (uint32_t)(stime / 10000);
				uint32_t uTime = (uint32_t)(stime % 10000);

				WTSTickStruct curTick;
				curTick.action_date = uDate;
				curTick.action_time = uTime * 100000;

				auto tit = std::lower_bound(tickList._items.begin(), tickList._items.end(), curTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
					if (a.action_date != b.action_date)
						return a.action_date < b.action_date;
					else
						return a.action_time < b.action_time;
				});

				std::size_t idx = tit - tickList._items.begin();
				tickList._cursor = idx + 1;
			}
		}

		if (tickList._cursor == 0 || tickList._cursor == UINT_MAX ||
			tickList._cursor > tickList._count || tickList._cursor > tickList._items.size())
			continue;

		uint32_t nextActionTime = tickList._items[tickList._cursor - 1].action_time;
		//By Wesley @ 2022.03.06
		//检查一下时间戳，如果不是交易时间的，就不回放了
		uint32_t nextMinTime = nextActionTime / 100000;
		/*
		 *	By Wesley @ 2023.05.05
		 *	这里做了一个调整，主要是针对小节中间出现的tick数据
		 *	部分数据源可能会落地小节中间的数据，导致时间戳不在交易时间
		 *	因此先判断时间戳是否超出收盘时间，如果超出也不回放tick了
		 *	然后如果tick数据处于小节之间，但是不在交易时间，则指针一直步进
		 *	这次修改主要针对Issue#104
		 */
		//超过收盘时间就跳过了
		if(sInfo->offsetTime(nextMinTime, false) > sInfo->getCloseTime(true))
			continue;

		while (!sInfo->isInTradingTime(nextMinTime))
		{
			tickList._cursor++;
			if (tickList._cursor > tickList._count || tickList._cursor > tickList._items.size())
				break;

			nextActionTime = tickList._items[tickList._cursor - 1].action_time;
			nextMinTime = nextActionTime / 100000;
		}

		clamp_hft_cursor(tickList);

		if (tickList._cursor == 0 || tickList._cursor == UINT_MAX ||
			tickList._cursor > tickList._count || tickList._cursor > tickList._items.size())
			continue;

		const WTSTickStruct& nextTick = tickList._items[tickList._cursor - 1];
		uint64_t lastTime = (uint64_t)nextTick.action_date * 1000000000 + nextTick.action_time;

		nextTime = min(lastTime, nextTime);
	}

	return nextTime;
}


uint64_t HisDataReplayer::getNextTransTime(uint32_t curTDate, uint64_t stime /* = UINT64_MAX */)
{
	uint64_t nextTime = UINT64_MAX;
	for (auto v : _trans_sub_map)
	{
		const char* stdCode = v.first.c_str();
		if (!checkTransactions(stdCode, curTDate))
			continue;

		auto& itemList = _trans_cache[stdCode];
		if (itemList._cursor == UINT_MAX)
		{
			if (stime == UINT64_MAX)
				itemList._cursor = 1;
			else
			{
				uint32_t uDate = (uint32_t)(stime / 10000);
				uint32_t uTime = (uint32_t)(stime % 10000);

				WTSTransStruct curItem;
				curItem.action_date = uDate;
				curItem.action_time = uTime * 100000;

				auto tit = std::lower_bound(itemList._items.begin(), itemList._items.end(), curItem, [](const WTSTransStruct& a, const WTSTransStruct& b) {
					if (a.action_date != b.action_date)
						return a.action_date < b.action_date;
					else
						return a.action_time < b.action_time;
				});

				std::size_t idx = tit - itemList._items.begin();
				itemList._cursor = idx + 1;
			}
		}

		clamp_hft_cursor(itemList);
		if (itemList._cursor == 0 || itemList._cursor == UINT_MAX ||
			itemList._cursor > itemList._count || itemList._cursor > itemList._items.size())
			continue;

		const auto& nextItem = itemList._items[itemList._cursor - 1];
		uint64_t lastTime = (uint64_t)nextItem.action_date * 1000000000 + nextItem.action_time;

		nextTime = min(lastTime, nextTime);
	}

	return nextTime;
}


uint64_t HisDataReplayer::getNextOrdDtlTime(uint32_t curTDate, uint64_t stime /* = UINT64_MAX */)
{
	uint64_t nextTime = UINT64_MAX;
	for (auto v : _orddtl_sub_map)
	{
		const char* stdCode = v.first.c_str();
		if (!checkOrderDetails(stdCode, curTDate))
			continue;

		auto& itemList = _orddtl_cache[stdCode];
		if (itemList._cursor == UINT_MAX)
		{
			if (stime == UINT64_MAX)
				itemList._cursor = 1;
			else
			{
				uint32_t uDate = (uint32_t)(stime / 10000);
				uint32_t uTime = (uint32_t)(stime % 10000);

				WTSOrdDtlStruct curItem;
				curItem.action_date = uDate;
				curItem.action_time = uTime * 100000;

				auto tit = std::lower_bound(itemList._items.begin(), itemList._items.end(), curItem, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
					if (a.action_date != b.action_date)
						return a.action_date < b.action_date;
					else
						return a.action_time < b.action_time;
				});

				std::size_t idx = tit - itemList._items.begin();
				itemList._cursor = idx + 1;
			}
		}

		clamp_hft_cursor(itemList);
		if (itemList._cursor == 0 || itemList._cursor == UINT_MAX ||
			itemList._cursor > itemList._count || itemList._cursor > itemList._items.size())
			continue;

		const auto& nextItem = itemList._items[itemList._cursor - 1];
		uint64_t lastTime = (uint64_t)nextItem.action_date * 1000000000 + nextItem.action_time;

		nextTime = min(lastTime, nextTime);
	}

	return nextTime;
}


uint64_t HisDataReplayer::getNextOrdQueTime(uint32_t curTDate, uint64_t stime /* = UINT64_MAX */)
{
	uint64_t nextTime = UINT64_MAX;
	for (auto v : _ordque_sub_map)
	{
		const char* stdCode = v.first.c_str();
		if (!checkOrderQueues(stdCode, curTDate))
			continue;

		auto& itemList = _ordque_cache[stdCode];
		if (itemList._cursor == UINT_MAX)
		{
			if (stime == UINT64_MAX)
				itemList._cursor = 1;
			else
			{
				uint32_t uDate = (uint32_t)(stime / 10000);
				uint32_t uTime = (uint32_t)(stime % 10000);

				WTSOrdQueStruct curItem;
				curItem.action_date = uDate;
				curItem.action_time = uTime * 100000;

				auto tit = std::lower_bound(itemList._items.begin(), itemList._items.end(), curItem, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
					if (a.action_date != b.action_date)
						return a.action_date < b.action_date;
					else
						return a.action_time < b.action_time;
				});

				std::size_t idx = tit - itemList._items.begin();
				itemList._cursor = idx + 1;
			}
		}

		clamp_hft_cursor(itemList);
		if (itemList._cursor == 0 || itemList._cursor == UINT_MAX ||
			itemList._cursor > itemList._count || itemList._cursor > itemList._items.size())
			continue;

		const auto& nextItem = itemList._items[itemList._cursor - 1];
		uint64_t lastTime = (uint64_t)nextItem.action_date * 1000000000 + nextItem.action_time;

		nextTime = min(lastTime, nextTime);
	}

	return nextTime;
}

uint64_t HisDataReplayer::natural_time_from_boundary(uint64_t boundary) const
{
	if (boundary == 0)
		return 0;
	const uint32_t date = static_cast<uint32_t>(boundary / 10000);
	const uint32_t time = static_cast<uint32_t>(boundary % 10000);
	if (time / 100 >= 24 || time % 100 >= 60)
		return 0;
	return static_cast<uint64_t>(date) * 1000000000ull + static_cast<uint64_t>(time) * 100000ull;
}

bool HisDataReplayer::loadReplayHftDate(uint32_t tDate, const std::string& stdCode, bool ticks,
	bool details, bool queues, bool transactions)
{
	bool hasData = false;
	auto failedReadIsFatal = [this, tDate, &stdCode](uint32_t kind)
	{
		// The built-in DSB reader reports both an absent file and a failed block
		// decode as false. For local bin/DSB sources, inspect the exact candidate
		// paths after the read: absent with no filesystem error is a missing day;
		// a present file or an inspection error is an unreadable source and must
		// stop replay rather than silently turn into an empty bucket.
		if (_bt_loader != NULL)
		{
			WTSLogger::warn("Extended HFT loader returned false for {} on {}; its bool result cannot distinguish missing data from a load error",
				stdCode.c_str(), tDate);
			return;
		}

		std::vector<fs::path> paths;
		if (kind == 0 && _mode == "csv")
		{
			paths.emplace_back(fs::path(_base_dir) / "bin/ticks" /
				fmtutil::format<128>("{}_tick_{}.dsb", stdCode.c_str(), tDate));
		}
		else if (_mode != "csv")
		{
			CodeHelper::CodeInfo info = CodeHelper::extractStdCode(stdCode.c_str(), &_hot_mgr);
			std::string dir;
			switch (kind)
			{
			case 0: dir = "ticks"; break;
			case 1: dir = "orders"; break;
			case 2: dir = "queue"; break;
			default: dir = "trans"; break;
			}
			if (kind == 0 && std::strlen(info._ruletag) > 0)
			{
				const std::string wrapped = fmtutil::format<128>("{}_{}", info._product, info._ruletag);
				paths.emplace_back(fs::path(_base_dir) / "his" / dir / info._exchg /
					std::to_string(tDate) / (wrapped + ".dsb"));
				const std::string customRaw = _hot_mgr.getCustomRawCode(info._ruletag, info.stdCommID(), tDate);
				if (!customRaw.empty())
					paths.emplace_back(fs::path(_base_dir) / "his" / dir / info._exchg /
						std::to_string(tDate) / (customRaw + ".dsb"));
			}
			paths.emplace_back(fs::path(_base_dir) / "his" / dir / info._exchg /
				std::to_string(tDate) / (std::string(info._code) + ".dsb"));
		}

		bool present = false;
		bool inspectionFailed = false;
		for (const fs::path& path : paths)
		{
			std::error_code ec;
			const bool exists = fs::exists(path, ec);
			if (ec)
				inspectionFailed = true;
			else
				present = present || exists;
		}
		if (present || inspectionFailed)
		{
			WTSLogger::error("HFT data read failed for {} on {} although a candidate file exists or cannot be inspected; stopping replay",
				stdCode.c_str(), tDate);
			_terminated = true;
			return;
		}
		if (_mode != "bin" && _mode != "csv")
			WTSLogger::warn("HFT loader mode {} returned false for {} on {}; local paths are not authoritative, so missing data and read failure remain indistinguishable",
				_mode.c_str(), stdCode.c_str(), tDate);
	};
	auto cachedHasData = [tDate](const auto& dates)
	{
		auto it = dates.find(tDate);
		return it != dates.end() && it->second._count > 0;
	};
	auto initialize = [](auto& dataList, const std::string& code, uint32_t date)
	{
		dataList._code = code;
		dataList._date = date;
		dataList._cursor = UINT_MAX;
		dataList._query_cursor = UINT_MAX;
		dataList._count = 0;
	};
	auto sortItems = [](auto& dataList)
	{
		std::stable_sort(dataList._items.begin(), dataList._items.end(), [](const auto& left, const auto& right) {
			if (left.action_date != right.action_date)
				return left.action_date < right.action_date;
			return left.action_time < right.action_time;
		});
		dataList._count = std::min(dataList._count, dataList._items.size());
	};

	if (ticks)
	{
		auto& dates = _replay_ticks_cache[stdCode];
		if (dates.find(tDate) == dates.end())
		{
			HftDataList<WTSTickStruct> dataList;
			initialize(dataList, stdCode, tDate);
			bool loaded = false;
			if (_bt_loader != NULL)
				loaded = cacheRawTicksFromLoader(dataList, stdCode.c_str(), tDate);
			else if (_mode == "csv")
				loaded = cacheRawTicksFromCSV(dataList, stdCode.c_str(), tDate);
			else
				loaded = cacheRawTicksFromBin(dataList, stdCode.c_str(), tDate);
			if (!loaded)
			{
				failedReadIsFatal(0);
				dataList._items.clear();
			}
			sortItems(dataList);
			dates.emplace(tDate, std::move(dataList));
		}
		hasData = cachedHasData(dates) || hasData;
	}
	if (details)
	{
		auto& dates = _replay_orddtl_cache[stdCode];
		if (dates.find(tDate) == dates.end())
		{
			HftDataList<WTSOrdDtlStruct> dataList;
			initialize(dataList, stdCode, tDate);
			bool loaded = false;
			if (_mode != "csv")
				loaded = cacheRawOrdDtlFromBin(dataList, stdCode.c_str(), tDate);
			else
				WTSLogger::error("Cannot use stock level2 data in csv mode!");
			if (!loaded)
			{
				if (_mode != "csv") failedReadIsFatal(1);
				dataList._items.clear();
			}
			sortItems(dataList);
			dates.emplace(tDate, std::move(dataList));
		}
		hasData = cachedHasData(dates) || hasData;
	}
	if (queues)
	{
		auto& dates = _replay_ordque_cache[stdCode];
		if (dates.find(tDate) == dates.end())
		{
			HftDataList<WTSOrdQueStruct> dataList;
			initialize(dataList, stdCode, tDate);
			bool loaded = false;
			if (_mode != "csv")
				loaded = cacheRawOrdQueFromBin(dataList, stdCode.c_str(), tDate);
			else
				WTSLogger::error("Cannot use stock level2 data in csv mode!");
			if (!loaded)
			{
				if (_mode != "csv") failedReadIsFatal(2);
				dataList._items.clear();
			}
			sortItems(dataList);
			dates.emplace(tDate, std::move(dataList));
		}
		hasData = cachedHasData(dates) || hasData;
	}
	if (transactions)
	{
		auto& dates = _replay_trans_cache[stdCode];
		if (dates.find(tDate) == dates.end())
		{
			HftDataList<WTSTransStruct> dataList;
			initialize(dataList, stdCode, tDate);
			bool loaded = false;
			if (_mode != "csv")
				loaded = cacheRawTransFromBin(dataList, stdCode.c_str(), tDate);
			else
				WTSLogger::error("Cannot use stock level2 data in csv mode!");
			if (!loaded)
			{
				if (_mode != "csv") failedReadIsFatal(3);
				dataList._items.clear();
			}
			sortItems(dataList);
			dates.emplace(tDate, std::move(dataList));
		}
		hasData = cachedHasData(dates) || hasData;
	}
	return hasData;
}

uint64_t HisDataReplayer::next_hft_session_boundary(uint32_t /*tradingDate*/, uint64_t afterTime)
{
	std::set<std::string> codes;
	auto collect = [&codes](const StraSubMap& subscriptions)
	{
		for (const auto& entry : subscriptions)
			codes.insert(entry.first);
	};
	collect(_tick_sub_map);
	collect(_orddtl_sub_map);
	collect(_ordque_sub_map);
	collect(_trans_sub_map);

	uint64_t earliest = std::numeric_limits<uint64_t>::max();
	for (const std::string& code : codes)
	{
		CodeHelper::CodeInfo info = CodeHelper::extractStdCode(code.c_str(), &_hot_mgr);
		const char* commId = info.stdCommID();
		WTSSessionInfo* session = get_session_info(code.c_str(), true);
		if (commId == NULL || *commId == '\0' || session == NULL)
			continue;

		const uint32_t naturalDate = static_cast<uint32_t>(afterTime / 1000000000ull);
		const uint32_t actionTime = static_cast<uint32_t>((afterTime % 1000000000ull) / 100000ull);
		if (session->getTradingMins() == 1440)
		{
			const uint32_t currentTDate = _bd_mgr.calcTradingDate(commId, naturalDate, actionTime, false);
			uint64_t sessionBegin = 0;
			uint64_t sessionEnd = 0;
			if (currentTDate == 0 || !continuous_session_natural_bounds(currentTDate,
				session->getOffsetMins(), sessionBegin, sessionEnd))
			{
				WTSLogger::error("Cannot determine 24-hour HFT session boundary for {} after {} with offset {}",
					code.c_str(), afterTime, session->getOffsetMins());
				_terminated = true;
				return std::numeric_limits<uint64_t>::max();
			}
			if (sessionEnd <= afterTime)
			{
				WTSLogger::error("24-hour HFT session boundary {} for {} did not advance after {}",
					sessionEnd, code.c_str(), afterTime);
				_terminated = true;
				return std::numeric_limits<uint64_t>::max();
			}
			earliest = std::min(earliest, sessionEnd);
			continue;
		}
		uint32_t currentTDate = _bd_mgr.calcTradingDate(commId, naturalDate, actionTime, false);
		if (currentTDate == 0)
		{
			WTSLogger::error("Cannot map natural time {} to a trading date for HFT subscription {}", afterTime, code.c_str());
			_terminated = true;
			return std::numeric_limits<uint64_t>::max();
		}
		uint32_t candidate = _bd_mgr.getNextTDate(commId, currentTDate, 1, false);
		uint64_t previousBoundary = 0;
		while (candidate > currentTDate)
		{
			const uint64_t boundary = _bd_mgr.getBoundaryTime(commId, candidate, false, true);
			const uint64_t natural = natural_time_from_boundary(boundary);
			if (natural == 0)
			{
				WTSLogger::error("Cannot determine next HFT session boundary for {} on trading date {}",
					code.c_str(), candidate);
				_terminated = true;
				return std::numeric_limits<uint64_t>::max();
			}
			if (natural > afterTime)
			{
				earliest = std::min(earliest, natural);
				break;
			}
			if (natural <= previousBoundary)
			{
				WTSLogger::error("HFT session boundaries for {} did not advance after {}", code.c_str(), afterTime);
				_terminated = true;
				return std::numeric_limits<uint64_t>::max();
			}
			previousBoundary = natural;
			const uint32_t following = _bd_mgr.getNextTDate(commId, candidate, 1, false);
			if (following <= candidate)
			{
				WTSLogger::error("Trading-date calendar did not advance for HFT subscription {}", code.c_str());
				_terminated = true;
				return std::numeric_limits<uint64_t>::max();
			}
			candidate = following;
		}
		if (candidate <= currentTDate)
		{
			WTSLogger::error("Trading-date calendar did not advance for HFT subscription {}", code.c_str());
			_terminated = true;
			return std::numeric_limits<uint64_t>::max();
		}
	}
	return earliest;
}

void HisDataReplayer::pruneReplayHftData(uint64_t beforeTime)
{
	auto prune = [this, beforeTime](auto& cache)
	{
		for (auto codeIt = cache.begin(); codeIt != cache.end(); )
		{
			CodeHelper::CodeInfo info = CodeHelper::extractStdCode(codeIt->first.c_str(), &_hot_mgr);
			const char* commId = info.stdCommID();
			WTSSessionInfo* session = get_session_info(codeIt->first.c_str(), true);
			for (auto dateIt = codeIt->second.begin(); dateIt != codeIt->second.end(); )
			{
				uint64_t closeBoundary = 0;
				if (session != NULL && session->getTradingMins() == 1440)
				{
					uint64_t openBoundary = 0;
					continuous_session_natural_bounds(dateIt->first, session->getOffsetMins(),
						openBoundary, closeBoundary);
				}
				else if (commId != NULL && *commId != '\0')
					closeBoundary = natural_time_from_boundary(_bd_mgr.getBoundaryTime(commId, dateIt->first, false, false));
				const std::size_t count = std::min(dateIt->second._count, dateIt->second._items.size());
				const bool consumed = dateIt->second._cursor != UINT_MAX && dateIt->second._cursor > count;
				const bool empty = count == 0;
				if ((empty || consumed) && closeBoundary != 0 && closeBoundary <= beforeTime)
					dateIt = codeIt->second.erase(dateIt);
				else
					++dateIt;
			}
			if (codeIt->second.empty())
				codeIt = cache.erase(codeIt);
			else
				++codeIt;
		}
	};
	prune(_replay_ticks_cache);
	prune(_replay_orddtl_cache);
	prune(_replay_ordque_cache);
	prune(_replay_trans_cache);
}

bool HisDataReplayer::replayHftInterval(uint64_t stime, uint64_t etime, bool& dispatchedRealTick,
	const ReplaySubscriptionSnapshot& subscriptions)
{
	dispatchedRealTick = false;
	if (stime >= etime)
		return true;
	if (static_cast<uint32_t>(stime / 1000000000ull) !=
		static_cast<uint32_t>((etime - 1) / 1000000000ull))
	{
		WTSLogger::error("HFT interval [{}, {}) exceeds its bounded natural-date window", stime, etime);
		_terminated = true;
		return false;
	}
	const std::set<std::string>& tickCodes = subscriptions.ticks;
	const std::set<std::string>& detailCodes = subscriptions.details;
	const std::set<std::string>& queueCodes = subscriptions.queues;
	const std::set<std::string>& transactionCodes = subscriptions.transactions;
	std::set<std::string> allCodes;
	allCodes.insert(tickCodes.begin(), tickCodes.end());
	allCodes.insert(detailCodes.begin(), detailCodes.end());
	allCodes.insert(queueCodes.begin(), queueCodes.end());
	allCodes.insert(transactionCodes.begin(), transactionCodes.end());
	if (allCodes.empty() || stime >= etime)
		return true;

	for (const std::string& code : allCodes)
	{
		CodeHelper::CodeInfo info = CodeHelper::extractStdCode(code.c_str(), &_hot_mgr);
		const char* commId = info.stdCommID();
		WTSSessionInfo* codeSession = get_session_info(code.c_str(), true);
		if (commId == NULL || *commId == '\0' || codeSession == NULL)
		{
			WTSLogger::error("Cannot schedule HFT buckets for {} without a session", code.c_str());
			_terminated = true;
			return false;
		}
		const bool naturalDaySession = codeSession->getTradingMins() == 1440;

		// Map every HHMM touched by this at-most-one-natural-day window. Session
		// mappings can be non-monotonic around midnight/holiday boundaries, so
		// endpoint-only mapping is insufficient. Minute sampling is bounded by
		// 1440 entries regardless of replay range length; the adjacent valid
		// trading dates cover overnight boundaries, and natural session bounds
		// below filter the resulting candidates before any data is loaded.
		std::set<uint32_t> mappedTradingDates;
		uint32_t naturalDate = static_cast<uint32_t>(stime / 1000000000ull);
		uint32_t actionTime = static_cast<uint32_t>((stime % 1000000000ull) / 100000ull);
		uint64_t minuteStart = static_cast<uint64_t>(naturalDate) * 1000000000ull +
			static_cast<uint64_t>(actionTime) * 100000ull;
		while (minuteStart < etime)
		{
			const uint32_t mapped = _bd_mgr.calcTradingDate(commId, naturalDate, actionTime, false);
			if (mapped == 0)
			{
				WTSLogger::error("Cannot map natural replay minute {}{} to a trading date for {}",
					naturalDate, actionTime, code.c_str());
				_terminated = true;
				return false;
			}
			mappedTradingDates.insert(mapped);

			const uint32_t followingTime = TimeUtils::getNextMinute(static_cast<int32_t>(actionTime));
			uint32_t followingDate = naturalDate;
			if (followingTime <= actionTime)
				followingDate = TimeUtils::getNextDate(naturalDate, 1);
			const uint64_t followingMinute = static_cast<uint64_t>(followingDate) * 1000000000ull +
				static_cast<uint64_t>(followingTime) * 100000ull;
			if (followingMinute <= minuteStart)
			{
				WTSLogger::error("Cannot advance bounded HFT calendar scan for {}", code.c_str());
				_terminated = true;
				return false;
			}
			naturalDate = followingDate;
			actionTime = followingTime;
			minuteStart = followingMinute;
		}

		std::set<uint32_t> candidateDates;
		for (uint32_t mapped : mappedTradingDates)
		{
			if (naturalDaySession)
			{
				// calcTradingDate's 1440-minute branch maps these to natural
				// calendar dates, adjusted only by the session offset. Preserve
				// weekend and holiday buckets rather than consulting a market calendar.
				candidateDates.insert(mapped);
				continue;
			}
			if (_bd_mgr.isTradingDate(commId, mapped, false))
				candidateDates.insert(mapped);
			const uint32_t previous = _bd_mgr.getPrevTDate(commId, mapped, 1, false);
			const uint32_t following = _bd_mgr.getNextTDate(commId, mapped, 1, false);
			if (previous == 0 || following == 0 || previous >= mapped || following <= mapped)
			{
				WTSLogger::error("Trading-date calendar cannot bound natural replay window for {}", code.c_str());
				_terminated = true;
				return false;
			}
			candidateDates.insert(previous);
			candidateDates.insert(following);
		}
		if (candidateDates.empty())
		{
			WTSLogger::error("Trading-date calendar cannot bound natural replay window for {}", code.c_str());
			_terminated = true;
			return false;
		}

		for (uint32_t date : candidateDates)
		{
			uint64_t sessionBegin = 0;
			uint64_t sessionEnd = 0;
			if (naturalDaySession)
			{
				if (!continuous_session_natural_bounds(date, codeSession->getOffsetMins(),
					sessionBegin, sessionEnd))
				{
					WTSLogger::error("Invalid continuous HFT session boundaries for {} on {} with offset {}",
						code.c_str(), date, codeSession->getOffsetMins());
					_terminated = true;
					return false;
				}
			}
			else
			{
				sessionBegin = natural_time_from_boundary(_bd_mgr.getBoundaryTime(commId, date, false, true));
				sessionEnd = natural_time_from_boundary(_bd_mgr.getBoundaryTime(commId, date, false, false));
			}
			if (sessionBegin == 0 || sessionEnd == 0 || sessionBegin >= sessionEnd)
			{
				WTSLogger::error("Invalid session boundaries for {} on HFT trading date {}", code.c_str(), date);
				_terminated = true;
				return false;
			}
			// Ordinary market sessions include their close timestamp, so a range
			// beginning exactly at close must inspect that session's bucket. A
			// continuous 24-hour bucket is naturally half-open at its date cut.
			const bool overlaps = naturalDaySession
				? (sessionBegin < etime && sessionEnd > stime)
				: (sessionBegin < etime && sessionEnd >= stime);
			if (overlaps)
			{
				loadReplayHftDate(date, code, tickCodes.count(code) != 0,
					detailCodes.count(code) != 0, queueCodes.count(code) != 0,
					transactionCodes.count(code) != 0);
				if (_terminated)
					return false;
			}
		}
	}

	struct ReplayEvent { uint64_t time; std::size_t round; uint32_t kind; std::string code; uint32_t tDate; std::size_t index; void* list; };
	struct LaterEvent
	{
		bool operator()(const ReplayEvent& left, const ReplayEvent& right) const
		{
			if (left.time != right.time) return left.time > right.time;
			// Preserve the legacy same-millisecond rounds: the first record from
			// every D/T/K/Q stream is delivered before any stream's second record.
			if (left.round != right.round) return left.round > right.round;
			if (left.kind != right.kind) return left.kind > right.kind;
			if (left.code != right.code) return left.code > right.code;
			if (left.tDate != right.tDate) return left.tDate > right.tDate;
			return left.index > right.index;
		}
	};
	std::priority_queue<ReplayEvent, std::vector<ReplayEvent>, LaterEvent> events;
	auto itemTime = [](const auto& item) {
		return static_cast<uint64_t>(item.action_date) * 1000000000ull + item.action_time;
	};
	auto enqueue = [&](auto& list, const std::string& code, uint32_t date, uint32_t kind)
	{
		const std::size_t count = std::min(list._count, list._items.size());
		if (list._cursor == UINT_MAX)
		{
			auto it = std::lower_bound(list._items.begin(), list._items.begin() + count, stime,
				[&itemTime](const auto& item, uint64_t time) { return itemTime(item) < time; });
			list._cursor = static_cast<std::size_t>(it - list._items.begin()) + 1;
		}
		while (list._cursor > 0 && list._cursor <= count)
		{
			const auto& item = list._items[list._cursor - 1];
			const uint64_t time = itemTime(item);
			if (time < stime) { ++list._cursor; continue; }
			if (time >= etime) return;
			if (kind == 2)
			{
				WTSSessionInfo* session = get_session_info(code.c_str(), true);
				const uint32_t hhmm = item.action_time / 100000;
				const uint32_t minute = hhmm;
				const uint32_t second = (item.action_time % 100000) / 1000;
				const bool validActionTime = hhmm / 100 < 24 && hhmm % 100 < 60 && second < 60;
				if (session == NULL || (session->getTradingMins() != 1440 &&
					(session->offsetTime(minute, false) > session->getCloseTime(true) ||
						!session->isInTradingTime(minute))) || !validActionTime)
				{ ++list._cursor; continue; }
			}
			std::size_t round = 0;
			std::size_t previous = list._cursor - 1;
			while (previous > 0 && itemTime(list._items[previous - 1]) == time)
			{
				++round;
				--previous;
			}
			events.push(ReplayEvent{time, round, kind, code, date, list._cursor - 1, &list});
			return;
		}
	};
	auto enqueueCache = [&](auto& cache, const std::set<std::string>& codes, uint32_t kind)
	{
		for (const std::string& code : codes)
		{
			auto codeIt = cache.find(code);
			if (codeIt == cache.end()) continue;
			for (auto& dateEntry : codeIt->second)
				enqueue(dateEntry.second, code, dateEntry.first, kind);
		}
	};
	enqueueCache(_replay_orddtl_cache, detailCodes, 0);
	enqueueCache(_replay_trans_cache, transactionCodes, 1);
	enqueueCache(_replay_ticks_cache, tickCodes, 2);
	enqueueCache(_replay_ordque_cache, queueCodes, 3);

	while (!events.empty() && !_terminated)
	{
		const ReplayEvent event = events.top();
		events.pop();
		if (_has_last_replay_hft_time && event.time < _last_replay_hft_time)
		{
			WTSLogger::error("HFT replay event {} for {} arrived behind the dispatched natural-time watermark {}",
				event.time, event.code.c_str(), _last_replay_hft_time);
			_terminated = true;
			return false;
		}
		_cur_date = static_cast<uint32_t>(event.time / 1000000000ull);
		const uint32_t actionTime = static_cast<uint32_t>(event.time % 1000000000ull);
		_cur_time = actionTime / 100000;
		_cur_secs = actionTime % 100000;
		_last_replay_hft_time = event.time;
		_has_last_replay_hft_time = true;

		if (event.kind == 0)
		{
			auto& list = *static_cast<HftDataList<WTSOrdDtlStruct>*>(event.list);
			WTSOrdDtlStruct item = list._items[event.index]; list._cursor = event.index + 2;
			WTSOrdDtlData* data = WTSOrdDtlData::create(item); WTSReleaseGuard<WTSOrdDtlData> guard(data);
			if (data != NULL) { data->setCode(event.code.c_str()); _listener->handle_order_detail(event.code.c_str(), data); }
		}
		else if (event.kind == 1)
		{
			auto& list = *static_cast<HftDataList<WTSTransStruct>*>(event.list);
			WTSTransStruct item = list._items[event.index]; list._cursor = event.index + 2;
			WTSTransData* data = WTSTransData::create(item); WTSReleaseGuard<WTSTransData> guard(data);
			if (data != NULL) { data->setCode(event.code.c_str()); _listener->handle_transaction(event.code.c_str(), data); }
		}
		else if (event.kind == 2)
		{
			auto& list = *static_cast<HftDataList<WTSTickStruct>*>(event.list);
			WTSTickStruct item = list._items[event.index]; list._cursor = event.index + 2;
			WTSTickData* data = WTSTickData::create(item);
			if (data != NULL)
			{
				WTSReleaseGuard<WTSTickData> guard(data);
				if (data->trySetCode(event.code.c_str()))
				{
					update_price(event.code.c_str(), item.price);
					_bar_real_tick_codes.insert(base_tick_code(event.code));
					_tick_simulated = false;
					_listener->handle_tick(event.code.c_str(), data, 0);
					dispatchedRealTick = true;
				}
			}
		}
		else
		{
			auto& list = *static_cast<HftDataList<WTSOrdQueStruct>*>(event.list);
			WTSOrdQueStruct item = list._items[event.index]; list._cursor = event.index + 2;
			WTSOrdQueData* data = WTSOrdQueData::create(item); WTSReleaseGuard<WTSOrdQueData> guard(data);
			if (data != NULL) { data->setCode(event.code.c_str()); _listener->handle_order_queue(event.code.c_str(), data); }
		}
		if (_terminated) break;
		if (event.kind == 0) enqueue(_replay_orddtl_cache[event.code][event.tDate], event.code, event.tDate, event.kind);
		else if (event.kind == 1) enqueue(_replay_trans_cache[event.code][event.tDate], event.code, event.tDate, event.kind);
		else if (event.kind == 2) enqueue(_replay_ticks_cache[event.code][event.tDate], event.code, event.tDate, event.kind);
		else enqueue(_replay_ordque_cache[event.code][event.tDate], event.code, event.tDate, event.kind);
	}
	if (_terminated)
		return false;
	pruneReplayHftData(etime);
	return true;
}

bool HisDataReplayer::replayHftDatasNatural(uint64_t stime, uint64_t etime)
{
	if (_time_range_valid)
	{
		stime = std::max(stime, _tick_begin_time);
		etime = std::min(etime, _tick_end_exclusive_time);
	}
	if (stime >= etime || _terminated) return false;
	// Freeze the subscription view for the whole public replay interval. A
	// callback can request another series, but date chunks in this call must not
	// activate it halfway through an OHLC bar; the next replay interval will see
	// the new subscription.
	ReplaySubscriptionSnapshot subscriptions;
	auto snapshot = [](const StraSubMap& source, std::set<std::string>& destination)
	{
		for (const auto& entry : source)
			destination.insert(entry.first);
	};
	snapshot(_tick_sub_map, subscriptions.ticks);
	snapshot(_orddtl_sub_map, subscriptions.details);
	snapshot(_ordque_sub_map, subscriptions.queues);
	snapshot(_trans_sub_map, subscriptions.transactions);
	bool dispatchedRealTick = false;
	uint64_t cursor = stime;
	while (cursor < etime && !_terminated)
	{
		const uint32_t naturalDate = static_cast<uint32_t>(cursor / 1000000000ull);
		if (_has_replay_completed_natural_date && naturalDate < _replay_completed_natural_date)
		{
			WTSLogger::error("HFT replay interval moved back from natural date {} to {}; reset is required before replaying older data",
				_replay_completed_natural_date, naturalDate);
			_terminated = true;
			return dispatchedRealTick;
		}
		const uint32_t nextDate = TimeUtils::getNextDate(naturalDate, 1);
		const uint64_t nextMidnight = static_cast<uint64_t>(nextDate) * 1000000000ull;
		if (nextMidnight <= cursor)
		{
			WTSLogger::error("Cannot advance HFT replay natural-date window from {}", cursor);
			_terminated = true;
			return dispatchedRealTick;
		}
		const uint64_t chunkEnd = std::min(etime, nextMidnight);
		std::vector<std::pair<uint64_t, uint64_t>> completedRanges;
		if (_has_replay_completed_natural_date && naturalDate == _replay_completed_natural_date)
			completedRanges = _replay_completed_natural_ranges;

		auto mergeCompletedRange = [](std::vector<std::pair<uint64_t, uint64_t>>& ranges,
			uint64_t begin, uint64_t end)
		{
			ranges.emplace_back(begin, end);
			std::sort(ranges.begin(), ranges.end());
			std::vector<std::pair<uint64_t, uint64_t>> merged;
			merged.reserve(ranges.size());
			for (const auto& range : ranges)
			{
				if (merged.empty() || merged.back().second < range.first)
					merged.push_back(range);
				else
					merged.back().second = std::max(merged.back().second, range.second);
			}
			ranges.swap(merged);
		};

		uint64_t segmentBegin = cursor;
		while (segmentBegin < chunkEnd && !_terminated)
		{
			const auto containingRange = std::find_if(completedRanges.begin(), completedRanges.end(),
				[segmentBegin](const auto& range) {
					return range.first <= segmentBegin && segmentBegin < range.second;
				});
			if (containingRange != completedRanges.end())
			{
				segmentBegin = std::min(chunkEnd, containingRange->second);
				continue;
			}

			uint64_t segmentEnd = chunkEnd;
			for (const auto& range : completedRanges)
			{
				if (range.first > segmentBegin)
				{
					segmentEnd = std::min(segmentEnd, range.first);
					break;
				}
			}
			if (segmentEnd <= segmentBegin)
			{
				WTSLogger::error("Cannot advance uncovered HFT replay interval from {} to {}",
					segmentBegin, chunkEnd);
				_terminated = true;
				break;
			}

			bool chunkDispatchedRealTick = false;
			if (!replayHftInterval(segmentBegin, segmentEnd, chunkDispatchedRealTick, subscriptions) || _terminated)
				return dispatchedRealTick || chunkDispatchedRealTick;
			dispatchedRealTick = dispatchedRealTick || chunkDispatchedRealTick;

			mergeCompletedRange(completedRanges, segmentBegin, segmentEnd);
			_replay_completed_natural_date = naturalDate;
			_replay_completed_natural_ranges = completedRanges;
			_has_replay_completed_natural_date = true;
			segmentBegin = segmentEnd;
		}
		if (_terminated)
			return dispatchedRealTick;
		cursor = chunkEnd;
	}
	return dispatchedRealTick;
}

bool HisDataReplayer::replayHftDatasByDay(uint32_t /*curTDate*/, uint64_t stime, uint64_t etime)
{
	return replayHftDatasNatural(stime, etime);
}

bool HisDataReplayer::replayHftDatas(uint64_t stime, uint64_t etime)
{
	WTSLogger::log_raw(LL_DEBUG, "replaying hft data...");
	const uint64_t naturalBegin = (stime / 10000ull) * 1000000000ull + (stime % 10000ull) * 100000ull;
	const uint64_t naturalEnd = etime == UINT64_MAX ? _tick_end_exclusive_time
		: (etime / 10000ull) * 1000000000ull + (etime % 10000ull) * 100000ull;
	return replayHftDatasNatural(naturalBegin, naturalEnd);
}

void HisDataReplayer::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */, bool /*tickSimulated*/ /* = true */)
{
	//这里应该触发检查
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	const bool hasMatchingSimTickBatch = _sim_tick_batch_active && _sim_tick_batch_time == nowTime;

	// Snapshot the currently loaded series before callbacks: callbacks may query
	// existing caches or insert new ones while the unordered cache is in use.
	std::vector<BarsListPtr> replayBars;
	std::vector<std::string> replayKeys;
	replayBars.reserve(_bars_cache.size());
	replayKeys.reserve(_bars_cache.size());
	for (auto& entry : _bars_cache)
	{
		if (entry.second != NULL && entry.second->_notify_close
			&& (!hasMatchingSimTickBatch
				|| _sim_tick_batch_close_keys.find(entry.first) != _sim_tick_batch_close_keys.end()))
		{
			replayKeys.push_back(entry.first);
			replayBars.push_back(entry.second);
		}
	}
	struct BarCloseEvent
	{
		BarsListPtr bars;
		const char* period;
		uint32_t times;
		WTSBarStruct* bar;
	};
	std::vector<BarCloseEvent> dueEvents;
	std::vector<uint32_t> cursorSnapshot;
	cursorSnapshot.reserve(replayBars.size());
	auto position_after_now = [&](const std::string& key, const BarsListPtr& barsList)
	{
		if (barsList == NULL || barsList->_bars.empty() || barsList->_cursor == UINT_MAX)
			return;

		uint32_t nextIdx = locate_barindex(key, nowTime, false);
		if (nextIdx == UINT32_MAX || nextIdx >= barsList->_bars.size())
		{
			barsList->_cursor = (uint32_t)barsList->_bars.size();
			return;
		}

		const WTSBarStruct& nextBar = barsList->_bars[nextIdx];
		uint64_t nextBarTime;
		if (barsList->_period == KP_DAY)
			nextBarTime = get_daily_bar_close_time(barsList, nextBar);
		else
			nextBarTime = 199000000000ull + nextBar.time;
		if (nextBarTime <= nowTime)
			nextIdx++;
		barsList->_cursor = (uint32_t)std::min<std::size_t>(nextIdx, barsList->_bars.size());
	};
	auto advance_late_series = [&]()
	{
		for (auto& entry : _bars_cache)
		{
			BarsListPtr& barsList = entry.second;
			if (barsList == NULL || !barsList->_notify_close || barsList->_bars.empty() || barsList->_cursor == UINT_MAX)
				continue;
			if (std::find(replayBars.begin(), replayBars.end(), barsList) != replayBars.end())
				continue;
			position_after_now(entry.first, barsList);
		}
	};

	for (BarsListPtr& barsList : replayBars)
	{
		barsList->mark();
		[[maybe_unused]] double factor = 1.0;// barsList->_factor;
		if (barsList->_cursor == UINT_MAX)
			continue;
		if (barsList->_cursor > barsList->_bars.size())
			barsList->_cursor = (uint32_t)barsList->_bars.size();

		std::size_t idx = barsList->_cursor;
		if (barsList->_period != KP_DAY)
		{
			while (idx < barsList->_bars.size())
			{
				WTSBarStruct& nextBar = barsList->_bars[idx];
				const uint64_t barTime = 199000000000ull + nextBar.time;
				if (barTime > nowTime)
					break;

				if (_has_bar_minutes && barTime >= _bar_begin_time && barTime <= _bar_end_time)
				{
					uint32_t times = barsList->_times;
					const char* period = "m";
					if (times == PERIOD_TIMES_HOUR)
					{
						period = "h";
						times = 1;
					}
					else if (times == PERIOD_TIMES_HALF)
					{
						period = "f";
						times = 1;
					}
					else if (barsList->_period == KP_Minute5)
						times *= 5;
					dueEvents.push_back({ barsList, period, times, &nextBar });
				}
				idx++;
			}
		}
		else
		{
			while (idx < barsList->_bars.size())
			{
				WTSBarStruct& nextBar = barsList->_bars[idx];
				const uint64_t barTime = get_daily_bar_close_time(barsList, nextBar);
				if (barTime > nowTime)
					break;
				if (_has_bar_minutes && barTime >= _bar_begin_time && barTime <= _bar_end_time)
					dueEvents.push_back({ barsList, "d", barsList->_times, &nextBar });
				idx++;
			}
		}
		barsList->_cursor = (uint32_t)idx;
	}

	// Advance every pre-existing series before exposing any callback. A query
	// during one close can then see all bars closed at this timestamp, while the
	// saved event list still delivers every due bar exactly once.
	for (const BarCloseEvent& event : dueEvents)
	{
		cursorSnapshot.clear();
		for (const BarsListPtr& barsList : replayBars)
			cursorSnapshot.push_back(barsList->_cursor);

		_listener->handle_bar_close(event.bars->_code.c_str(), event.period, event.times, event.bar);

		for (std::size_t i = 0; i < replayBars.size(); i++)
		{
			if (cursorSnapshot[i] == UINT_MAX && replayBars[i]->_cursor != UINT_MAX)
				position_after_now(replayKeys[i], replayBars[i]);
			else
				replayBars[i]->_cursor = cursorSnapshot[i];
		}
	}
	advance_late_series();

	for (auto it = _unbars_cache.begin(); it != _unbars_cache.end(); it++)
	{
		BarsListPtr& barsList = (BarsListPtr&)it->second;
		[[maybe_unused]] double factor = 1.0;// barsList->_factor;
		if (barsList->_period != KP_DAY)
		{
			if (barsList->_bars.size() > barsList->_cursor)
			{
				for (;;)
				{
					WTSBarStruct& nextBar = barsList->_bars[barsList->_cursor];

					uint64_t barTime = 199000000000 + nextBar.time;
					if (barTime > nowTime)
						break;

					barsList->_cursor++;

					if (barsList->_cursor == barsList->_bars.size())
						break;
				}
			}
		}
		else
		{
			if (barsList->_bars.size() > barsList->_cursor)
			{
				for (;;)
				{
					WTSBarStruct& nextBar = barsList->_bars[barsList->_cursor];

					if (nextBar.date > endTDate)
						break;

					barsList->_cursor++;

					if (barsList->_cursor >= barsList->_bars.size())
						break;
				}
			}
		}
	}

	// The close event set has been collected and dispatched. New subscriptions
	// become eligible when the next simulated OHLC batch starts.
	if (hasMatchingSimTickBatch)
	{
		_sim_tick_batch_active = false;
		_sim_tick_batch_time = 0;
		_sim_tick_batch_close_keys.clear();
		_sim_tick_batch_ticker_keys.clear();
	}

	if (_listener)
		_listener->handle_schedule(uDate, uTime);
}

WTSKlineSlice* HisDataReplayer::get_kline_slice(const char* stdCode, const char* period, uint32_t count, uint32_t times /* = 1 */, bool isMain /* = false */)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}#{}", stdCode, period, times);

	//if(!_tick_enabled)
	//不做判断,主要为了防止没有tick数据,而采用第二方案
	{
		if(_ticker_keys.find(stdCode) == _ticker_keys.end())
			_ticker_keys[stdCode] = key;
		else
		{
			std::string oldKey = _ticker_keys[stdCode];
			oldKey = oldKey.substr(strlen(stdCode) + 1);
			if (strcmp(period, "m") == 0 && oldKey.at(0) == 'd')
			{
				_ticker_keys[stdCode] = key;
				_min_period = period;
			}
			else if (oldKey.at(0) == period[0] && times < convert::to_uint32(oldKey.substr(2).c_str()))
			{
				_ticker_keys[stdCode] = key;
				_min_period = period;
			}
		}

		auto len = strlen(stdCode);
		char lastCh = stdCode[len - 1];
		if(lastCh == SUFFIX_HFQ || lastCh == SUFFIX_QFQ)
		{
			//如果是复权数据，则要把原始数据放到需要的列表中，最后再做检查
			std::string tickCode(stdCode, len - 1);
			_unsubbed_in_need.insert(tickCode);
		}
	}

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	uint32_t baseTimes = 1;
	if (strcmp(period, "m") == 0)
	{
		if(times % 5 == 0)
		{
			kp = KP_Minute5;
			baseTimes = 5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else if (strcmp(period, "h") == 0)	//小时线
	{
		kp = KP_Minute5;
		baseTimes = 5;
		realTimes = PERIOD_TIMES_HOUR;
	}
	else if (strcmp(period, "f") == 0)	//半小时线
	{
		kp = KP_Minute5;
		baseTimes = 5;
		realTimes = PERIOD_TIMES_HALF;
	}
	else
		kp = KP_DAY;

	bool isDay = kp == KP_DAY;

	auto it = _bars_cache.find(key);
	bool bHasHisData = false;
	bool bHasCache = (it != _bars_cache.end());
	if (!bHasCache)
	{
		std::string cacheKey = key;
		if (realTimes != 1)
		{
			std::string rawKey = fmtutil::format<64>("{}#{}#{}", stdCode, period, baseTimes);
			if (_bars_cache.find(rawKey) != _bars_cache.end())
				bHasHisData = true;
			else
				cacheKey = rawKey;

		}
		
		if(!bHasHisData)
		{
			/*
			 *	By Wesley @ 2021.12.20
			 *	先从extloader加载数据，如果加载不到，再走原来的历史数据存储引擎加载
			 */			
			if (NULL != _bt_loader)
			{
				try
				{
					bHasHisData = cacheFinalBarsFromLoader(cacheKey, stdCode, kp);
				}
				catch (const std::exception& ex)
				{
					WTSLogger::error("Exception while cache finale bars from loader of {}({}): {}", stdCode, period, ex.what());
				}
				catch (...)
				{
					WTSLogger::error("Exception while cache finale bars from loader of {}({})", stdCode, period);
				}
			}

			if (!bHasHisData)
			{
				if (_mode == "csv")
				{
					try
					{
						bHasHisData = cacheRawBarsFromCSV(cacheKey, stdCode, kp);
					}
					catch (const std::exception& ex)
					{
						WTSLogger::error("Exception while cache finale bars from csv of {}({}): {}", stdCode, period, ex.what());
					}
					catch (...)
					{
						WTSLogger::error("Exception while cache finale bars from loader of {}({})", stdCode, period);
					}

				}
				else
				{
					try
					{
						bHasHisData = cacheRawBarsFromBin(cacheKey, stdCode, kp);
					}
					catch (const std::exception& ex)
					{
						WTSLogger::error("Exception while cache finale bars from bin of {}({}): {}", stdCode, period, ex.what());
					}
					catch (...)
					{
						WTSLogger::error("Exception while cache finale bars from bin of {}({})", stdCode, period);
					}

				}
			}
		}
	}
	else
	{
		bHasHisData = true;
	}

	if (!bHasHisData)
		return NULL;

	WTSSessionInfo* sInfo = get_session_info(stdCode, true);
	if(sInfo == NULL)
	{
		WTSLogger::error("Cannot find corresponding session of {}", stdCode);
		return NULL;
	}

	bool isClosed = (sInfo->offsetTime(_cur_time, true) >= sInfo->getCloseTime(true));
	if (realTimes != 1 && !bHasCache)
	{	
		std::string rawKey = fmtutil::format<64>("{}#{}#{}", stdCode, period, baseTimes);
		BarsListPtr& rawBars = _bars_cache[rawKey];
		if (rawBars == NULL || rawBars->_bars.empty())
		{
			WTSLogger::warn("No raw bars cached for resampling {}({})", stdCode, period);
			return NULL;
		}
		if (rawBars->_bars.size() > static_cast<std::size_t>(INT32_MAX))
		{
			WTSLogger::error("Raw bar count for {}({}) exceeds WTSKlineSlice index range", stdCode, period);
			return NULL;
		}
		WTSKlineSlice* rawKline = WTSKlineSlice::create(stdCode, kp, realTimes, &rawBars->_bars[0], rawBars->_bars.size());
		if (rawKline == NULL)
			return NULL;
		rawKline->setCode(stdCode);

		static WTSDataFactory dataFact;
		WTSKlineData* kData = dataFact.extractKlineData(rawKline, kp, realTimes, sInfo, true, _align_by_section);
		rawKline->release();

		if(kData)
		{
			_bars_cache[key].reset(new BarsList());
			BarsListPtr barsList = _bars_cache[key];
			barsList->_code = stdCode;
			barsList->_period = kp;
			barsList->_times = realTimes;
			barsList->_count = kData->size();
			barsList->_bars.swap(kData->getDataRef());
			kData->release();
			WTSLogger::info("{} resampled {}{} back kline of {} ready", barsList->_bars.size(), period, times, stdCode);
		}
		else
		{
			WTSLogger::error("Resampling {}{} back kline of {} failed", period, times, stdCode);
			return NULL;
		}
	}

	BarsListPtr& kBlkPair = _bars_cache[key];
	if(kBlkPair == NULL || kBlkPair->_bars.empty())
	{
		return NULL;
	}

	// Publish the requested main series only after its cache entry is valid.
	// If loading failed, a previous valid main key remains usable and a later
	// retry can select this key after the cache has been populated.
	if (isMain)
	{
		_main_key = key;
		_main_period = fmt::format("{}#{}", period, times);
	}
	kBlkPair->_notify_close = true;

	_codes_in_subbed.insert(stdCode);

	if (kBlkPair->_cursor == UINT_MAX)
	{
		//还没有经过初始定位
		WTSBarStruct bar;
		bar.date = _cur_tdate;
		if(kp != KP_DAY)
			bar.time = (_cur_date - 19900000) * 10000 + _cur_time;
		
		auto it = std::lower_bound(kBlkPair->_bars.begin(), kBlkPair->_bars.end(), bar, [isDay, isClosed](const WTSBarStruct& a, const WTSBarStruct& b){
			if (isDay)
				if (!isClosed)
					return a.date < b.date;
				else 
					return a.date <= b.date;
			else
				return a.time < b.time;
		});

		std::size_t eIdx = it - kBlkPair->_bars.begin();

		if (it != kBlkPair->_bars.end())
		{
			WTSBarStruct& curBar = *it;
			if (isDay)
			{
				if (curBar.date >= _cur_tdate && !isClosed)
				{
					if (eIdx > 0)
					{
						it--;
						eIdx--;
					}

				}
				else if (curBar.date > _cur_tdate && isClosed)
				{
					if (eIdx > 0)
					{
						it--;
						eIdx--;
					}
				}

				/*
				 *	By Wesley @ 2022.11.04
				 *	根据Issue#122，加了一个兜底的判断
				 *	主要防止日线回测漏掉第一根bar
				 */
				// If the first bar is today and not closed yet, no historical slice is
				// available, but keep cursor at zero so the replay loop processes it.
				if (eIdx == 0 &&
					(kBlkPair->_bars[eIdx].date > _cur_tdate ||
						(!isClosed && kBlkPair->_bars[eIdx].date == _cur_tdate)))
				{
					kBlkPair->_cursor = 0;
				}
				else
				{
					kBlkPair->_cursor = eIdx + 1;
				}
			}
			else
			{
				if (curBar.time > bar.time)
				{
					if (eIdx == 0)
					{
						// The first available minute bar is still in the future. Keep
						// its replay cursor at zero, but do not expose it as history.
						kBlkPair->_cursor = 0;
						return NULL;
					}
					else
					{
						it--;
						eIdx--;
					}
				}

				kBlkPair->_cursor = eIdx + 1;
			}
		}
		else
		{
			return NULL;
		}
	}
	else
	{
		uint32_t curMin = (_cur_date - 19900000) * 10000 + _cur_time;
		if (isDay)
		{
			//首次定位时若目标bar晚于当前交易日(如标的尚未上市), _cursor会被置为0
			//此时必须跳过本分支, 否则 _bars[_cursor-1] 会越界访问 _bars[-1]
			if (kBlkPair->_cursor > 0 && kBlkPair->_cursor <= kBlkPair->_count)
			{
				if(!isClosed)
				{
					while (kBlkPair->_bars[kBlkPair->_cursor - 1].date < _cur_tdate  && kBlkPair->_cursor < kBlkPair->_count && kBlkPair->_bars[kBlkPair->_cursor].date < _cur_tdate)
					{
						kBlkPair->_cursor++;
					}
				}
				else
				{
					while (kBlkPair->_bars[kBlkPair->_cursor - 1].date <= _cur_tdate  && kBlkPair->_cursor < kBlkPair->_count && kBlkPair->_bars[kBlkPair->_cursor].date <= _cur_tdate)
					{
						kBlkPair->_cursor++;
					}
				}
				

				if (kBlkPair->_bars[kBlkPair->_cursor - 1].date > _cur_tdate)
					kBlkPair->_cursor--;
			}
		}
		else
		{
			//同上, 分钟线首次定位为未上市/未来标的时 _cursor 也可能为0, 避免 _bars[-1] 越界
			if (kBlkPair->_cursor > 0 && kBlkPair->_cursor <= kBlkPair->_count)
			{
				while (kBlkPair->_bars[kBlkPair->_cursor-1].time < curMin && kBlkPair->_cursor < kBlkPair->_count)
				{
					kBlkPair->_cursor++;
				}

				if (kBlkPair->_bars[kBlkPair->_cursor - 1].time > curMin)
					kBlkPair->_cursor--;
			}
		}
	}


	if (kBlkPair->_cursor == 0)
		return NULL;

	uint32_t sIdx = 0;
	if (kBlkPair->_cursor > count)
		sIdx = kBlkPair->_cursor - count;

	uint32_t realCnt = kBlkPair->_cursor - sIdx;
	WTSKlineSlice* kline = WTSKlineSlice::create(stdCode, kp, 1, kBlkPair->_bars.data() + sIdx, realCnt);
	return kline;
}

WTSTickSlice* HisDataReplayer::get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime)
{
	if (!_tick_enabled)
		return NULL;

	if (!checkTicks(stdCode, _cur_tdate))
		return NULL;

	auto& tickList = _ticks_cache[stdCode];
	if (tickList._items.empty() || tickList._count == 0)
		return NULL;

	// Recompute the visible history position for this query. The replay cursor
	// remains owned by the event loop and is never advanced by a slice lookup.
	tickList._query_cursor = UINT_MAX;
	if (tickList._query_cursor == UINT_MAX)
	{
		uint32_t uDate = _cur_date;
		uint32_t uTime = _cur_time * 100000 + _cur_secs;

		if (etime != 0)
		{
			uDate = (uint32_t)(etime / 10000);
			uTime = (uint32_t)(etime % 10000 * 100000);
		}

		WTSTickStruct curTick;
		curTick.action_date = uDate;
		curTick.action_time = uTime;

		auto tit = std::lower_bound(tickList._items.begin(), tickList._items.end(), curTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		// The query cursor is the number of records strictly before the cutoff.
		// lower_bound already identifies that prefix, including when the cutoff
		// is beyond the final item or exactly equal to a record timestamp.
		tickList._query_cursor = static_cast<std::size_t>(tit - tickList._items.begin());
	}

	const std::size_t visibleCount = tickList._query_cursor;
	if (visibleCount == 0)
		return NULL;
	const std::size_t sIdx = count != 0 && visibleCount > count
		? visibleCount - count
		: 0;
	const std::size_t realCnt = visibleCount - sIdx;

	WTSTickSlice* ticks = WTSTickSlice::create(stdCode, tickList._items.data() + sIdx, realCnt);
	return ticks;
}

WTSOrdDtlSlice* HisDataReplayer::get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (!checkOrderDetails(stdCode, _cur_tdate))
		return NULL;

	auto& dataList = _orddtl_cache[stdCode];
	if (dataList._items.empty() || dataList._count == 0 || dataList._query_cursor == 0)
		return NULL;

	dataList._query_cursor = UINT_MAX;
	if (dataList._query_cursor == UINT_MAX)
	{
		uint32_t uDate = _cur_date;
		uint32_t uTime = _cur_time * 100000 + _cur_secs;

		if (etime != 0)
		{
			uDate = (uint32_t)(etime / 10000);
			uTime = (uint32_t)(etime % 10000 * 100000);
		}

		WTSOrdDtlStruct curItem;
		curItem.action_date = uDate;
		curItem.action_time = uTime;

		auto tit = std::lower_bound(dataList._items.begin(), dataList._items.end(), curItem, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t idx = tit - dataList._items.begin();
		dataList._query_cursor = idx + 1;
	}

	uint32_t eIdx = dataList._query_cursor - 1;
	uint32_t sIdx = 0;
	if (eIdx >= count - 1)
		sIdx = eIdx + 1 - count;

	uint32_t realCnt = eIdx - sIdx + 1;
	if (realCnt == 0)
		return NULL;

	WTSOrdDtlSlice* ticks = WTSOrdDtlSlice::create(stdCode, dataList._items.data() + sIdx, realCnt);
	return ticks;
}

WTSOrdQueSlice* HisDataReplayer::get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (!checkOrderQueues(stdCode, _cur_tdate))
		return NULL;

	auto& dataList = _ordque_cache[stdCode];
	if (dataList._items.empty() || dataList._count == 0 || dataList._query_cursor == 0)
		return NULL;

	dataList._query_cursor = UINT_MAX;
	if (dataList._query_cursor == UINT_MAX)
	{
		uint32_t uDate = _cur_date;
		uint32_t uTime = _cur_time * 100000 + _cur_secs;

		if (etime != 0)
		{
			uDate = (uint32_t)(etime / 10000);
			uTime = (uint32_t)(etime % 10000 * 100000);
		}

		WTSOrdQueStruct curItem;
		curItem.action_date = uDate;
		curItem.action_time = uTime;

		auto tit = std::lower_bound(dataList._items.begin(), dataList._items.end(), curItem, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t idx = tit - dataList._items.begin();
		dataList._query_cursor = idx + 1;
	}

	uint32_t eIdx = dataList._query_cursor - 1;
	uint32_t sIdx = 0;
	if (eIdx >= count - 1)
		sIdx = eIdx + 1 - count;

	uint32_t realCnt = eIdx - sIdx + 1;
	if (realCnt == 0)
		return NULL;

	WTSOrdQueSlice* ticks = WTSOrdQueSlice::create(stdCode, dataList._items.data() + sIdx, realCnt);
	return ticks;
}

WTSTransSlice* HisDataReplayer::get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (!checkTransactions(stdCode, _cur_tdate))
		return NULL;

	auto& dataList = _trans_cache[stdCode];
	if (dataList._items.empty() || dataList._count == 0 || dataList._query_cursor == 0)
		return NULL;

	dataList._query_cursor = UINT_MAX;
	if (dataList._query_cursor == UINT_MAX)
	{
		uint32_t uDate = _cur_date;
		uint32_t uTime = _cur_time * 100000 + _cur_secs;

		if (etime != 0)
		{
			uDate = (uint32_t)(etime / 10000);
			uTime = (uint32_t)(etime % 10000 * 100000);
		}

		WTSTransStruct curItem;
		curItem.action_date = uDate;
		curItem.action_time = uTime;

		auto tit = std::lower_bound(dataList._items.begin(), dataList._items.end(), curItem, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t idx = tit - dataList._items.begin();
		dataList._query_cursor = idx + 1;
	}

	std::size_t eIdx = dataList._query_cursor - 1;
	std::size_t sIdx = 0;
	if (eIdx >= count - 1)
		sIdx = eIdx + 1 - count;

	std::size_t realCnt = eIdx - sIdx + 1;
	if (realCnt == 0)
		return NULL;

	WTSTransSlice* ticks = WTSTransSlice::create(stdCode, dataList._items.data() + sIdx, realCnt);
	return ticks;
}

bool HisDataReplayer::checkAllHftData(uint32_t uDate)
{
	bool hasData = false;
	auto check = [&](const StraSubMap& subscriptions, auto& cache, uint32_t kind)
	{
		if (_terminated)
			return;
		for (const auto& subscription : subscriptions)
		{
			if (kind == 0)
				loadReplayHftDate(uDate, subscription.first, true, false, false, false);
			else if (kind == 1)
				loadReplayHftDate(uDate, subscription.first, false, true, false, false);
			else if (kind == 2)
				loadReplayHftDate(uDate, subscription.first, false, false, true, false);
			else
				loadReplayHftDate(uDate, subscription.first, false, false, false, true);
			if (_terminated)
				return;
			auto codeIt = cache.find(subscription.first);
			if (codeIt != cache.end())
			{
				auto dateIt = codeIt->second.find(uDate);
				hasData = (dateIt != codeIt->second.end() && dateIt->second._count > 0) || hasData;
			}
		}
	};
	check(_tick_sub_map, _replay_ticks_cache, 0);
	check(_orddtl_sub_map, _replay_orddtl_cache, 1);
	check(_ordque_sub_map, _replay_ordque_cache, 2);
	check(_trans_sub_map, _replay_trans_cache, 3);
	if (_terminated)
		return false;

	// Missing/empty buckets need no cursor progress. Once their natural session
	// has closed, drop the negative cache so long no-data ranges stay bounded.
	pruneReplayHftData(static_cast<uint64_t>(uDate) * 1000000000ull);

	return hasData;
}

bool HisDataReplayer::checkOrderDetails(const char* stdCode, uint32_t uDate)
{
	bool bNeedCache = false;
	auto it = _orddtl_cache.find(stdCode);
	if (it == _orddtl_cache.end())
		bNeedCache = true;
	else
	{
		auto& dataList = it->second;
		if (dataList._date != uDate)
			bNeedCache = true;
	}


	if (bNeedCache)
	{
		bool hasData = false;
		if (_mode == "csv")
		{
			WTSLogger::error("Cannot use stock level2 data in csv mode!");
			return false;
		}
		else
		{
			auto& dataList = _orddtl_cache[stdCode];
			hasData = cacheRawOrdDtlFromBin(dataList, stdCode, uDate);
		}

		if (!hasData)
		{
			auto& dataList = _orddtl_cache[stdCode];
			dataList._items.resize(0);
			dataList._cursor = UINT_MAX;
			dataList._query_cursor = UINT_MAX;
			dataList._code = stdCode;
			dataList._date = uDate;
			dataList._count = 0;
			return false;
		}
	}

	return true;
}

bool HisDataReplayer::checkOrderQueues(const char* stdCode, uint32_t uDate)
{
	bool bNeedCache = false;
	auto it = _ordque_cache.find(stdCode);
	if (it == _ordque_cache.end())
		bNeedCache = true;
	else
	{
		auto& dataList = it->second;
		if (dataList._date != uDate)
			bNeedCache = true;
	}


	if (bNeedCache)
	{
		bool hasData = false;
		if (_mode == "csv")
		{
			WTSLogger::error("Cannot use stock level2 data in csv mode!");
			return false;
		}
		else
		{
			auto& dataList = _ordque_cache[stdCode];
			hasData = cacheRawOrdQueFromBin(dataList, stdCode, uDate);
		}

		if (!hasData)
		{
			auto& dataList = _ordque_cache[stdCode];
			dataList._items.resize(0);
			dataList._cursor = UINT_MAX;
			dataList._query_cursor = UINT_MAX;
			dataList._code = stdCode;
			dataList._date = uDate;
			dataList._count = 0;
			return false;
		}
	}

	return true;
}

bool HisDataReplayer::checkTransactions(const char* stdCode, uint32_t uDate)
{
	bool bNeedCache = false;
	auto it = _trans_cache.find(stdCode);
	if (it == _trans_cache.end())
		bNeedCache = true;
	else
	{
		auto& dataList = it->second;
		if (dataList._date != uDate)
			bNeedCache = true;
	}


	if (bNeedCache)
	{
		bool hasData = false;
		if (_mode == "csv")
		{
			WTSLogger::error("Cannot use stock level2 data in csv mode!");
			return false;
		}
		else
		{
			auto& dataList = _trans_cache[stdCode];
			hasData = cacheRawTransFromBin(dataList, stdCode, uDate);
		}

		if (!hasData)
		{
			auto& dataList = _trans_cache[stdCode];
			dataList._items.resize(0);
			dataList._cursor = UINT_MAX;
			dataList._query_cursor = UINT_MAX;
			dataList._code = stdCode;
			dataList._date = uDate;
			dataList._count = 0;
			return false;
		}
	}

	return true;
}

bool HisDataReplayer::checkTicks(const char* stdCode, uint32_t uDate)
{
	if (strlen(stdCode) == 0)
		return false;

	bool bNeedCache = false;
	bool keepEmptyNegativeCache = false;
	auto it = _ticks_cache.find(stdCode);
	if (it == _ticks_cache.end())
		bNeedCache = true;
	else
	{
		HftDataList<WTSTickStruct>& tickList = (HftDataList<WTSTickStruct>&)it->second;
		if (tickList._date != uDate)
		{
			tickList._items.clear();
			tickList._cursor = UINT_MAX;
			tickList._query_cursor = UINT_MAX;
			bNeedCache = true;
		}
		else if (tickList._count == 0)
		{
			bNeedCache = true;
			keepEmptyNegativeCache = true;
		}
	}
	
	if (bNeedCache)
	{
		bool replaySourceFound = false;
		auto replayCode = _replay_ticks_cache.find(stdCode);
		if (replayCode != _replay_ticks_cache.end())
		{
			auto replayDate = replayCode->second.find(uDate);
			if (replayDate != replayCode->second.end())
			{
				replaySourceFound = true;
				auto& ticksList = _ticks_cache[stdCode];
				const std::size_t count = std::min(replayDate->second._count,
					replayDate->second._items.size());
				ticksList._items.assign(replayDate->second._items.begin(),
					replayDate->second._items.begin() + count);
				ticksList._code = stdCode;
				ticksList._date = uDate;
				ticksList._count = count;
				ticksList._cursor = UINT_MAX;
				ticksList._query_cursor = UINT_MAX;
			}
		}

		if (!replaySourceFound && keepEmptyNegativeCache)
			return false;

		bool hasTicks = replaySourceFound && _ticks_cache[stdCode]._count > 0;
		if (!replaySourceFound && NULL != _bt_loader)
		{
			auto& ticksList = _ticks_cache[stdCode];
			hasTicks = cacheRawTicksFromLoader(ticksList, stdCode, uDate);
		}
		else if (!replaySourceFound && _mode == "csv")
		{
			auto& ticksList = _ticks_cache[stdCode];
			hasTicks = cacheRawTicksFromCSV(ticksList, stdCode, uDate);
		}
		else if (!replaySourceFound)
		{
			auto& ticksList = _ticks_cache[stdCode];
			hasTicks = cacheRawTicksFromBin(ticksList, stdCode, uDate);
		}

		if (!hasTicks)
		{
			auto& ticksList = _ticks_cache[stdCode];
			ticksList._items.resize(0);
			ticksList._cursor = UINT_MAX;
			ticksList._query_cursor = UINT_MAX;
			ticksList._code = stdCode;
			ticksList._date = uDate;
			ticksList._count = 0;
			return false;
		}
	}


	return true;
}

WTSTickData* HisDataReplayer::get_last_tick(const char* stdCode)
{
	if (!checkTicks(stdCode, _cur_tdate))
		return NULL;

	auto& tickList = _ticks_cache[stdCode];
	if (tickList._items.empty() || tickList._count == 0 || tickList._query_cursor == 0)
		return NULL;

	tickList._query_cursor = UINT_MAX;
	if (tickList._query_cursor == UINT_MAX)
	{
		uint32_t uDate = _cur_date;
		uint32_t uTime = _cur_time * 100000 + _cur_secs;

		WTSTickStruct curTick;
		curTick.action_date = uDate;
		curTick.action_time = uTime;

		auto tit = std::lower_bound(tickList._items.begin(), tickList._items.end(), curTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});
		if (tit == tickList._items.end() || tit->action_date > uDate ||
			(tit->action_date == uDate && tit->action_time > uTime))
			return NULL;

		std::size_t idx = tit - tickList._items.begin();
		if (idx >= tickList._count || idx >= tickList._items.size())
			return NULL;
		tickList._query_cursor = idx + 1;
	}
	return WTSTickData::create(tickList._items[tickList._query_cursor - 1]);
}

WTSCommodityInfo* HisDataReplayer::get_commodity_info(const char* stdCode)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	return _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
}

std::string HisDataReplayer::get_rawcode(const char* stdCode)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	if(cInfo.hasRule())
	{
		std::string code = _hot_mgr.getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
		return CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	}

	return "";
}

WTSSessionInfo* HisDataReplayer::get_session_info(const char* sid, bool isCode /* = false */)
{
	if (!isCode)
		return _bd_mgr.getSession(sid);

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, &_hot_mgr);
	WTSCommodityInfo* cInfo = _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

void HisDataReplayer::loadFees(const char* filename)
{
	if (strlen(filename) == 0)
		return;

	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Fees template file {} not exists", filename);
		return;
	}

	WTSVariant* cfg = WTSCfgLoader::load_from_file(filename);
	if (cfg == NULL)
	{
		WTSLogger::error("Converting fees template file {} failed", filename);
		return;
	}

	auto keys = cfg->memberNames();
	for (const std::string& key : keys)
	{
		WTSVariant* cfgItem = cfg->get(key.c_str());
		FeeItem& fItem = _fee_map[key];
		fItem._by_volume = cfgItem->getBoolean("byvolume");
		fItem._open = cfgItem->getDouble("open");
		fItem._close = cfgItem->getDouble("close");
		fItem._close_today = cfgItem->getDouble("closetoday");
		// Optional for backward-compatible fee templates.  A missing field is 0.
		fItem._min_fee = cfgItem->getDouble("minfee");
	}

	cfg->release();

	WTSLogger::info("{} items of fees template loaded", _fee_map.size());
}


double HisDataReplayer::calc_fee(const char* stdCode, double price, double qty, uint32_t offset)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	const char* stdPID = codeInfo.stdCommID();
	auto it = _fee_map.find(stdPID);
	if (it == _fee_map.end())
		return 0.0;

	double ret = 0.0;
	double rate = 0.0;
	WTSCommodityInfo* commInfo = _bd_mgr.getCommodity(stdPID);
	const FeeItem& fItem = it->second;
	if (fItem._by_volume)
	{
		switch (offset)
		{
		case 0: rate = fItem._open; ret = rate*qty; break;
		case 1: rate = fItem._close; ret = rate*qty; break;
		case 2: rate = fItem._close_today; ret = rate*qty; break;
		default: ret = 0.0; break;
		}
	}
	else
	{
		double amount = price*qty*commInfo->getVolScale();
		switch (offset)
		{
		case 0: rate = fItem._open; ret = rate*amount; break;
		case 1: rate = fItem._close; ret = rate*amount; break;
		case 2: rate = fItem._close_today; ret = rate*amount; break;
		default: ret = 0.0; break;
		}
	}

	ret = apply_fee_minimum(ret, qty, rate, fItem._min_fee);
	return (int32_t)(ret * 100 + 0.5) / 100.0;
}

double HisDataReplayer::get_cur_price(const char* stdCode)
{
	auto it = _price_map.find(stdCode);
	if (it == _price_map.end())
	{
		return 0.0;
	}
	else
	{
		return it->second;
	}
}

double HisDataReplayer::get_day_price(const char* stdCode, int flag /* = 0 */)
{
	if(_tick_enabled)
	{
		WTSTickData* lastTick = get_last_tick(stdCode);
		if(lastTick != NULL)
		{
			const WTSTickStruct& curTs = lastTick->getTickStruct();

			double price = 0.0;
			switch (flag)
			{
			case 0:
				price = curTs.open;
				break;
			case 1:
				price = curTs.high;
				break;
			case 2:
				price = curTs.low;
				break;
			case 3:
				price = curTs.price;
				break;
			default:
				break;
			}

			lastTick->release();

			return price;
		}
	}

	auto it = _day_cache.find(stdCode);
	if (it == _day_cache.end())
		return 0.0;

	const WTSTickStruct& curTs = it->second;
	[[maybe_unused]] double price = 0.0;
	switch (flag)
	{
	case 0:
		return curTs.open;
	case 1:
		return curTs.high;
	case 2:
		return curTs.low;
	case 3:
		return curTs.price;
	default:
		return 0.0;
	}
}

void HisDataReplayer::sub_tick(uint32_t sid, const char* stdCode)
{
	if (strlen(stdCode) == 0)
		return;

	std::size_t length = strlen(stdCode);
	uint32_t flag = 0;
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
	{
		length--;

		flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
	}

	std::string hitCode(stdCode, length);
	SubList& sids = _tick_sub_map[hitCode];
	sids[sid] = std::make_pair(sid, flag);

	if (_tick_enabled)
		return;

	_unsubbed_in_need.insert(hitCode);
}

void HisDataReplayer::sub_order_detail(uint32_t sid, const char* stdCode)
{
	if (strlen(stdCode) == 0)
		return;

	std::size_t length = strlen(stdCode);
	uint32_t flag = 0;
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
	{
		length--;

		flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
	}

	SubList& sids = _orddtl_sub_map[std::string(stdCode, length)];
	sids[sid] = std::make_pair(sid, flag);
}

void HisDataReplayer::sub_order_queue(uint32_t sid, const char* stdCode)
{
	if (strlen(stdCode) == 0)
		return;

	std::size_t length = strlen(stdCode);
	uint32_t flag = 0;
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
	{
		length--;

		flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
	}

	SubList& sids = _ordque_sub_map[std::string(stdCode, length)];
	sids[sid] = std::make_pair(sid, flag);
}

void HisDataReplayer::sub_transaction(uint32_t sid, const char* stdCode)
{
	if (strlen(stdCode) == 0)
		return;

	std::size_t length = strlen(stdCode);
	uint32_t flag = 0;
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
	{
		length--;

		flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
	}

	SubList& sids = _trans_sub_map[std::string(stdCode, length)];
	sids[sid] = std::make_pair(sid, flag);
}

void HisDataReplayer::checkUnbars()
{
	for(const std::string& stdCode : _unsubbed_in_need)
	{
		//先检查是否已经在未订阅K线中
		bool bHasBars = _codes_in_unsubbed.find(stdCode) != _codes_in_unsubbed.end();
		if(bHasBars)
			continue;

		//再检查是否在已订阅K线中
		bHasBars = _codes_in_subbed.find(stdCode) != _codes_in_subbed.end();
		if (bHasBars)
			continue;

		//如果订阅了tick,但是没有对应的K线数据,则自动加载主K线周期的数据
		bool bHasHisData = false;
		std::string key = fmt::format("{}#{}", stdCode, _main_period);

		WTSKlinePeriod kp;
		uint32_t realTimes = convert::to_uint32(_main_period.c_str() + 2);
		if (_main_period[0] == 'm')
		{
			if (realTimes % 5 == 0)
			{
				kp = KP_Minute5;
			}
			else
			{
				kp = KP_Minute1;
			}
		}
		else
			kp = KP_DAY;

		/*
		 *	By Wesley @ 2021.12.20
		 *	先从extloader加载最终的K线数据
		 *	如果加载不到，再从配置的历史数据存储引擎加载数据
		 */
		if (NULL != _bt_loader)
		{
			bHasHisData = cacheFinalBarsFromLoader(key, stdCode.c_str(), kp, false);
		}

		if(!bHasHisData)
		{
			if (_mode == "csv")
			{
				bHasHisData = cacheRawBarsFromCSV(key, stdCode.c_str(), kp, false);
			}
			else
			{
				bHasHisData = cacheRawBarsFromBin(key, stdCode.c_str(), kp, false);
			}
		}
		
		if (!bHasHisData)
			continue;

		[[maybe_unused]] WTSSessionInfo* sInfo = get_session_info(stdCode.c_str(), true);

		BarsListPtr& kBlkPair = _unbars_cache[key];

		_codes_in_unsubbed.insert(stdCode);
		
		//还没有经过初始定位
		WTSBarStruct bar;
		bar.date = _cur_tdate;
		bar.time = (_cur_date - 19900000) * 10000 + _cur_time;

		const bool isDay = kBlkPair->_period == KP_DAY;
		auto it = std::lower_bound(kBlkPair->_bars.begin(), kBlkPair->_bars.end(), bar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
			return isDay ? a.date < b.date : a.time < b.time;
		});

		// _unbars_cache cursors are zero-based next-row indices. Choosing the
		// first row at/after the start preserves a first-row tick and avoids
		// interpreting an uninitialized daily-bar time field.
		kBlkPair->_cursor = static_cast<uint32_t>(it - kBlkPair->_bars.begin());
	}
}

uint32_t strToTime(const char* strTime, bool bHasSec = false)
{
	std::string str;
	const char *pos = strTime;
	while (strlen(pos) > 0)
	{
		if (pos[0] != ':')
		{
			str.append(pos, 1);
		}
		pos++;
	}

	uint32_t ret = convert::to_uint32(str.c_str());
	if (str.size() > 4 && !bHasSec)
		ret /= 100;

	return ret;
}

uint32_t strToDate(const char* strDate)
{
	StringVector ay = StrUtil::split(strDate, "/");
	if(ay.size() == 1)
		ay = StrUtil::split(strDate, "-");
	std::stringstream ss;
	if (ay.size() > 1)
	{
		auto pos = ay[2].find(" ");
		if (pos != std::string::npos)
			ay[2] = ay[2].substr(0, pos);
		ss << ay[0] << (ay[1].size() == 1 ? "0" : "") << ay[1] << (ay[2].size() == 1 ? "0" : "") << ay[2];
	}
	else
		ss << ay[0];

	return convert::to_uint32(ss.str().c_str());
}

bool HisDataReplayer::cacheRawTicksFromBin(HftDataList<WTSTickStruct>& ticksList, const char* stdCode, uint32_t uDate)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);
	
	std::string rawCode = cInfo._code;
	if(strlen(cInfo._ruletag) > 0)
	{
		rawCode = _hot_mgr.getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), uDate);
	}


	std::string content;
	bool bHit = false;
	//先检查有没有HOT、SND的主力次主力的tick文件
	const char* ruleTag = cInfo._ruletag;
	if(strlen(ruleTag) > 0)
	{
		const char* hot_flag = ruleTag;
		std::string wrappCode = fmtutil::format<64>("{}_{}", cInfo._product, hot_flag);
		bHit = _his_dt_mgr.load_raw_ticks(cInfo._exchg, wrappCode.c_str(), uDate, [&content](std::string& data) {
			content.swap(data);
		});
	}

	//如果没有找到，则读取分月合约
	if (!bHit)
	{
		/*
		 *	By Wesley @ 2022.01.11
		 *	这里将直接从文件读取，改成从HisDtMgr封装的接口加载
		 */
		bHit = _his_dt_mgr.load_raw_ticks(cInfo._exchg, rawCode.c_str(), uDate, [&content](std::string& data) {
			content.swap(data);
		});
	}

	if(!bHit)
	{
		WTSLogger::warn("No ticks data of {} on {} found", stdCode, uDate);
		return false;
	}
	if (content.size() % sizeof(WTSTickStruct) != 0)
	{
		WTSLogger::error("Tick data of {} on {} has a truncated record", stdCode, uDate);
		return false;
	}

	uint32_t tickcnt = 0;
	tickcnt = content.size() / sizeof(WTSTickStruct);
	ticksList._items.resize(tickcnt);
	memcpy(ticksList._items.data(), content.data(), content.size());
	
	ticksList._cursor = UINT_MAX;
	ticksList._query_cursor = UINT_MAX;
	ticksList._code = stdCode;
	ticksList._date = uDate;
	ticksList._count = tickcnt;

	return true;
}

bool HisDataReplayer::cacheRawOrdDtlFromBin(HftDataList<WTSOrdDtlStruct>& dataList, const char* stdCode, uint32_t uDate)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);

	std::string content;
	bool bHit = _his_dt_mgr.load_raw_orddtl(cInfo._exchg, cInfo._code, uDate, [&content](std::string& data) {
		content.swap(data);
	});

	if (!bHit)
	{
		WTSLogger::warn("No order detail data of {} on {} found", stdCode, uDate);
		return false;
	}
	if (content.size() % sizeof(WTSOrdDtlStruct) != 0)
	{
		WTSLogger::error("Order detail data of {} on {} has a truncated record", stdCode, uDate);
		return false;
	}

	uint32_t dataCnt = 0;
	dataCnt = content.size() / sizeof(WTSOrdDtlStruct);
	dataList._items.resize(dataCnt);
	memcpy(dataList._items.data(), content.data(), content.size());

	dataList._cursor = UINT_MAX;
	dataList._query_cursor = UINT_MAX;
	dataList._code = stdCode;
	dataList._date = uDate;
	dataList._count = dataCnt;

	return true;
}

bool HisDataReplayer::cacheRawOrdQueFromBin(HftDataList<WTSOrdQueStruct>& dataList, const char* stdCode, uint32_t uDate)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);

	std::string content;
	bool bHit = _his_dt_mgr.load_raw_ordque(cInfo._exchg, cInfo._code, uDate, [&content](std::string& data) {
		content.swap(data);
	});

	if (!bHit)
	{
		WTSLogger::warn("No order queue data of {} on {} found", stdCode, uDate);
		return false;
	}
	if (content.size() % sizeof(WTSOrdQueStruct) != 0)
	{
		WTSLogger::error("Order queue data of {} on {} has a truncated record", stdCode, uDate);
		return false;
	}

	uint32_t dataCnt = 0;
	dataCnt = content.size() / sizeof(WTSOrdQueStruct);
	dataList._items.resize(dataCnt);
	memcpy(dataList._items.data(), content.data(), content.size());

	dataList._cursor = UINT_MAX;
	dataList._query_cursor = UINT_MAX;
	dataList._code = stdCode;
	dataList._date = uDate;
	dataList._count = dataCnt;

	return true;
}

bool HisDataReplayer::cacheRawTransFromBin(HftDataList<WTSTransStruct>& dataList, const char* stdCode, uint32_t uDate)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);

	std::string content;
	bool bHit = _his_dt_mgr.load_raw_trans(cInfo._exchg, cInfo._code, uDate, [&content](std::string& data) {
		content.swap(data);
	});

	if (!bHit)
	{
		WTSLogger::warn("No transaction data of {} on {} found", stdCode, uDate);
		return false;
	}
	if (content.size() % sizeof(WTSTransStruct) != 0)
	{
		WTSLogger::error("Transaction data of {} on {} has a truncated record", stdCode, uDate);
		return false;
	}

	uint32_t dataCnt = 0;
	dataCnt = content.size() / sizeof(WTSTransStruct);
	dataList._items.resize(dataCnt);
	memcpy(dataList._items.data(), content.data(), content.size());

	dataList._cursor = UINT_MAX;
	dataList._query_cursor = UINT_MAX;
	dataList._code = stdCode;
	dataList._date = uDate;
	dataList._count = dataCnt;

	return true;
}

bool HisDataReplayer::cacheRawTicksFromLoader(HftDataList<WTSTickStruct>& dataList, const char* stdCode, uint32_t uDate)
{
	if (NULL == _bt_loader)
		return false;

	dataList._cursor = UINT_MAX;
	dataList._query_cursor = UINT_MAX;
	dataList._code = stdCode;
	dataList._date = uDate;
	dataList._count = 0;

	struct TickLoaderContext
	{
		std::mutex mutex;
		std::vector<WTSTickStruct> items;
		bool valid { true };
	};
	TickLoaderContext context;

	// FuncReadTicks is a C function pointer, so the thunk must not capture.
	// IBtDataLoader requires every callback to finish before returning. Collect
	// owned copies under a per-call lock so ordered batches from different
	// callback threads cannot race or retain caller-owned memory.
	// The loader must return obj unchanged; an arbitrary wrong pointer cannot be
	// safely inspected by a C thunk and is a loader-contract violation.
	bool bSucc = _bt_loader->loadRawHisTicks(&context, stdCode, uDate,
		[](void* obj, WTSTickStruct* firstItem, uint32_t count) {
			auto* callbackContext = static_cast<TickLoaderContext*>(obj);
			if (callbackContext == NULL)
				return;
			std::lock_guard<std::mutex> lock(callbackContext->mutex);
			if (!callbackContext->valid)
				return;
			if (count > 0 && firstItem == NULL)
			{
				callbackContext->items.clear();
				callbackContext->valid = false;
				return;
			}
			if (count == 0)
				return;
			if (static_cast<std::size_t>(count) > callbackContext->items.max_size() - callbackContext->items.size())
			{
				callbackContext->items.clear();
				callbackContext->valid = false;
				return;
			}
			try
			{
				callbackContext->items.insert(callbackContext->items.end(), firstItem, firstItem + count);
			}
			catch (...)
			{
				callbackContext->items.clear();
				callbackContext->valid = false;
			}
		});

	std::vector<WTSTickStruct> loadedItems;
	bool callbackValid = false;
	{
		std::lock_guard<std::mutex> lock(context.mutex);
		callbackValid = context.valid;
		if (callbackValid)
			loadedItems.swap(context.items);
	}
	if (!callbackValid)
	{
		WTSLogger::error("Extended HFT loader provided an invalid tick callback for {} on {}; stopping replay",
			stdCode, uDate);
		dataList._items.clear();
		dataList._count = 0;
		_terminated = true;
		return false;
	}

	if (!bSucc)
		return false;

	dataList._items = std::move(loadedItems);
	std::stable_sort(dataList._items.begin(), dataList._items.end(), [](const WTSTickStruct& left, const WTSTickStruct& right) {
		if (left.action_date != right.action_date)
			return left.action_date < right.action_date;
		return left.action_time < right.action_time;
	});
	dataList._count = dataList._items.size();

	if (dataList._count > 0)
		WTSLogger::info("{} items of back tick data of {} on {} loaded via extended loader", dataList._count, stdCode, uDate);

	return true;
}

bool HisDataReplayer::cacheRawTicksFromCSV(HftDataList<WTSTickStruct>& ticksList, const char* stdCode, uint32_t uDate)
{
	if (strlen(stdCode) == 0)
		return false;

	std::stringstream ss;
	ss << _base_dir << "bin/ticks/";
	std::string path = ss.str();
	if (!StdFile::exists(path.c_str()))
		fs::create_directories(path.c_str());
	ss << stdCode << "_tick_" << uDate << ".dsb";
	std::string filename = ss.str();
	if (StdFile::exists(filename.c_str()))
	{
		//如果有格式化的历史数据文件, 则直接读取
		WTSLogger::info("Reading data from {}...", filename);
		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisTickBlockV2))
		{
			WTSLogger::error("Sizechecking of back tick data file {} failed", filename);
			return false;
		}

		WTSLogger::info("Processing file content of {}...", filename);
		proc_block_data(filename.c_str(), content, false, false);
		if (content.size() % sizeof(WTSTickStruct) != 0)
		{
			WTSLogger::error("Tick data file {} has a truncated record", filename.c_str());
			return false;
		}
		uint32_t tickcnt = content.size() / sizeof(WTSTickStruct);
		ticksList._items.resize(tickcnt);
		memcpy(ticksList._items.data(), content.data(), content.size());
		ticksList._cursor = UINT_MAX;
		ticksList._query_cursor = UINT_MAX;
		ticksList._code = stdCode;
		ticksList._date = uDate;
		ticksList._count = tickcnt;
	}
	else
	{
		WTSLogger::error("Back tick data file {} not exists", filename.c_str());
		WTSLogger::warn("If you want to use tick data in csv mode, you can use wtpy.WtDataHelper.store_ticks to generate dsb file", filename.c_str());
		return false;

		/*
		 *	By Wesley @ 2023.05.18
		 *	回测的tick数据不再支持从csv读取，因为tick数据维度更多，有处理csv的时间，直接生成dsb了
		 */
		//如果没有格式化的历史数据文件, 则从csv加载
		//std::stringstream ss;
		//ss << _base_dir << "csv/ticks/" << stdCode << "_tick_" << uDate << ".csv";
		//std::string csvfile = ss.str();

		//if (!StdFile::exists(csvfile.c_str()))
		//{
		//	WTSLogger::error("Back tick data file {} not exists", csvfile.c_str());
		//	return false;
		//}

		//std::ifstream ifs;
		//ifs.open(csvfile.c_str());

		//WTSLogger::info("Reading data from {}...", csvfile.c_str());

		//char buffer[1024];
		//bool headerskipped = false;
		//auto& tickList = ticksList;
		//tickList._code = stdCode;
		//tickList._date = uDate;
		//while (!ifs.eof())
		//{
		//	ifs.getline(buffer, 1024);
		//	if (strlen(buffer) == 0)
		//		continue;

		//	//跳过头部
		//	if (!headerskipped)
		//	{
		//		headerskipped = true;
		//		continue;
		//	}

		//	//逐行读取
		//	StringVector ay = StrUtil::split(buffer, ",");
		//	WTSTickStruct ticks;
		//	ticks.action_date = strToDate(ay[0].c_str());
		//	ticks.action_time = strToTime(ay[1].c_str(), true) * 1000;
		//	ticks.price = strtod(ay[2].c_str(), NULL);
		//	ticks.volume = strtod(ay[3].c_str(), NULL);
		//	tickList._items.emplace_back(ticks);

		//	if (tickList._items.size() % 1000 == 0)
		//	{
		//		WTSLogger::info("{} items of data loaded", tickList._items.size());
		//	}
		//}
		//tickList._count = tickList._items.size();
		//ifs.close();
		//WTSLogger::info("Data file {} all loaded, totally {} items", csvfile.c_str(), tickList._items.size());

		/*
		 *	By Wesley @ 2021.12.14
		 *	这一段之前有bug，之前没有把文件头写到文件里，所以转储的dsb解析的时候会抛出异常
		 */
		//std::string content;
		//content.resize(sizeof(HisTickBlockV2));
		//HisTickBlockV2 *pBlk = (HisTickBlockV2*)content.data();
		//strcpy(pBlk->_blk_flag, BLK_FLAG);
		//pBlk->_type = BT_HIS_Ticks;
		//pBlk->_version = BLOCK_VERSION_CMP_V2;

		//std::string cmpData = WTSCmpHelper::compress_data(tickList._items.data(), sizeof(WTSTickStruct)*tickList._count);
		//pBlk->_size = cmpData.size();
		//content.append(cmpData);

		//StdFile::write_file_content(filename.c_str(), content.c_str(), content.size());
		//WTSLogger::info("Ticks transfered to file {}", filename.c_str());
	}

	return true;
}

bool HisDataReplayer::cacheFinalBarsFromLoader(const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed /* = true */)
{
	if (NULL == _bt_loader)
		return false;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	WTSCommodityInfo* commInfo = _bd_mgr.getCommodity(cInfo._exchg, cInfo._product);
	[[maybe_unused]] const char* stdPID = cInfo.stdCommID();

	std::string pname;
	std::string dirname;
	switch (period)
	{
	case KP_Minute1: pname = "m1"; dirname = "min1"; break;
	case KP_Minute5: pname = "m5"; dirname = "min5"; break;
	case KP_DAY: pname = "d"; dirname = "day"; break;
	default: pname = ""; break;
	}

	bool isDay = (period == KP_DAY);

	std::stringstream ss;
	ss << _base_dir << "his/" << dirname << "/" << cInfo._exchg << "/";
	if (!StdFile::exists(ss.str().c_str()))
        fs::create_directories(ss.str().c_str());

	const char* ruleTag = cInfo._ruletag;

	if (strlen(ruleTag) > 0)
	{
		ss << cInfo._exchg << "." << cInfo._product << "_" << ruleTag;
		if (cInfo.isExright())
			ss << (cInfo._exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ);
		ss << ".dsb";
	}
	else if (cInfo.isExright() && commInfo->isStock())
	{
		//复权数据，采用SSE.600000+.dsb这样的文件名
		ss << cInfo._exchg << "." << cInfo._code << (cInfo._exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ) << ".dsb";
	}
	else
		ss << cInfo._code << ".dsb";
	std::string filename = ss.str();

	bool bHit = false;
	if(_bt_loader->isAutoTrans() && StdFile::exists(filename.c_str()))
	{
		//如果支持自动转储，则先读取已经转储的dsb文件
		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisKlineBlockV2))
		{
			WTSLogger::error("Sizechecking of back kbar data file {} failed", filename.c_str());
		}
		else
		{
			HisKlineBlockV2* kBlock = (HisKlineBlockV2*)content.c_str();
			std::string rawData = WTSCmpHelper::uncompress_data(kBlock->_data, kBlock->_size);
			uint32_t barcnt = rawData.size() / sizeof(WTSBarStruct);
			if (barcnt == 0 || rawData.size() % sizeof(WTSBarStruct) != 0)
			{
				WTSLogger::error("Loaded {} bars of {} are empty or truncated", pname.c_str(), stdCode);
			}
			else
			{

				if (bSubbed)
					_bars_cache[key].reset(new BarsList);
				else
					_unbars_cache[key].reset(new BarsList);

				BarsListPtr& barsList = bSubbed ? _bars_cache[key] : _unbars_cache[key];
				barsList->_bars.resize(barcnt);
				memcpy(barsList->_bars.data(), rawData.data(), rawData.size());
				barsList->_cursor = UINT_MAX;
				barsList->_code = stdCode;
				barsList->_period = period;
				barsList->_count = barcnt;

				uint64_t stime = isDay ? barsList->_bars[0].date : barsList->_bars[0].time;
				uint64_t etime = isDay ? barsList->_bars[barcnt - 1].date : barsList->_bars[barcnt - 1].time;

				WTSLogger::info("{} items of back {} data of {} directly loaded from dsb file, from {} to {}", barcnt, pname.c_str(), stdCode, stime, etime);
				bHit = true;
			}
		}
	}

	if(!bHit)
	{
		//如果没有转储的历史数据文件, 则从csv加载
		WTSLogger::log_raw(LL_INFO, "Reading data via extended loader...");

		if (bSubbed)
			_bars_cache[key].reset(new BarsList);
		else
			_unbars_cache[key].reset(new BarsList);

		BarsListPtr& barsList = bSubbed ? _bars_cache[key] : _unbars_cache[key];
		barsList->_code = stdCode;
		barsList->_period = period;
		barsList->_cursor = UINT_MAX;
		barsList->_count = 0;

		std::string buffer;
		bool bSucc = _bt_loader->loadFinalHisBars(barsList.get(), stdCode, period, [](void* obj, WTSBarStruct* firstBar, uint32_t count) {
			BarsList* bars = (BarsList*)obj;
			if (bars == NULL)
				return;
			if (count > 0 && firstBar == NULL)
			{
				bars->_count = 0;
				bars->_bars.clear();
				return;
			}
			bars->_count = count;
			bars->_bars.resize(count);
			if (count > 0)
				memcpy((void*)bars->_bars.data(), firstBar, sizeof(WTSBarStruct)*count);
		});

		if (!bSucc || barsList->_count == 0 || barsList->_bars.size() != barsList->_count)
		{
			if (bSucc)
				WTSLogger::error("Loaded {} bars of {} are empty or invalid", pname.c_str(), stdCode);
			if (bSubbed)
				_bars_cache.erase(key);
			else
				_unbars_cache.erase(key);
			return false;
		}

		bool isDay = (period == KP_DAY);

		uint64_t stime = isDay ? barsList->_bars[0].date : barsList->_bars[0].time;
		uint64_t etime = isDay ? barsList->_bars[barsList->_count - 1].date : barsList->_bars[barsList->_count - 1].time;

		WTSLogger::info("{} items of back {} data of {} loaded via extended loader, from {} to {}", barsList->_count, pname.c_str(), stdCode, stime, etime);

		if(_bt_loader->isAutoTrans())
		{
			BlockType btype;
			switch (period)
			{
			case KP_Minute1: btype = BT_HIS_Minute1; break;
			case KP_Minute5: btype = BT_HIS_Minute5; break;
			default: btype = BT_HIS_Day; break;
			}

			/*
			 *	By Wesley @ 2021.12.14
			 *	这一段之前有bug，之前没有把文件头写到文件里，所以转储的dsb解析的时候会抛出异常
			 */
			std::string content;
			content.resize(sizeof(HisKlineBlockV2));
			HisKlineBlockV2 *kBlock = (HisKlineBlockV2*)content.data();
			strcpy(kBlock->_blk_flag, BLK_FLAG);
			kBlock->_type = btype;
			kBlock->_version = BLOCK_VERSION_CMP_V2;

			std::string cmpData = WTSCmpHelper::compress_data(barsList->_bars.data(), sizeof(WTSBarStruct)*barsList->_count);
			kBlock->_size = cmpData.size();
			content.append(cmpData);

			StdFile::write_file_content(filename.c_str(), content.c_str(), content.size());
			WTSLogger::info("Bars transfered to file {}", filename);
		}
	}

	return true;
}

bool HisDataReplayer::cacheRawBarsFromCSV(const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed/* = true*/)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	WTSCommodityInfo* commInfo = _bd_mgr.getCommodity(cInfo._exchg, cInfo._product);
	std::string stdPID = fmtutil::format<64>("{}.{}", cInfo._exchg, cInfo._product);

	std::string p_suffix;
	std::string dirname = PERIOD_NAME[period];
	switch (period)
	{
	case KP_Minute1: p_suffix = "m1"; break;
	case KP_Minute5: p_suffix = "m5"; break;
	case KP_DAY: p_suffix = "d"; break;
	default: p_suffix = ""; break;
	}

	bool isDay = (period == KP_DAY);

	std::stringstream ss;
	ss << _base_dir << "his/" << dirname << "/" << cInfo._exchg << "/";

	//这里自动创建，是因为后面转储需要
	if (!StdFile::exists(ss.str().c_str()))
		fs::create_directories(ss.str().c_str());

	const char* ruleTag = cInfo._ruletag;
	if (strlen(ruleTag) > 0 && commInfo->isFuture())
	{
		if(cInfo.isExright())
		{
			ss << cInfo._exchg << "." << cInfo._product << "_" << ruleTag << (cInfo._exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ) << ".dsb";
		}
		else
			ss << cInfo._exchg << "." << cInfo._product << "_" << ruleTag << ".dsb";
	}
	else if (cInfo.isExright() && commInfo->isStock())
	{
		//复权数据，采用SSE.600000+.dsb这样的文件名
		ss << cInfo._exchg << "." << cInfo._code << (cInfo._exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ) << ".dsb";
	}
	else
		ss << cInfo._code << ".dsb";
	std::string filename = ss.str();
	if (StdFile::exists(filename.c_str()))
	{
		//如果有格式化的历史数据文件, 则直接读取
		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisKlineBlockV2))
		{
			WTSLogger::error("Sizechecking of back kbar data file {} failed", filename);
			return false;
		}

		//By Wesley @ 2021.12.30
		//转储的数据不做检查，直接重新生成即可
		proc_block_data(filename.c_str(), content, true, false);
		uint32_t barcnt = content.size() / sizeof(WTSBarStruct);
		if (barcnt == 0)
		{
			WTSLogger::error("Back kbar data file {} contains no bars", filename);
			return false;
		}

		if (bSubbed)
			_bars_cache[key].reset(new BarsList);
		else
			_unbars_cache[key].reset(new BarsList);

		BarsListPtr& barsList = bSubbed ? _bars_cache[key] : _unbars_cache[key];
		barsList->_bars.resize(barcnt);
		memcpy(barsList->_bars.data(), content.data(), content.size());
		barsList->_cursor = UINT_MAX;
		barsList->_code = stdCode;
		barsList->_period = period;
		barsList->_count = barcnt;

		uint64_t stime = isDay ? barsList->_bars[0].date : barsList->_bars[0].time;
		uint64_t etime = isDay ? barsList->_bars[barcnt-1].date : barsList->_bars[barcnt-1].time;

		WTSLogger::info("{} items of back {} data of {} directly loaded from dsb file, from {} to {}", barcnt, p_suffix.c_str(), stdCode, stime, etime);
	}
	else
	{
		//如果没有格式化的历史数据文件, 则从csv加载
		std::stringstream ss;
		ss << _base_dir << "csv/" << stdCode << "_" << p_suffix << ".csv";
		std::string csvfile = ss.str();

		if (!StdFile::exists(csvfile.c_str()))
		{
			WTSLogger::error("Back kbar data file {} not exists", csvfile);
			return false;
		}

		CsvReader reader;
		reader.load_from_file(csvfile.c_str());

		WTSLogger::info("Reading data from {}, with fields: {}...", csvfile, reader.fields());

		if (bSubbed)
			_bars_cache[key].reset(new BarsList);
		else
			_unbars_cache[key].reset(new BarsList);

		BarsListPtr& barsList = bSubbed ? _bars_cache[key] : _unbars_cache[key];
		barsList->_code = stdCode;
		barsList->_period = period;
		while (reader.next_row())
		{
			//逐行读取
			WTSBarStruct bs;
			bs.date = strToDate(reader.get_string("date"));
			if (period != KP_DAY)
				bs.time = TimeUtils::timeToMinBar(bs.date, strToTime(reader.get_string("time")));
			bs.open = reader.get_double("open");
			bs.high = reader.get_double("high");
			bs.low = reader.get_double("low");
			bs.close = reader.get_double("close");
			bs.vol = reader.get_double("volume");
			bs.money = reader.get_double("turnover");
			bs.hold = reader.get_double("open_interest");
			bs.add = reader.get_double("diff_interest");
			bs.settle = reader.get_double("settle");
			barsList->_bars.emplace_back(bs);

			if (barsList->_bars.size() % 1000 == 0)
			{
				WTSLogger::info("{} lines of data loaded", barsList->_bars.size());
			}
		}
		barsList->_count = barsList->_bars.size();
		if (barsList->_count == 0)
		{
			WTSLogger::error("Back kbar data file {} contains no bars", csvfile);
			if (bSubbed)
				_bars_cache.erase(key);
			else
				_unbars_cache.erase(key);
			return false;
		}

		uint64_t stime = isDay ? barsList->_bars[0].date : barsList->_bars[0].time;
		uint64_t etime = isDay ? barsList->_bars[barsList->_count - 1].date : barsList->_bars[barsList->_count - 1].time;

		WTSLogger::info("Data file {} all loaded, totally {} items, from {} to {}", csvfile.c_str(), barsList->_bars.size(), stime, etime);

		BlockType btype;
		switch (period)
		{
		case KP_Minute1: btype = BT_HIS_Minute1; break;
		case KP_Minute5: btype = BT_HIS_Minute5; break;
		default: btype = BT_HIS_Day; break;
		}

		/*
		 *	By Wesley @ 2021.12.14
		 *	这一段之前有bug，之前没有把文件头写到文件里，所以转储的dsb解析的时候会抛出异常
		 */
		std::string content;
		content.resize(sizeof(HisKlineBlockV2));
		HisKlineBlockV2 *kBlock = (HisKlineBlockV2*)content.data();
		strcpy(kBlock->_blk_flag, BLK_FLAG);
		kBlock->_type = btype;
		kBlock->_version = BLOCK_VERSION_CMP_V2;

		std::string cmpData = WTSCmpHelper::compress_data(barsList->_bars.data(), sizeof(WTSBarStruct)*barsList->_count);
		kBlock->_size = cmpData.size();
		content.append(cmpData);

		StdFile::write_file_content(filename.c_str(), content.c_str(), content.size());
		WTSLogger::info("Bars transfered to file {}", filename);
	}

	return true;
}

bool HisDataReplayer::cacheIntegratedFutBarsFromBin(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed /* = true */)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;
	const char* stdPID = cInfo->stdCommID();

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	uint32_t endTDate = _bd_mgr.calcTradingDate(stdPID, curDate, curTime, false);

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	if(bSubbed)
		_bars_cache[key].reset(new BarsList());
	else
		_unbars_cache[key].reset(new BarsList());
	BarsListPtr& barsList = bSubbed ? _bars_cache[key] : _unbars_cache[key];
	barsList->_code = stdCode;
	barsList->_period = period;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;
	bool isDay = (period == KP_DAY);

	//const char* hot_flag = cInfo.isHot() ? FILE_SUF_HOT : FILE_SUF_2ND;
	const char* ruleTag = cInfo->_ruletag;

	std::vector<WTSBarStruct>* hotAy = NULL;
	uint64_t lastHotTime = 0;
	do
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	本来这里是要先调用_loader->loadRawHisBars从外部加载器读取主力合约数据的
		 *	但是上层会调用一次loadFinalHisBars，这里再调用loadRawHisBars就冗余了，所以直接跳过
		 *
		 *	@ 2022.01.11
		 *	将直接从文件读取，改成从HisDtMgr读取
		 */
		std::string content;
		std::string wrappCode = fmtutil::format<64>("{}.{}_{}", cInfo->_exchg, cInfo->_product, ruleTag);
		if (cInfo->isExright())
			wrappCode += cInfo->_exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ;
		bool bSucc = _his_dt_mgr.load_raw_bars(cInfo->_exchg, wrappCode.c_str(), period, [&content](std::string& data) {
			content.swap(data);
		});

		if(!bSucc)
		{
			WTSLogger::warn("Loading {} bars of {} via HisDtMgr failed", PERIOD_NAME[period], stdCode);
			break;
		}

		uint32_t barcnt = content.size() / sizeof(WTSBarStruct);
		if (barcnt <= 0)
			break;

		hotAy = new std::vector<WTSBarStruct>();
		hotAy->resize(barcnt);
		memcpy(hotAy->data(), content.data(), content.size());

		if (period != KP_DAY)
			lastHotTime = hotAy->at(barcnt - 1).time;
		else
			lastHotTime = hotAy->at(barcnt - 1).date;

		uint64_t stime = isDay ? hotAy->at(0).date : hotAy->at(0).time;
		uint64_t etime = isDay ? hotAy->at(barcnt - 1).date : hotAy->at(barcnt - 1).time;

		WTSLogger::info("{} items of back {} data of hot contract {} directly loaded, from {} to {}", barcnt, pname.c_str(), stdCode, stime, etime);

	} while (false);

	HotSections secs;
	//if (cInfo.isHot())
	//{
	//	if (!_hot_mgr.splitHotSecions(cInfo._exchg, cInfo._product, 19900102, endTDate, secs))
	//		return false;
	//}
	//else if (cInfo.isSecond())
	//{
	//	if (!_hot_mgr.splitSecondSecions(cInfo._exchg, cInfo._product, 19900102, endTDate, secs))
	//		return false;
	//}
	if(strlen(ruleTag) > 0)
	{
		if (!_hot_mgr.splitCustomSections(ruleTag, cInfo->stdCommID(), 19900102, endTDate, secs))
			return false;
	}

	if (secs.empty())
		return false;

	double baseFactor = 1.0;
	if (cInfo->_exright == 1)
		baseFactor = secs.back()._factor;
	else if (cInfo->_exright == 2)
		barsList->_factor = secs.back()._factor;

	bool bAllCovered = false;
	for (auto it = secs.rbegin(); it != secs.rend(); it++)
	{
		const HotSection& hotSec = *it;
		const char* curCode = hotSec._code.c_str();
		uint32_t rightDt = hotSec._e_date;
		uint32_t leftDt = hotSec._s_date;

		//要先将日期转换为边界时间
		WTSBarStruct sBar, eBar;
		if (period != KP_DAY)
		{
			uint64_t sTime = _bd_mgr.getBoundaryTime(stdPID, leftDt, false, true);
			uint64_t eTime = _bd_mgr.getBoundaryTime(stdPID, rightDt, false, false);

			sBar.date = leftDt;
			sBar.time = ((uint32_t)(sTime / 10000) - 19900000) * 10000 + (uint32_t)(sTime % 10000);

			if (sBar.time < lastHotTime)	//如果边界时间小于主力的最后一根Bar的时间, 说明已经有交叉了, 则不需要再处理了
			{
				bAllCovered = true;
				sBar.time = lastHotTime + 1;
			}

			eBar.date = rightDt;
			eBar.time = ((uint32_t)(eTime / 10000) - 19900000) * 10000 + (uint32_t)(eTime % 10000);

			if (eBar.time <= lastHotTime)	//右边界时间小于最后一条Hot时间, 说明全部交叉了, 没有再找的必要了
				break;
		}
		else
		{
			sBar.date = leftDt;
			if (sBar.date < lastHotTime)	//如果边界时间小于主力的最后一根Bar的时间, 说明已经有交叉了, 则不需要再处理了
			{
				bAllCovered = true;
				sBar.date = (uint32_t)lastHotTime + 1;
			}

			eBar.date = rightDt;

			if (eBar.date <= lastHotTime)
				break;
		}

		/*
		 *	By Wesley @ 2021.12.20
		 *	先从extloader读取分月合约的K线数据
		 *	如果没有读到，再从文件读取
		 */
		bool bLoaded = false;
		std::string buffer;
		if (NULL != _bt_loader)
		{
			//分月合约代码
			std::string wCode = fmtutil::format<64>("{}.{}.{}", cInfo->_exchg, cInfo->_product, curCode + strlen(cInfo->_product));
			bLoaded = _bt_loader->loadRawHisBars(&buffer, wCode.c_str(), period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
				std::string* buff = (std::string*)obj;
				buff->resize(sizeof(WTSBarStruct)*count);
				memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
			});

		}

		if (!bLoaded)
		{
			bLoaded = _his_dt_mgr.load_raw_bars(cInfo->_exchg, curCode, period, [&buffer](std::string& data) {
				buffer.swap(data);
			});

			if (!bLoaded)
			{
				WTSLogger::warn("Loading {} bars of {} via HisDtMgr failed", PERIOD_NAME[period], curCode);
				break;
			}
		}

		if (buffer.empty())
			break;

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);

		WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

		WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});

		std::size_t sIdx = pBar - firstBar;
		if ((period == KP_DAY && pBar->date < sBar.date) || (period != KP_DAY && pBar->time < sBar.time))	//早于边界时间
		{
			//早于边界时间, 说明没有数据了, 因为lower_bound会返回大于等于目标位置的数据
			continue;
		}

		pBar = std::lower_bound(firstBar + sIdx, firstBar + (barcnt - 1), eBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});
		std::size_t eIdx = pBar - firstBar;
		if ((period == KP_DAY && pBar->date > eBar.date) || (period != KP_DAY && pBar->time > eBar.time))
		{
			pBar--;
			eIdx--;
		}

		if (eIdx < sIdx)
			continue;

		uint32_t curCnt = eIdx - sIdx + 1;

		if (cInfo->isExright())
		{
			double factor = hotSec._factor / baseFactor;
			for (uint32_t idx = sIdx; idx <= eIdx; idx++)
			{
				firstBar[idx].open *= factor;
				firstBar[idx].high *= factor;
				firstBar[idx].low *= factor;
				firstBar[idx].close *= factor;
				firstBar[idx].settle *= factor;

				if (_adjust_flag & 1)
					firstBar[idx].vol /= factor;

				if (_adjust_flag & 2)
					firstBar[idx].money *= factor;

				if (_adjust_flag & 4)
				{
					firstBar[idx].hold /= factor;
					firstBar[idx].add /= factor;
				}
			}
		}

		std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
		tempAy->resize(curCnt);
		memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
		realCnt += curCnt;

		barsSections.emplace_back(tempAy);

		if (bAllCovered)
			break;
	}

	if (hotAy)
	{
		barsSections.emplace_back(hotAy);
		realCnt += hotAy->size();
	}

	if (realCnt > 0)
	{
		barsList->_bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barsList->_bars.data() + curIdx, tempAy->data(), tempAy->size() * sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	WTSLogger::info("{} items of back {} data of {} cached", realCnt, pname, stdCode);

	return true;
}

const HisDataReplayer::AdjFactorList& HisDataReplayer::getAdjFactors(const char* code, const char* exchg, const char* pid /* = "" */)
{
	static char key[20] = { 0 };
	fmtutil::format_to(key, "{}.{}.{}", exchg, pid, code);

	auto it = _adj_factors.find(key);
	if (it == _adj_factors.end())
	{
		//By Wesley @ 2021.12.21
		//如果没有复权因子，就从extloader按需读一次
		if (_bt_loader)
		{
            WTSLogger::info("No adjusting factors of {} cached, searching via extented loader...", key);
			_bt_loader->loadAdjFactors(this, key, [](void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count) {
				HisDataReplayer* self = (HisDataReplayer*)obj;
				AdjFactorList& fctrLst = self->_adj_factors[stdCode];

				for (uint32_t i = 0; i < count; i++)
				{
					AdjFactor adjFact;
					adjFact._date = dates[i];
					adjFact._factor = factors[i];

					fctrLst.emplace_back(adjFact);
				}

				//一定要把第一条加进去，不然如果是前复权的话，可能会漏处理最早的数据
				AdjFactor adjFact;
				adjFact._date = 19900101;
				adjFact._factor = 1;
				fctrLst.emplace_back(adjFact);

				std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
					return left._date < right._date;
				});

                WTSLogger::info("{} items of adjusting factors of {} loaded via extended loader", count, stdCode);
			});
		}
	}

	return _adj_factors[key];
}

bool HisDataReplayer::cacheAdjustedStkBarsFromBin(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period, bool /*bSubbed*/ /* = true */)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;
	const char* stdPID = cInfo->stdCommID();

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	[[maybe_unused]] uint32_t endTDate = _bd_mgr.calcTradingDate(stdPID, curDate, curTime, false);

	_bars_cache[key].reset(new BarsList());
	BarsListPtr& barsList = _bars_cache[key];
	barsList->_code = stdCode;
	barsList->_period = period;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;

	std::vector<WTSBarStruct>* adjustedBars = NULL;
	uint64_t lastQTime = 0;

	WTSLogger::info("Loading adjusted bars of {}...", stdCode);
	do
	{
		//先直接读取复权过的历史数据,路径如/his/day/sse/SH600000Q.dsb

		/*
		 *	By Wesley @ 2021.12.20
		 *	本来这里是要先调用_loader->loadRawHisBars从外部加载器读取主力合约数据的
		 *	但是上层会调用一次loadFinalHisBars，这里再调用loadRawHisBars就冗余了，所以直接跳过
		 *	
		 *	@ 2022.01.11
		 *	这里将文件读取改为从HisDtMgr封装的接口读取
		 */
		std::string wrappCode = fmt::format("{}{}", cInfo->_code, (cInfo->_exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ));
		std::string content;
		bool bSucc = _his_dt_mgr.load_raw_bars(cInfo->_exchg, wrappCode.c_str(), period, [&content](std::string& data) {
			content.swap(data);
		});

		if(!bSucc)
		{
			WTSLogger::warn("Loading {} bars of {} via HisDtMgr failed", PERIOD_NAME[period], stdCode);
			break;
		}

		uint32_t barcnt = content.size() / sizeof(WTSBarStruct);
		if (barcnt <= 0)
			break;

		adjustedBars = new std::vector<WTSBarStruct>();
		adjustedBars->resize(barcnt);
		memcpy(adjustedBars->data(), content.data(), content.size());

		if (period != KP_DAY)
			lastQTime = adjustedBars->at(barcnt - 1).time;
		else
			lastQTime = adjustedBars->at(barcnt - 1).date;

		WTSLogger::info("{} items of adjusted back {} data of {} directly loaded", barcnt, PERIOD_NAME[period], stdCode);
	} while (false);

	WTSLogger::info("Loading raw bars of {}...", stdCode);
	do
	{
		const char* curCode = cInfo->_code;

		//要先将日期转换为边界时间
		WTSBarStruct sBar;
		if (period != KP_DAY)
		{
			sBar.date = TimeUtils::minBarToDate(lastQTime);

			sBar.time = lastQTime + 1;
		}
		else
		{
			sBar.date = (uint32_t)lastQTime + 1;
		}

		/*
		 *	By Wesley @ 2021.12.20
		 *	先从extloader读取未复权K线数据
		 *	如果没有读到，再从文件读取
		 */
		bool bLoaded = false;
		std::string buffer;
		if (NULL != _bt_loader)
		{
			std::string wCode = fmtutil::format<64>("{}.{}.{}", cInfo->_exchg, cInfo->_product, curCode);
			bLoaded = _bt_loader->loadRawHisBars(&buffer, wCode.c_str(), period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
				std::string* buff = (std::string*)obj;
				buff->resize(sizeof(WTSBarStruct)*count);
				memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
			});

			if (bLoaded)
				WTSLogger::debug("Raw bars of {} loaded via extended loader", stdCode);
		}

		if (!bLoaded)
		{
			/*
			 *	By Wesley @ 2022.01.11
			 *	这里将文件读取改为从HisDtMgr封装的接口读取
			 */
			bLoaded = _his_dt_mgr.load_raw_bars(cInfo->_exchg, curCode, period, [&buffer](std::string& data) {
				buffer.swap(data);
			});

			if (!bLoaded)
			{
				WTSLogger::warn("Loading {} bars of {} via HisDtMgr failed", PERIOD_NAME[period], curCode);
				continue;
			}
		}

		if (buffer.empty())
			break;
		
		std::size_t barcnt = buffer.size() / sizeof(WTSBarStruct);

		WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

		WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});

		if (pBar != NULL)
		{
			std::size_t sIdx = pBar - firstBar;
			std::size_t curCnt = barcnt - sIdx;
			std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
			tempAy->resize(curCnt);
			memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
			realCnt += curCnt;

			auto& ayFactors = getAdjFactors(cInfo->_code, cInfo->_exchg, cInfo->_product);
			if (!ayFactors.empty())
			{
				WTSLogger::info("Adjusting bars of {} with adjusting factors...", stdCode);
				//做复权处理
				std::size_t lastIdx = curCnt;
				WTSBarStruct bar;
				firstBar = tempAy->data();

				//根据复权类型确定基础因子
				//如果是前复权，则历史数据会变小，以最后一个复权因子为基础因子
				//如果是后复权，则新数据会变大，基础因子为1
				double baseFactor = 1.0;
				if (cInfo->_exright == 1)
					baseFactor = ayFactors.back()._factor;
				else if (cInfo->_exright == 2)
					barsList->_factor = ayFactors.back()._factor;

				for (auto it = ayFactors.rbegin(); it != ayFactors.rend(); it++)
				{
					const AdjFactor& adjFact = *it;
					bar.date = adjFact._date;

					//调整因子
					double factor = adjFact._factor / baseFactor;

					WTSBarStruct* pBar = NULL;
					pBar = std::lower_bound(firstBar, firstBar + lastIdx - 1, bar, [](const WTSBarStruct& a, const WTSBarStruct& b) {
						return a.date < b.date;
					});

					if (pBar->date < bar.date)
						continue;

					WTSBarStruct* endBar = pBar;
					if (pBar != NULL)
					{
						std::size_t curIdx = pBar - firstBar;
						while (pBar && curIdx < lastIdx)
						{
							pBar->open *= factor;
							pBar->high *= factor;
							pBar->low *= factor;
							pBar->close *= factor;

							if (_adjust_flag & 1)
								pBar->vol /= factor;

							if (_adjust_flag & 2)
								pBar->money *= factor;

							if (_adjust_flag & 4)
							{
								pBar->hold /= factor;
								pBar->add /= factor;
							}

							pBar++;
							curIdx++;
						}
						lastIdx = endBar - firstBar;
					}

					if (lastIdx == 0)
						break;
				}
			}
			else
			{
				WTSLogger::info("No adjusting factors of {} found, ajusting task skipped...", stdCode);
			}

			barsSections.emplace_back(tempAy);
		}
	} while (false);

	if (adjustedBars)
	{
		barsSections.emplace_back(adjustedBars);
		realCnt += adjustedBars->size();
	}

	if (realCnt > 0)
	{
		barsList->_bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barsList->_bars.data() + curIdx, tempAy->data(), tempAy->size() * sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	WTSLogger::info("{} items of back {} data of {} cached", realCnt, PERIOD_NAME[period], stdCode);

	return true;
}


bool HisDataReplayer::cacheRawBarsFromBin(const std::string& key, const char* stdCode, WTSKlinePeriod period, bool bSubbed/* = true*/)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, &_hot_mgr);
	WTSCommodityInfo* commInfo = _bd_mgr.getCommodity(cInfo._exchg, cInfo._product);
	const char* stdPID = cInfo.stdCommID();

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	[[maybe_unused]] uint32_t endTDate = _bd_mgr.calcTradingDate(stdPID, curDate, curTime, false);

	[[maybe_unused]] bool isDay = (period == KP_DAY);

	const char* ruleTag = cInfo._ruletag;
	if (strlen(ruleTag) > 0)//如果是读取期货主力连续数据
	{
		return cacheIntegratedFutBarsFromBin(&cInfo, key, stdCode, period, bSubbed);
	}
	else if (cInfo.isExright() && commInfo->isStock())//如果是读取股票复权数据
	{
		return cacheAdjustedStkBarsFromBin(&cInfo, key, stdCode, period, bSubbed);
	}

	// Keep an unsuccessful or malformed storage read out of the cache so the
	// caller can repair the file and retry on the same replayer instance.
	BarsListPtr barsList(new BarsList());
	barsList->_code = stdCode;
	barsList->_period = period;
	

	/*
	 *	By Wesley @ 2021.12.20
	 *	先从extloader读取
	 *	如果没有读到，再从文件读取
	 */
	bool bLoaded = false;
	std::string buffer;
	if (NULL != _bt_loader)
	{
		bLoaded = _bt_loader->loadRawHisBars(&buffer, stdCode, period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
			std::string* buff = (std::string*)obj;
			buff->resize(sizeof(WTSBarStruct)*count);
			memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
		});
	}

	if(!bLoaded)
	{
		//读取历史的
		//std::stringstream ss;
		//ss << _base_dir << "his/" << pname << "/" << cInfo._exchg << "/" << cInfo._code << ".dsb";
		//std::string filename = ss.str();
		//if (StdFile::exists(filename.c_str()))
		//{
		//	//如果有格式化的历史数据文件, 则直接读取
		//	std::string content;
		//	StdFile::read_file_content(filename.c_str(), content);
		//	if (content.size() < sizeof(HisKlineBlock))
		//	{
		//		WTSLogger::error("Sizechecking of back kbar data file {} failed", filename.c_str());
		//		return false;
		//	}

		//	proc_block_data(filename.c_str(), content, true, false);
		//	buffer.swap(content);
		//}
		bLoaded = _his_dt_mgr.load_raw_bars(cInfo._exchg, cInfo._code, period, [&buffer](std::string& data) {
			buffer.swap(data);
		});

		if(!bLoaded)
		{
			WTSLogger::warn("Loading {} bars of {} via HisDtMgr failed", PERIOD_NAME[period], stdCode);
		}
	}
	if (!bLoaded)
		return false;

	if (buffer.size() < sizeof(WTSBarStruct) || buffer.size() % sizeof(WTSBarStruct) != 0)
	{
		WTSLogger::warn("Loaded {} bars of {} are empty or truncated", PERIOD_NAME[period], stdCode);
		return false;
	}

	const std::size_t barcnt = buffer.size() / sizeof(WTSBarStruct);
	if (barcnt > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
	{
		WTSLogger::warn("Loaded {} bars of {} exceed the K-line slice count limit", barcnt, stdCode);
		return false;
	}

	barsList->_bars.resize(barcnt);
	memcpy(barsList->_bars.data(), buffer.data(), buffer.size());
	barsList->_count = static_cast<uint32_t>(barcnt);
	(bSubbed ? _bars_cache : _unbars_cache)[key] = barsList;

	WTSLogger::info("{} items of back {} data of {} cached", barcnt, PERIOD_NAME[period], stdCode);
	return true;
}

void HisDataReplayer::check_cache_days()
{
	if (_cache_clear_days == 0)
		return;

	std::set<std::string> to_clear;
	std::string codes;
	for(auto& v : _bars_cache)
	{
		if(v.first == _main_key)
			continue;

		BarsListPtr& barsList = (BarsListPtr&)v.second;
		barsList->_untouch_days++;

		if (barsList->_untouch_days >= _cache_clear_days)
		{
			to_clear.insert(v.first);
			if (codes.size() > 0)
				codes += ",";
			codes += v.first;
		}
	}

	for (const std::string& key : to_clear)
		_bars_cache.erase(key);

	WTSLogger::info("Cached bars of {} cleared due to outdated", codes);
}
