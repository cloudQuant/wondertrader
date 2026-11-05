/*!
 * \file WtDtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtDtRunner.h"

#include "../WtDtCore/WtHelper.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/Converter.hpp"

#include <cstring>
#include <set>

USING_NS_WTP;

namespace
{
	uint32_t g_auto_parserid = 1000;

	bool isOptionCodeFormatExpected(const char* code, const char* exchange)
	{
		if (code == NULL || exchange == NULL)
			return false;

		const bool requireHyphenated = std::strcmp(exchange, "CFFEX") == 0
			|| std::strcmp(exchange, "DCE") == 0;
		const bool requireCompact = std::strcmp(exchange, "SHFE") == 0
			|| std::strcmp(exchange, "INE") == 0
			|| std::strcmp(exchange, "ZCE") == 0
			|| std::strcmp(exchange, "CZCE") == 0;
		if (!requireHyphenated && !requireCompact)
			return true;

		std::size_t hyphens = 0;
		std::size_t length = 0;
		while (length < MAX_INSTRUMENT_LENGTH && code[length] != '\0')
		{
			if (code[length] == '-')
				++hyphens;
			++length;
		}
		if (length == 0 || length >= MAX_INSTRUMENT_LENGTH)
			return false;

		return requireHyphenated ? hyphens == 2 : hyphens == 0;
	}
}

WtDtRunner::WtDtRunner()
	: _data_store(NULL)
	, _is_inited(false)
	, _parser_commands_submitted(false)
{
}


WtDtRunner::~WtDtRunner()
{
	if (shutdown())
		return;

	// Destroying a runner whose parsers are not proven drained is an owner
	// contract violation. Keep the adapter objects (the SPI the providers still
	// hold) alive for the rest of the process; their callback gates are closed,
	// so they no longer dispatch into this runner's members.
	try
	{
		static std::vector<ParserAdapterPtr>* s_leaked_owners = new std::vector<ParserAdapterPtr>();
		for (auto& entry : _parsers._adapters)
			s_leaked_owners->push_back(entry.second);
		s_leaked_owners->insert(s_leaked_owners->end(), _quarantined_parsers.begin(), _quarantined_parsers.end());
	}
	catch (...)
	{
		std::terminate();
	}
	WTSLogger::error("WtDtServo runner destroyed with parser owners that are not proven drained; owners leaked to process exit");
}

bool WtDtRunner::shutdown()
{
	bool stopped = _parsers.requestStopLifecycleAll();
	for (const ParserAdapterPtr& parser : _quarantined_parsers)
	{
		if (parser && !parser->requestStopLifecycle())
			stopped = false;
	}

	bool released = _parsers.finishReleaseLifecycleAll(2000);
	for (auto it = _quarantined_parsers.begin(); it != _quarantined_parsers.end();)
	{
		const ParserAdapterPtr& parser = *it;
		if (!parser || (parser->finishReleaseLifecycle(2000) && !parser->hasApi()))
			it = _quarantined_parsers.erase(it);
		else
		{
			released = false;
			++it;
		}
	}

	_parser_commands_submitted = false;
	if (!stopped || !released || _parsers.hasRetainedAdapters() || !_quarantined_parsers.empty())
	{
		WTSLogger::error("WtDtServo parser shutdown is incomplete; parser owners are retained");
		return false;
	}
	return true;
}
#ifdef _MSC_VER
#include "../Common/mdump.h"
extern const char* getModuleName();
#endif

void WtDtRunner::initialize(const char* cfgFile, bool isFile /* = true */, const char* modDir /* = "" */, const char* logCfg /* = "logcfg.yaml" */, 
			FuncOnTickCallback cbTick /* = NULL */, FuncOnBarCallback cbBar /* = NULL */)
{
	if(_is_inited)
	{
		WTSLogger::error("WtDtServo has already been initialized");
		return;
	}
	if (_parser_init_failed)
	{
		WTSLogger::error("WtDtServo cannot retry initialization while a parser candidate is quarantined");
		return;
	}

	WTSLogger::init(logCfg);

	WTSVariant* config = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);
	if(config == NULL)
	{
		WTSLogger::error("Loading config failed");
		WTSLogger::log_raw(LL_INFO, cfgFile);
		return;
	}

	WTSVariant* cfgBF = config->get("basefiles");
	if (cfgBF == NULL || cfgBF->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("Invalid data service config: 'basefiles' must be an object");
		config->release();
		return;
	}

	WTSVariant* parserSidecar = NULL;
	WTSVariant* parserItems = NULL;
	WTSVariant* cfgParser = config->get("parsers");
	auto releaseConfig = [&]() {
		if (parserSidecar != NULL)
		{
			parserSidecar->release();
			parserSidecar = NULL;
		}
		config->release();
	};

	// Validate the complete parser-config envelope before loading any manager.
	// A broken explicitly configured sidecar must fail before reinitializing the
	// data reader or partially mutating base-data/hot managers.
	if (cfgParser != NULL)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (filename == NULL || filename[0] == '\0')
			{
				WTSLogger::error("Parser configuration path is empty");
				releaseConfig();
				return;
			}
			if (!StdFile::exists(filename))
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
				releaseConfig();
				return;
			}

			WTSLogger::info("Reading parser config from {}...", filename);
			parserSidecar = WTSCfgLoader::load_from_file(filename);
			if (parserSidecar == NULL)
			{
				WTSLogger::error("Loading parser config {} failed", filename);
				releaseConfig();
				return;
			}
			if (parserSidecar->type() != WTSVariant::VT_Object)
			{
				WTSLogger::error("Invalid parser config {}: root must be an object", filename);
				releaseConfig();
				return;
			}
			parserItems = parserSidecar->get("parsers");
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			parserItems = cfgParser;
		}
		else
		{
			WTSLogger::error("Invalid data service config: 'parsers' must be an array or sidecar path");
			releaseConfig();
			return;
		}

		if (parserItems == NULL || parserItems->type() != WTSVariant::VT_Array)
		{
			WTSLogger::error("Invalid parser config: 'parsers' must be an array");
			releaseConfig();
			return;
		}

		std::set<std::string> parserIds;
		uint32_t nextAutoParserId = g_auto_parserid;
		for (uint32_t idx = 0; idx < parserItems->size(); idx++)
		{
			WTSVariant* cfgItem = parserItems->get(idx);
			if (cfgItem == NULL || cfgItem->type() != WTSVariant::VT_Object)
			{
				WTSLogger::error("Invalid parser config: each parser entry must be an object");
				releaseConfig();
				return;
			}
			if (!cfgItem->getBoolean("active"))
				continue;

			const char* id = cfgItem->getCString("id");
			const std::string parserId = (id == NULL || id[0] == '\0')
				? fmtutil::format<64>("auto_parser_{}", nextAutoParserId++)
				: std::string(id);
			if (!parserIds.insert(parserId).second)
			{
				WTSLogger::error("Duplicate parser id '{}' in data service configuration", parserId.c_str());
				releaseConfig();
				return;
			}
		}
	}

	_cb_tick = cbTick;
	_cb_bar = cbBar;
	WtHelper::set_module_dir(modDir);
	if (!config->getBoolean("disable_dump"))
	{
#ifdef _MSC_VER
		CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd());
#endif
	}

	//基础数据文件

	if (cfgBF->get("session"))
	{
		_bd_mgr.loadSessions(cfgBF->getCString("session"));
		WTSLogger::info("Trading sessions loaded");
	}

	WTSVariant* cfgItem = cfgBF->get("commodity");
	if (cfgItem)
	{
		if (cfgItem->type() == WTSVariant::VT_String)
		{
			_bd_mgr.loadCommodities(cfgItem->asCString());
		}
		else if (cfgItem->type() == WTSVariant::VT_Array)
		{
			for (uint32_t i = 0; i < cfgItem->size(); i++)
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
	{
		_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));
		WTSLogger::info("Holidays loaded");
	}

	if (cfgBF->get("hot"))
	{
		_hot_mgr.loadHots(cfgBF->getCString("hot"));
		WTSLogger::info("Hot rules loaded");
	}

	if (cfgBF->get("second"))
	{
		_hot_mgr.loadSeconds(cfgBF->getCString("second"));
		WTSLogger::info("Second rules loaded");
	}

	auto& ayHotTags = _hot_mgr.getHotTags();
	WTSArray* ayContracts = _bd_mgr.getContracts();
	for (auto it = ayContracts->begin(); it != ayContracts->end(); it++)
	{
		WTSContractInfo* cInfo = (WTSContractInfo*)(*it);

		for (const std::string& hotTag : ayHotTags)
		{
			bool isHot = _hot_mgr.isCustomHot(hotTag.c_str(), cInfo->getFullCode(), 0);
			if (!isHot)
				continue;

			std::string hotCode = cInfo->getFullPid();
			hotCode += ".";
			hotCode += hotTag;
			cInfo->addHotCode(hotCode.c_str());
		}
	}
	ayContracts->release();

	if (!initDataMgr(config->get("data")))
	{
		releaseConfig();
		return;
	}

	if (cfgParser != NULL)
	{
		if (!initParsers(parserItems))
		{
			const bool quarantined = hasQuarantinedParser();
			if (quarantined)
				_parser_init_failed = true;
			releaseConfig();
			if (quarantined)
				WTSLogger::error("WtDtServo initialization stopped because a parser failed to initialize; quarantined parser owners must remain alive");
			else
				WTSLogger::error("WtDtServo initialization stopped because parser configuration could not be applied");
			return;
		}
	}
	else
		WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers");

	releaseConfig();

	_is_inited = true;
	start();
}

bool WtDtRunner::initDataMgr(WTSVariant* config)
{
	if (config == NULL)
		return true;

	const bool storeConfigured = config->has("store");
	if (!_data_mgr.init(config, this) && storeConfigured)
	{
		WTSLogger::error("Configured data store failed to initialize; WtDtServo startup stopped");
		return false;
	}

	WTSLogger::info("Data manager initialized");
	return true;
}

WTSKlineSlice* WtDtRunner::get_bars_by_range(const char* stdCode, const char* period, uint64_t beginTime, uint64_t endTime /* = 0 */)
{
	if(!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = convert::to_uint32(period + 1);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
		kp = KP_DAY;

	if (endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}

	return _data_mgr.get_kline_slice_by_range(stdCode, kp, realTimes, beginTime, endTime);
}

WTSKlineSlice* WtDtRunner::get_bars_by_date(const char* stdCode, const char* period, uint32_t uDate /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	if (stdCode == NULL || *stdCode == '\0' || period == NULL || *period == '\0')
		return NULL;

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = convert::to_uint32(period + 1);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
	{
		WTSLogger::log_raw(LL_ERROR, "get_bars_by_date only supports minute period");
		return NULL;
	}

	return _data_mgr.get_kline_slice_by_date(stdCode, kp, realTimes, uDate);
}

WTSTickSlice* WtDtRunner::get_ticks_by_range(const char* stdCode, uint64_t beginTime, uint64_t endTime /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	if(endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}
	return _data_mgr.get_tick_slices_by_range(stdCode, beginTime, endTime);
}

WTSTickSlice* WtDtRunner::get_ticks_by_date(const char* stdCode, uint32_t uDate /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	return _data_mgr.get_tick_slice_by_date(stdCode, uDate);
}

WTSKlineSlice* WtDtRunner::get_bars_by_count(const char* stdCode, const char* period, uint32_t count, uint64_t endTime /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = convert::to_uint32(period + 1);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
		kp = KP_DAY;

	if (endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}

	return _data_mgr.get_kline_slice_by_count(stdCode, kp, realTimes, count, endTime);
}

WTSTickSlice* WtDtRunner::get_ticks_by_count(const char* stdCode, uint32_t count, uint64_t endTime /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	if (endTime == 0)
	{
		uint32_t curDate = TimeUtils::getCurDate();
		endTime = (uint64_t)curDate * 10000 + 2359;
	}
	return _data_mgr.get_tick_slice_by_count(stdCode, count, endTime);
}

WTSKlineSlice* WtDtRunner::get_sbars_by_date(const char* stdCode, uint32_t secs, uint32_t uDate /* = 0 */)
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return NULL;
	}

	return _data_mgr.get_skline_slice_by_date(stdCode, secs, uDate);
}

bool WtDtRunner::initParsers(WTSVariant* cfg)
{
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	std::vector<ParserAdapterPtr> candidates;
	std::set<std::string> stagedIds;
	std::set<std::string> optionalIds;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			realid = fmtutil::format<64>("auto_parser_{}", g_auto_parserid++);
		}
		if (!stagedIds.insert(realid).second || _parsers.getAdapter(realid.c_str()) != nullptr)
		{
			WTSLogger::error("Duplicate parser id '{}' in data service configuration", realid.c_str());
			_quarantined_parsers.insert(_quarantined_parsers.end(), candidates.begin(), candidates.end());
			return false;
		}

		// Decision K13=B (BUG-R104-002 / I03-NEW-003): `required` defaults to true and a
		// failure stops the data service; an optional parser that fails is reported, its
		// candidate is quarantined, and the service starts without it.
		const bool required = !cfgItem->has("required") || cfgItem->getBoolean("required");
		ParserAdapterPtr adapter(new ParserAdapter(&_bd_mgr, this));
		if (!adapter->init(realid.c_str(), cfgItem))
		{
			_quarantined_parsers.push_back(adapter);
			if (!required)
			{
				WTSLogger::warn("Optional parser '{}' failed to initialize; data service continues without it", realid.c_str());
				continue;
			}
			_quarantined_parsers.insert(_quarantined_parsers.end(), candidates.begin(), candidates.end());
			WTSLogger::error("Parser '{}' failed to initialize; data service will not start", realid.c_str());
			return false;
		}
		candidates.push_back(adapter);
		if (!required)
			optionalIds.insert(realid);
	}

	for (auto& adapter : candidates)
	{
		if (!_parsers.addAdapter(adapter->id(), adapter))
		{
			if (optionalIds.count(adapter->id()) != 0)
			{
				_quarantined_parsers.push_back(adapter);
				WTSLogger::warn("Optional parser '{}' could not be registered; data service continues without it", adapter->id());
				continue;
			}
			_quarantined_parsers.insert(_quarantined_parsers.end(), candidates.begin(), candidates.end());
			WTSLogger::error("Failed to register initialized parser '{}'", adapter->id());
			return false;
		}
	}

	WTSLogger::info("{} market data parsers loaded in total", _parsers.size());
	return true;
}

void WtDtRunner::start()
{
	if (!_is_inited)
	{
		WTSLogger::error("WtDtServo not initialized");
		return;
	}

	// Card 9c (I03-NEW-044): initialize() already started the parsers; a repeated
	// start() must not submit connect commands to the same instances again.
	if (_parsers_started.exchange(true))
	{
		WTSLogger::warn("WtDtServo parsers were already started; repeated start rejected");
		return;
	}

	// Some providers may already have accepted a command when another fails.
	// Keep all owners alive; callback-drain behavior is not established here.
	_parser_commands_submitted = false;
	_parser_commands_submitted = _parsers.runWithResult();
	if (!_parser_commands_submitted)
		WTSLogger::error("WtDtServo parser connect commands were not all submitted; partial startup may have occurred");
}

void WtDtRunner::proc_tick(WTSTickData* curTick)
{
	WTSContractInfo* cInfo = curTick->getContractInfo();
	if (cInfo == NULL)
	{
		cInfo = _bd_mgr.getContract(curTick->code(), curTick->exchg());
		curTick->setContractInfo(cInfo);
	}

	if (cInfo == NULL)
		return;

	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	if (commInfo == NULL)
	{
		WTSLogger::error("Tick contract '{}' on exchange '{}' has no commodity info, dropped",
			cInfo->getCode(), cInfo->getExchg());
		return;
	}
	[[maybe_unused]] WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	[[maybe_unused]] uint32_t hotflag = 0;

	std::string stdCode;
	const ContractCategory category = commInfo->getCategoty();
	const char* exchange = cInfo->getExchg();
	const char* rawCode = cInfo->getCode();
	const bool cffexSpotOption = category == CC_SpotOption
		&& exchange != NULL && std::strcmp(exchange, "CFFEX") == 0;
	if (category == CC_FutOption || cffexSpotOption)
	{
		if (!CodeHelper::tryRawFutOptCodeToStdCode(rawCode, exchange, stdCode)
			|| !isOptionCodeFormatExpected(rawCode, exchange))
		{
			WTSLogger::error("Invalid option tick code '{}' for exchange '{}' (category {}), dropped",
				rawCode != NULL ? rawCode : "", exchange != NULL ? exchange : "",
				static_cast<uint32_t>(category));
			return;
		}
	}
	else if (CodeHelper::isMonthlyCode(curTick->code()))
	{
		//如果是分月合约，则进行主力和次主力的判断
		stdCode = CodeHelper::rawMonthCodeToStdCode(rawCode, exchange);
	}
	else
	{
		stdCode = CodeHelper::rawFlatCodeToStdCode(rawCode, exchange, cInfo->getProduct());
	}
	if (stdCode.empty())
	{
		WTSLogger::error("Failed to normalize tick code '{}' for exchange '{}', dropped",
			rawCode != NULL ? rawCode : "", exchange != NULL ? exchange : "");
		return;
	}
	if (!curTick->trySetCode(stdCode.c_str()))
	{
		WTSLogger::error("Normalized tick code '{}' for exchange '{}' is too long, dropped",
			stdCode.c_str(), exchange != NULL ? exchange : "");
		return;
	}

	trigger_tick(stdCode.c_str(), curTick);

	if (!cInfo->isFlat())
	{
		auto hotCodes = cInfo->getHotCodes();
		for (const std::string& hotCode : hotCodes)
		{
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			if (hotTick == NULL)
				continue;
			if (!hotTick->trySetCode(hotCode.c_str()))
			{
				hotTick->release();
				continue;
			}
			hotTick->setContractInfo(curTick->getContractInfo());

			trigger_tick(hotCode.c_str(), hotTick);

			hotTick->release();
		}
	}
}

void WtDtRunner::trigger_tick(const char* stdCode, WTSTickData* curTick)
{
	if (_cb_tick != NULL)
	{
		StdUniqueLock lock(_mtx_subs);
		auto sit = _tick_sub_map.find(stdCode);
		if (sit != _tick_sub_map.end())
		{
			SubFlags flags = sit->second;
			for (uint32_t flag : flags)
			{
				if (flag == 0)
				{
					_cb_tick(stdCode, &curTick->getTickStruct());
				}
				else
				{
					std::string wCode = fmtutil::format("{}{}", stdCode, (flag == 1) ? SUFFIX_QFQ : SUFFIX_HFQ);
					if (wCode.size() >= MAX_INSTRUMENT_LENGTH)
						continue;
					if (flag == 1)
					{
						_cb_tick(wCode.c_str(), &curTick->getTickStruct());
					}
					else //(flag == 2)
					{
						WTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());
						WTSTickStruct& newTS = newTick->getTickStruct();
						newTick->setContractInfo(curTick->getContractInfo());

						//这里做一个复权因子的处理
						double factor = _data_mgr.get_exright_factor(stdCode, curTick->getContractInfo()->getCommInfo());
						newTS.open *= factor;
						newTS.high *= factor;
						newTS.low *= factor;
						newTS.price *= factor;

						newTS.settle_price *= factor;

						newTS.pre_close *= factor;
						newTS.pre_settle *= factor;

						_cb_tick(wCode.c_str(), &newTS);
						newTick->release();
					}
				}
			}

		}
	}

	{
		StdUniqueLock lock(_mtx_innersubs);
		auto sit = _tick_innersub_map.find(stdCode);
		if (sit == _tick_innersub_map.end())
			return;

		SubFlags flags = sit->second;
		for (uint32_t flag : flags)
		{
			if (flag == 0)
			{
				_data_mgr.update_bars(stdCode, curTick);
			}
			else
			{
				std::string wCode = fmtutil::format("{}{}", stdCode, (flag == 1) ? SUFFIX_QFQ : SUFFIX_HFQ);
				if (wCode.size() >= MAX_INSTRUMENT_LENGTH || !curTick->trySetCode(wCode.c_str()))
					continue;
				if (flag == 1)
				{
					_data_mgr.update_bars(wCode.c_str(), curTick);
				}
				else //(flag == 2)
				{
					WTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());
					WTSTickStruct& newTS = newTick->getTickStruct();
					newTick->setContractInfo(curTick->getContractInfo());

					//这里做一个复权因子的处理
					double factor = _data_mgr.get_exright_factor(stdCode, curTick->getContractInfo()->getCommInfo());
					newTS.open *= factor;
					newTS.high *= factor;
					newTS.low *= factor;
					newTS.price *= factor;

					newTS.settle_price *= factor;

					newTS.pre_close *= factor;
					newTS.pre_settle *= factor;

					_data_mgr.update_bars(wCode.c_str(), newTick);
					newTick->release();
				}
			}
		}
	}
}

void WtDtRunner::sub_tick(const char* codes, bool bReplace, bool bInner /* = false */)
{
	if (codes == NULL || codes[0] == '\0')
		return;

	const auto isSuffixOnly = [](const std::string& code) {
		return code.size() == 1
			&& (code[0] == SUFFIX_QFQ || code[0] == SUFFIX_HFQ);
	};
	if (bInner && isSuffixOnly(codes))
		return;

	if(bInner)
	{
		StdUniqueLock lock(_mtx_innersubs);
		if (bReplace)
			_tick_innersub_map.clear();

		const char* stdCode = codes;
		std::size_t length = strlen(stdCode);
		uint32_t flag = 0;
		if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		{
			length--;

			flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
		}

		SubFlags& flags = _tick_innersub_map[std::string(stdCode, length)];
		flags.insert(flag);
		WTSLogger::info("Tick dada of {} subscribed with flag {} for inner use", stdCode, flag);
	}
	else
	{
		StringVector ayCodes = StrUtil::split(codes, ",");
		bool hasValidCode = false;
		for (const std::string& code : ayCodes)
		{
			if (!code.empty() && !isSuffixOnly(code))
			{
				hasValidCode = true;
				break;
			}
		}
		if (!hasValidCode)
			return;

		StdUniqueLock lock(_mtx_subs);
		if (bReplace)
			_tick_sub_map.clear();

		for (const std::string& code : ayCodes)
		{
			if (code.empty() || isSuffixOnly(code))
				continue;

			//如果是主力合约代码, 如SHFE.ag.HOT, 那么要转换成原合约代码, SHFE.ag.1912
			//因为执行器只识别原合约代码
			const char* stdCode = code.c_str();
			std::size_t length = strlen(stdCode);
			uint32_t flag = 0;
			if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
			{
				length--;

				flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
			}

			SubFlags& flags = _tick_sub_map[std::string(stdCode, length)];
			flags.insert(flag);
			WTSLogger::info("Tick dada of {} subscribed with flag {}", stdCode, flag);
		}
	}
}

void WtDtRunner::sub_bar(const char* stdCode, const char* period)
{
	if (stdCode == NULL || stdCode[0] == '\0' || period == NULL || period[0] == '\0')
		return;

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = convert::to_uint32(period + 1);

	WTSKlinePeriod kp;
	uint32_t realTimes = times;
	if (basePeriod[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			realTimes /= 5;
		}
		else
		{
			kp = KP_Minute1;
		}
	}
	else
		kp = KP_DAY;

	_data_mgr.clear_subbed_bars();
	_data_mgr.subscribe_bar(stdCode, kp, realTimes);
	sub_tick(stdCode, true, true);
}

void WtDtRunner::trigger_bar(const char* stdCode, const char* period, WTSBarStruct* lastBar)
{
	if (_cb_bar == NULL)
		return;

	_cb_bar(stdCode, period, lastBar);
}

void WtDtRunner::clear_cache()
{
	_data_mgr.clear_cache();
}
