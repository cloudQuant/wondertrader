/*!
 * /file WtRunner.cpp
 * /project	WonderTrader
 *
 * /author Wesley
 * /date 2020/03/30
 * 
 * /brief 
 */
#include "WtRunner.h"

#include <exception>
#include <algorithm>

#include "../WtCore/WtHelper.h"
#include "../WtCore/CtaStraContext.h"
#include "../WtCore/HftStraContext.h"
#include "../WtCore/WtDiffExecuter.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSUtils/yamlcpp/yaml.h"
#include "../WTSUtils/SignalHook.hpp"
#include "../Share/StrUtil.hpp"

#include <boost/filesystem.hpp>
#include <vector>

namespace
{
struct PendingTraderBinding
{
	TraderAdapterPtr trader;
	ExecCmdPtr owner; // Keep the sink alive until deferred registration.
	ITrdNotifySink* sink;
};

struct ScopedBoolReset
{
	explicit ScopedBoolReset(bool& value) : _value(value) { _value = true; }
	~ScopedBoolReset() { _value = false; }
	bool& _value;
};

WTSVariant* loadConfigSafely(const char* path)
{
	try
	{
		return WTSCfgLoader::load_from_file(path);
	}
	catch (const YAML::Exception& ex)
	{
		WTSLogger::error("Parsing config file {} failed: {}", path == NULL ? "<null>" : path, ex.what());
		return NULL;
	}
}

bool preflightArrayConfig(WTSVariant* root, const char* key, const char* label)
{
	WTSVariant* entry = root == NULL ? NULL : root->get(key);
	if (entry == NULL || entry->type() == WTSVariant::VT_Array)
		return true;
	if (entry->type() != WTSVariant::VT_String)
	{
		WTSLogger::error("Invalid {} configuration", label);
		return false;
	}

	const char* path = entry->asCString();
	if (!StdFile::exists(path))
	{
		WTSLogger::error("{} configuration {} not exists", label, path);
		return false;
	}

	WTSVariant* sideConfig = loadConfigSafely(path);
	if (sideConfig == NULL)
	{
		WTSLogger::error("Loading {} configuration {} failed", label, path);
		return false;
	}
	WTSVariant* items = sideConfig->get(key);
	const bool valid = items != NULL && items->type() == WTSVariant::VT_Array;
	if (!valid)
		WTSLogger::error("{} configuration {} must contain an array named {}", label, path, key);
	sideConfig->release();
	return valid;
}

bool preflightRequiredObject(WTSVariant* root, const char* key, const char* label)
{
	WTSVariant* value = root == NULL ? NULL : root->get(key);
	if (value != NULL && value->type() == WTSVariant::VT_Object)
		return true;
	WTSLogger::error("{} configuration must be an object", label);
	return false;
}

bool preflightOptionalFile(WTSVariant* root, const char* key, const char* label)
{
	WTSVariant* value = root == NULL ? NULL : root->get(key);
	if (value == NULL)
		return true;
	if (value->type() != WTSVariant::VT_String || !StdFile::exists(value->asCString()))
	{
		WTSLogger::error("{} configuration file is invalid or missing", label);
		return false;
	}
	return true;
}

bool preflightRequiredFile(WTSVariant* root, const char* key, const char* label)
{
	WTSVariant* value = root == NULL ? NULL : root->get(key);
	if (value != NULL && value->type() == WTSVariant::VT_String && StdFile::exists(value->asCString()))
		return true;
	WTSLogger::error("{} configuration file is invalid or missing", label);
	return false;
}

bool hasActiveItems(WTSVariant* list)
{
	if (list == NULL || list->type() != WTSVariant::VT_Array)
		return false;
	for (uint32_t i = 0; i < list->size(); ++i)
	{
		WTSVariant* item = list->get(i);
		if (item != NULL && item->getBoolean("active"))
			return true;
	}
	return false;
}

bool hasActiveStrategies(WTSVariant* config)
{
	WTSVariant* strategies = config == NULL ? NULL : config->get("strategies");
	if (strategies == NULL || strategies->type() != WTSVariant::VT_Object)
		return false;
	return hasActiveItems(strategies->get("cta")) ||
		hasActiveItems(strategies->get("hft")) ||
		hasActiveItems(strategies->get("sel"));
}

bool hasExecuters(WtExecuterMgr& manager)
{
	bool found = false;
	manager.enum_executer([&found](ExecCmdPtr) { found = true; });
	return found;
}

}

const char* getBinDir()
{
	static std::string basePath;
	if (basePath.empty())
	{
		basePath = boost::filesystem::initial_path<boost::filesystem::path>().string();

		basePath = StrUtil::standardisePath(basePath);
	}

	return basePath.c_str();
}



WtRunner::WtRunner()
	: _config(NULL)
	, _engine(NULL)
	, _data_store(NULL)
	, _is_hft(false)
	, _is_sel(false)
	, _to_exit(false)
	, _runtime_started(false)
	, _shutdown_requested(false)
{
	
}


WtRunner::~WtRunner()
{
	if (!shutdown())
		std::terminate();
}

bool WtRunner::shutdown()
{
	return shutdownRuntime(true);
}

bool WtRunner::shutdownRuntime(bool stopEngines, bool fromConfigFailure)
{
	std::unique_lock<std::recursive_mutex> lifecycleLock(_lifecycle_mutex, std::try_to_lock);
	if (!lifecycleLock.owns_lock())
	{
		// Do not block a parser, trader, or strategy callback behind configure or
		// startup. The active control path observes this request and fails closed.
		_shutdown_requested.store(true, std::memory_order_release);
		_to_exit.store(true, std::memory_order_release);
		return false;
	}
	if (_shutdown_in_progress)
		return false;
	if ((_configuring && !fromConfigFailure) || _runtime_starting)
	{
		// A callback that reenters shutdown during configure/startup only records
		// the request. The outer control path completes it after the callback exits.
		_shutdown_requested.store(true, std::memory_order_release);
		return false;
	}
	ScopedBoolReset shutdownGuard(_shutdown_in_progress);
	_shutdown_requested.store(true, std::memory_order_release);
	_to_exit.store(true, std::memory_order_release);
	_traders.requestStopSinkNotifications();
	if (_traders.isInSinkCallback())
		return false;

	// Keep this coordinator across failed attempts: it owns the stable snapshots
	// and manager binding needed to retry the same global shutdown transaction.
	if (!_exec_quiesce)
	{
		try
		{
			std::vector<WtExecuterMgr*> managers;
			managers.push_back(&_cta_engine.getExecuterMgr());
			managers.push_back(&_sel_engine.getExecuterMgr());
			_exec_quiesce.reset(new WtExecQuiesceCoordinator(managers));
		}
		catch (...)
		{
			WTSLogger::error("Allocating executor shutdown coordinator failed; runner owners are retained");
			return false;
		}
	}
	if (!_exec_quiesce->quiesce_all())
	{
		WTSLogger::error("Executor shutdown is incomplete; runner owners are retained for a control-thread retry");
		return false;
	}

	if (!_failed_parser_owners.empty())
	{
		WTSLogger::error("Parser initialization left an owner without a verified shutdown contract; runner owners are retained");
		return false;
	}

	// The coordinator has closed executor ingress and drained every accepted
	// host/plugin operation. It is now safe to stop upstream quote producers.
	_parsers.release();
	if (!_traders.waitSinkNotificationsStopped())
		return false;

	if (stopEngines)
	{
		_cta_engine.release();
		_hft_engine.release();
		_sel_engine.release();
	}
	for (const auto& item : _traders.getAdapters())
		item.second->release();
	return true;
}

void WtRunner::init(const std::string& filename)
{
	WTSLogger::init(filename.c_str());

	WtHelper::setInstDir(getBinDir());

	if(!StdFile::exists(filename.c_str()))
	{
		WTSLogger::warn("logging configure {} not exists", filename);
	}
}

bool WtRunner::config(const std::string& filename)
{
	std::lock_guard<std::recursive_mutex> lifecycleLock(_lifecycle_mutex);
	if (_configuring)
	{
		WTSLogger::error("Runner configuration is already in progress");
		return false;
	}
	ScopedBoolReset configuringGuard(_configuring);
	if (_runtime_started.load(std::memory_order_acquire) ||
		_shutdown_requested.load(std::memory_order_acquire) || _exec_quiesce)
	{
		WTSLogger::error("Runner cannot be configured while running or after executor shutdown has started");
		return false;
	}

	// Loading and validating the top-level file plus its sidecar list configs is
	// a pure preflight. A missing or malformed file must leave a previously
	// configured runtime intact so the caller can retry with a valid path.
	WTSVariant* newConfig = loadConfigSafely(filename.c_str());
	if (newConfig == NULL)
	{
		WTSLogger::error("Loading config file {} failed", filename);
		return false;
	}
	if (!preflightArrayConfig(newConfig, "parsers", "parser") ||
		!preflightArrayConfig(newConfig, "traders", "trader"))
	{
		newConfig->release();
		return false;
	}
	if (!preflightRequiredObject(newConfig, "basefiles", "basefiles") ||
		!preflightRequiredObject(newConfig, "env", "environment") ||
		!preflightRequiredObject(newConfig, "data", "data") ||
		!preflightRequiredFile(newConfig, "bspolicy", "action policy"))
	{
		newConfig->release();
		return false;
	}
	WTSVariant* envPreflight = newConfig->get("env");
	const char* envNamePreflight = envPreflight->getCString("name");
	if ((envNamePreflight == NULL || envNamePreflight[0] == '\0' ||
		wt_stricmp(envNamePreflight, "cta") == 0 || wt_stricmp(envNamePreflight, "sel") == 0) &&
		!preflightArrayConfig(newConfig, "executers", "executer"))
	{
		newConfig->release();
		return false;
	}
	WTSVariant* baseFilesPreflight = newConfig->get("basefiles");
	if (!preflightOptionalFile(baseFilesPreflight, "hot", "hot") ||
		!preflightOptionalFile(baseFilesPreflight, "second", "second hot"))
	{
		newConfig->release();
		return false;
	}
	WTSVariant* rulesPreflight = baseFilesPreflight->get("rules");
	if (rulesPreflight != NULL && rulesPreflight->type() == WTSVariant::VT_Object)
	{
		for (const std::string& ruleTag : rulesPreflight->memberNames())
		{
			if (!preflightOptionalFile(rulesPreflight, ruleTag.c_str(), "custom rule"))
			{
				newConfig->release();
				return false;
			}
		}
	}
	if (_engine != NULL &&
		(!_parsers._adapters.empty() || !_traders.getAdapters().empty() ||
			hasExecuters(_cta_engine.getExecuterMgr()) ||
			hasExecuters(_sel_engine.getExecuterMgr()) ||
			hasActiveStrategies(_config)))
	{
		WTSLogger::error("Runner cannot replace a configured runtime with active parser, trader, executor, or strategy owners");
		newConfig->release();
		return false;
	}
	if (_shutdown_requested.load(std::memory_order_acquire))
	{
		newConfig->release();
		return false;
	}

	// From here on configuration mutates managers and may publish plugin
	// owners. Any failure therefore takes the fail-closed shutdown path.
	_config = newConfig;
	_engine = NULL;
	auto failConfig = [this]() {
		shutdownRuntime(false, true);
		_engine = NULL;
		return false;
	};

	//基础数据文件
	WTSVariant* cfgBF = _config->get("basefiles");
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
		_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));

	if (cfgBF->get("hot") && !_hot_mgr.loadHots(cfgBF->getCString("hot")))
	{
		WTSLogger::error("Loading hot configuration {} failed", cfgBF->getCString("hot"));
		return failConfig();
	}

	if (cfgBF->get("second") && !_hot_mgr.loadSeconds(cfgBF->getCString("second")))
	{
		WTSLogger::error("Loading second hot configuration {} failed", cfgBF->getCString("second"));
		return failConfig();
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

	if (cfgBF->has("rules"))
	{
		auto cfgRules = cfgBF->get("rules");
		auto tags = cfgRules->memberNames();
		for (const std::string& ruleTag : tags)
		{
			if (!_hot_mgr.loadCustomRules(ruleTag.c_str(), cfgRules->getCString(ruleTag.c_str())))
			{
				WTSLogger::error("Loading {} rules from {} failed", ruleTag, cfgRules->getCString(ruleTag.c_str()));
				return failConfig();
			}
			WTSLogger::info("{} rules loaded from {}", ruleTag, cfgRules->getCString(ruleTag.c_str()));
		}
	}

	//初始化运行环境
	if (!initEngine())
		return failConfig();

	//初始化数据管理
	if (!initDataMgr())
	{
		WTSLogger::error("Initializing data manager failed");
		return failConfig();
	}

	if (!initActionPolicy())
		return failConfig();

	//初始化行情通道
	WTSVariant* cfgParser = _config->get("parsers");
	if (cfgParser)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				WTSVariant* var = loadConfigSafely(filename);
				if(var)
				{
					if (!initParsers(var->get("parsers")))
					{
						WTSLogger::error("Loading parsers failed");
						var->release();
						return failConfig();
					}
					var->release();
				}
				else
				{
					WTSLogger::error("Loading parser config {} failed", filename);
					return failConfig();
				}
			}
			else
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
				return failConfig();
			}
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			if (!initParsers(cfgParser))
			{
				WTSLogger::error("Loading parsers failed");
				return failConfig();
			}
		}
		else
		{
			WTSLogger::error("Invalid parsers configuration in {}", filename);
			return failConfig();
		}
	}
	if (_shutdown_requested.load(std::memory_order_acquire))
		return failConfig();

	//初始化交易通道
	WTSVariant* cfgTraders = _config->get("traders");
	if (cfgTraders)
	{
		if (cfgTraders->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgTraders->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading trader config from {}...", filename);
				WTSVariant* var = loadConfigSafely(filename);
				if (var)
				{
					if (!initTraders(var->get("traders")))
					{
						WTSLogger::error("Loading traders failed");
						var->release();
						return failConfig();
					}
					var->release();
				}
				else
				{
					WTSLogger::error("Loading trader config {} failed", filename);
					return failConfig();
				}
			}
			else
			{
				WTSLogger::error("Trader configuration {} not exists", filename);
				return failConfig();
			}
		}
		else if (cfgTraders->type() == WTSVariant::VT_Array)
		{
			if (!initTraders(cfgTraders))
			{
				WTSLogger::error("Loading traders failed");
				return failConfig();
			}
		}
		else
		{
			WTSLogger::error("Invalid traders configuration in {}", filename);
			return failConfig();
		}
	}
	if (_shutdown_requested.load(std::memory_order_acquire))
		return failConfig();

	initEvtNotifier();

	//如果不是高频引擎,则需要配置执行模块
	if (!_is_hft)
	{
		WTSVariant* cfgExec = _config->get("executers");
		if (cfgExec != NULL)
		{
			if (cfgExec->type() == WTSVariant::VT_String)
			{
				const char* filename = cfgExec->asCString();
				if (StdFile::exists(filename))
				{
					WTSLogger::info("Reading executer config from {}...", filename);
					WTSVariant* var = loadConfigSafely(filename);
					if (var)
					{
						if (!initExecuters(var->get("executers")))
						{
							WTSLogger::error("Loading executers from {} failed", filename);
							var->release();
							return failConfig();
						}

						WTSVariant* c = var->get("routers");
						const bool routerRulesLoaded = c == NULL || _cta_engine.loadRouterRules(c);
						var->release();
						if (!routerRulesLoaded)
						{
							WTSLogger::error("Loading router rules from {} failed", filename);
							return failConfig();
						}
					}
					else
					{
						WTSLogger::error("Loading executer config {} failed", filename);
						return failConfig();
					}
				}
				else
				{
					WTSLogger::error("Executer configuration {} not exists", filename);
					return failConfig();
				}
			}
			else if (cfgExec->type() == WTSVariant::VT_Array)
			{
				if (!initExecuters(cfgExec))
				{
					WTSLogger::error("Loading inline executers failed");
					return failConfig();
				}
			}
			else
			{
				WTSLogger::error("Invalid executer configuration in {}", filename);
				return failConfig();
			}
		}

		WTSVariant* cfgRouter = _config->get("routers");
		if (cfgRouter != NULL && !_cta_engine.loadRouterRules(cfgRouter))
		{
			WTSLogger::error("Loading router rules from {} failed", filename);
			return failConfig();
		}
	}
	if (_shutdown_requested.load(std::memory_order_acquire))
		return failConfig();

	if ((!_is_hft && !initCtaStrategies()) || (_is_hft && !initHftStrategies()))
	{
		WTSLogger::error("Initializing configured strategies failed");
		return failConfig();
	}
	if (_shutdown_requested.load(std::memory_order_acquire))
		return failConfig();
	
	return true;
}

bool WtRunner::initCtaStrategies()
{
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL)
		return true;
	if (cfg->type() != WTSVariant::VT_Object)
		return false;

	cfg = cfg->get("cta");
	if (cfg == NULL)
		return true;
	if (cfg->type() != WTSVariant::VT_Array)
		return false;

	std::string path = WtHelper::getCWD() + "cta/";
	_cta_stra_mgr.loadFactories(path.c_str());

	struct PendingCtaStrategy
	{
		WTSVariant* config;
		const char* id;
		const char* name;
		int32_t slippage;
		CtaStrategyPtr strategy;
	};
	std::vector<PendingCtaStrategy> pending;
	pending.reserve(cfg->size());

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (cfgItem == NULL || cfgItem->type() != WTSVariant::VT_Object)
		{
			WTSLogger::error("Invalid CTA strategy configuration at index {}", idx);
			return false;
		}
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		int32_t slippage = cfgItem->getInt32("slippage");
		if (id == NULL || id[0] == '\0' || name == NULL || name[0] == '\0')
		{
			WTSLogger::error("Creating CTA strategy failed: strategy id and name are required");
			return false;
		}
		CtaStrategyPtr stra;
		try { stra = _cta_stra_mgr.createStrategy(name, id); }
		catch (...)
		{
			WTSLogger::error("Creating CTA strategy {} ({}) threw an exception", name, id);
			return false;
		}
		if (stra == NULL || stra->self() == NULL)
		{
			WTSLogger::error("Creating CTA strategy {} ({}) failed", name, id);
			return false;
		}
		pending.push_back({ cfgItem, id, name, slippage, stra });
	}

	for (const PendingCtaStrategy& item : pending)
	{
		try
		{
			if (!item.strategy->self()->init(item.config->get("params")))
			{
				WTSLogger::error("Initializing CTA strategy {} ({}) failed", item.name, item.id);
				return false;
			}
		}
		catch (...)
		{
			WTSLogger::error("Initializing CTA strategy {} ({}) threw an exception", item.name, item.id);
			return false;
		}
		CtaStraContext* ctx = new CtaStraContext(&_cta_engine, item.id, item.slippage);
		ctx->set_strategy(item.strategy->self());
		_cta_engine.addContext(CtaContextPtr(ctx));
	}

	return true;
}

bool WtRunner::initHftStrategies()
{
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL)
		return true;
	if (cfg->type() != WTSVariant::VT_Object)
		return false;

	cfg = cfg->get("hft");
	if (cfg == NULL)
		return true;
	if (cfg->type() != WTSVariant::VT_Array)
		return false;

	std::string path = WtHelper::getCWD() + "hft/";
	_hft_stra_mgr.loadFactories(path.c_str());

	struct PendingHftStrategy
	{
		WTSVariant* config;
		const char* id;
		const char* name;
		bool agent;
		int32_t slippage;
		HftStrategyPtr strategy;
	};
	std::vector<PendingHftStrategy> pending;
	pending.reserve(cfg->size());

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (cfgItem == NULL || cfgItem->type() != WTSVariant::VT_Object)
		{
			WTSLogger::error("Invalid HFT strategy configuration at index {}", idx);
			return false;
		}
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		bool agent = cfgItem->getBoolean("agent");
		int32_t slippage = cfgItem->getInt32("slippage");
		if (id == NULL || id[0] == '\0' || name == NULL || name[0] == '\0')
		{
			WTSLogger::error("Creating HFT strategy failed: strategy id and name are required");
			return false;
		}
		HftStrategyPtr stra;
		try { stra = _hft_stra_mgr.createStrategy(name, id); }
		catch (...)
		{
			WTSLogger::error("Creating HFT strategy {} ({}) threw an exception", name, id);
			return false;
		}
		if (stra == NULL || stra->self() == NULL)
		{
			WTSLogger::error("Creating HFT strategy {} ({}) failed", name, id);
			return false;
		}
		pending.push_back({ cfgItem, id, name, agent, slippage, stra });
	}

	for (const PendingHftStrategy& item : pending)
	{
		try
		{
			if (!item.strategy->self()->init(item.config->get("params")))
			{
				WTSLogger::error("Initializing HFT strategy {} ({}) failed", item.name, item.id);
				return false;
			}
		}
		catch (...)
		{
			WTSLogger::error("Initializing HFT strategy {} ({}) threw an exception", item.name, item.id);
			return false;
		}
		HftStraContext* ctx = new HftStraContext(&_hft_engine, item.id, item.agent, item.slippage);
		ctx->set_strategy(item.strategy->self());

		const char* traderid = item.config->getCString("trader");
		TraderAdapterPtr trader = _traders.getAdapter(traderid);
		if(trader)
		{
			ctx->setTrader(trader.get());
			_hft_engine.addContext(HftContextPtr(ctx));
			trader->addSink(ctx);
		}
		else
		{
			WTSLogger::error("Trader {} not exists, binding trader to HFT strategy failed", traderid);
			_hft_engine.addContext(HftContextPtr(ctx));
		}
	}

	return true;
}


bool WtRunner::initEngine()
{
	WTSVariant* cfg = _config->get("env");
	if (cfg == NULL)
		return false;

	const char* name = cfg->getCString("name");
	
	if (strlen(name) == 0 || wt_stricmp(name, "cta") == 0)
	{
		_is_hft = false;
		_is_sel = false;
	}
	else if (wt_stricmp(name, "sel") == 0)
	{
		_is_sel = true;
	}
	else //if (wt_stricmp(name, "hft") == 0)
	{
		_is_hft = true;
	}

	if (_is_hft)
	{
		WTSLogger::info("Trading enviroment initialzied with engine: HFT");
		_hft_engine.init(cfg, &_bd_mgr, &_data_mgr, &_hot_mgr, &_notifier);
		_engine = &_hft_engine;
	}
	else if (_is_sel)
	{
		WTSLogger::info("Trading enviroment initialzied with engine: SEL");
		_sel_engine.init(cfg, &_bd_mgr, &_data_mgr, &_hot_mgr, &_notifier);
		_engine = &_sel_engine;
	}
	else
	{
		WTSLogger::info("Trading enviroment initialzied with engine: CTA");
		_cta_engine.init(cfg, &_bd_mgr, &_data_mgr, &_hot_mgr, &_notifier);
		_engine = &_cta_engine;
	}
	if (!_engine->fee_config_valid())
	{
		WTSLogger::error("Runner initialization rejected: fee template configuration is invalid");
		_engine = NULL;
		return false;
	}

	_engine->set_adapter_mgr(&_traders);

	return true;
}

bool WtRunner::initActionPolicy()
{
	return _act_policy.init(_config->getCString("bspolicy"));
}

bool WtRunner::initDataMgr()
{
	WTSVariant*cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	if (!_data_mgr.init(cfg, _engine))
		return false;
	WTSLogger::info("Data manager initialized");

	return true;
}

bool WtRunner::initParsers(WTSVariant* cfgParser)
{
	if (cfgParser == NULL)
		return false;

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgParser->size(); idx++)
	{
		WTSVariant* cfgItem = cfgParser->get(idx);
		if(!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			static uint32_t auto_parserid = 1000;
			realid = fmtutil::format<64>("auto_parser_{}", auto_parserid++);
		}

		ParserAdapterPtr adapter(new ParserAdapter);
		try
		{
			// Keep the candidate before initialization: ParserAdapter's destructor
			// cannot release a partially initialized external API safely.
			_failed_parser_owners.push_back(adapter);
		}
		catch (...)
		{
			WTSLogger::error("Retaining parser {} for fail-closed cleanup failed", realid);
			return false;
		}

		bool initialized = false;
		try { initialized = adapter->init(realid.c_str(), cfgItem, _engine, &_bd_mgr, &_hot_mgr); }
		catch (...) { initialized = false; }
		if (!initialized)
		{
			WTSLogger::error("Initializing parser {} failed", realid);
			if (!adapter->hasApi())
			{
				// The adapter reports that no API or uncertain factory owner exists;
				// its init failure path already released the module/config candidate.
				const auto owner = std::find(_failed_parser_owners.begin(), _failed_parser_owners.end(), adapter);
				if (owner != _failed_parser_owners.end())
					_failed_parser_owners.erase(owner);
				WTSLogger::error("Parser {} failed before API creation; candidate owner can be discarded safely", realid);
			}
			else
			{
				// An API (or uncertain factory owner) exists; keep its borrowed graph
				// alive because the provider has no verified callback-drain contract.
				WTSLogger::error("Parser {} retains an API owner after init failure; candidate is quarantined", realid);
			}
			return false;
		}

		bool registered = false;
		try { registered = _parsers.addAdapter(realid.c_str(), adapter); }
		catch (...) { registered = false; }
		if (!registered)
		{
			WTSLogger::error("Registering parser {} failed", realid);
			// Keep the initialized API alive because no shutdown contract was
			// established for this registration failure path.
			return false;
		}

		const auto owner = std::find(_failed_parser_owners.begin(), _failed_parser_owners.end(), adapter);
		if (owner != _failed_parser_owners.end())
			_failed_parser_owners.erase(owner);

		count++;
	}

	WTSLogger::info("{} parsers loaded", count);
	return true;
}

bool WtRunner::initExecuters(WTSVariant* cfgExecuter)
{
	if (cfgExecuter == NULL || cfgExecuter->type() != WTSVariant::VT_Array)
		return false;

	std::string path = WtHelper::getCWD() + "executer/";
	_exe_factory.loadFactories(path.c_str());

	std::vector<PendingTraderBinding> pendingTraderBindings;
	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgExecuter->size(); idx++)
	{
		WTSVariant* cfgItem = cfgExecuter->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		if (id == NULL || id[0] == '\0' || _cta_engine.findExecuter(id) != NULL ||
			_sel_engine.findExecuter(id) != NULL)
		{
			WTSLogger::error("Executor id is empty or already registered: {}", id == NULL ? "<null>" : id);
			return false;
		}
		std::string name = cfgItem->getCString("name");	//local,diff,dist
		if (name.empty())
			name = "local";

		try
		{
			if (name == "local")
			{
				std::shared_ptr<WtLocalExecuter> executer(new WtLocalExecuter(&_exe_factory, id, &_data_mgr));
				ExecCmdPtr owner = executer;
				// Publish before init so every failed or partially initialized host is
				// retained by the manager and covered by fail-closed quiesce.
				_cta_engine.addExecuter(owner);
				if (!executer->init(cfgItem))
				{
					WTSLogger::error("Initializing local executor {} failed", id);
					return false;
				}
				const char* tid = cfgItem->getCString("trader");
				if (tid == NULL || tid[0] == '\0')
					WTSLogger::error("No Trader configured for Executer {}", id);
				else if (TraderAdapterPtr trader = _traders.getAdapter(tid))
				{
					executer->setTrader(trader.get());
					pendingTraderBindings.push_back({ trader, owner, executer.get() });
				}
				else
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
			}
			else if (name == "diff")
			{
				std::shared_ptr<WtDiffExecuter> executer(new WtDiffExecuter(&_exe_factory, id, &_data_mgr, &_bd_mgr));
				ExecCmdPtr owner = executer;
				_cta_engine.addExecuter(owner);
				if (!executer->init(cfgItem))
				{
					WTSLogger::error("Initializing diff executor {} failed", id);
					return false;
				}
				const char* tid = cfgItem->getCString("trader");
				if (tid == NULL || tid[0] == '\0')
					WTSLogger::error("No Trader configured for Executer {}", id);
				else if (TraderAdapterPtr trader = _traders.getAdapter(tid))
				{
					executer->setTrader(trader.get());
					pendingTraderBindings.push_back({ trader, owner, executer.get() });
				}
				else
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
			}
			else if (name == "dist")
			{
				std::shared_ptr<WtDistExecuter> executer(new WtDistExecuter(id));
				_cta_engine.addExecuter(executer);
				if (!executer->init(cfgItem))
				{
					WTSLogger::error("Initializing distributed executor {} failed", id);
					return false;
				}
			}
			else
			{
				WTSLogger::error("Unknown executor kind {} for {}", name, id);
				return false;
			}
		}
		catch (...)
		{
			WTSLogger::error("Initializing executor {} threw an exception; registered owners will be quiesced", id);
			return false;
		}
		count++;
	}

	// Every executor, its stub and its manager entry must exist before an
	// already-ready adapter can synchronously replay initial readiness.
	try
	{
		for (const PendingTraderBinding& binding : pendingTraderBindings)
			binding.trader->addSink(binding.sink);
	}
	catch (...)
	{
		WTSLogger::error("Registering executor trader sinks failed; registered owners will be quiesced");
		return false;
	}

	WTSLogger::info("{} executers loaded", count);

	return true;
}

bool WtRunner::initTraders(WTSVariant* cfgTrader)
{
	// This configuration replaces the active set. On an invalid item, release
	// every adapter already registered in this pass before returning failure.
	if (!_traders.getAdapters().empty())
	{
		shutdownRuntime(false);
		WTSLogger::error("Runner cannot replace traders while their sink owners are still registered");
		return false;
	}
	if (cfgTrader == NULL || cfgTrader->type() != WTSVariant::VT_Array)
		return false;
	
	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgTrader->size(); idx++)
	{
		WTSVariant* cfgItem = cfgTrader->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		TraderAdapterPtr adapter(new TraderAdapter(&_notifier));
		if (!adapter->init(id, cfgItem, &_bd_mgr, &_act_policy))
		{
			WTSLogger::error("Initializing trader {} failed", id);
			shutdownRuntime(false);
			return false;
		}

		if (!_traders.addAdapter(id, adapter))
		{
			WTSLogger::error("Registering trader {} failed", id);
			shutdownRuntime(false);
			return false;
		}

		count++;
	}

	WTSLogger::info("{} traders loaded", count);

	return true;
}

void WtRunner::run(bool bAsync /* = false */)
{
	bool startupFailed = false;
	{
		std::lock_guard<std::recursive_mutex> lifecycleLock(_lifecycle_mutex);
		if (_configuring || _shutdown_requested.load(std::memory_order_acquire) ||
			_runtime_started.load(std::memory_order_acquire))
		{
			WTSLogger::error("Runner cannot start while configuration, shutdown, or another run is active");
			return;
		}
		if (_engine == NULL)
		{
			WTSLogger::error("Runner is not configured; run skipped");
			return;
		}

		_runtime_started.store(true, std::memory_order_release);
		{
			ScopedBoolReset startupGuard(_runtime_starting);
			try
			{
				_parsers.run();
				if (!_shutdown_requested.load(std::memory_order_acquire))
					_traders.run();
				if (!_shutdown_requested.load(std::memory_order_acquire))
					_engine->run();
			}
			catch (...)
			{
				startupFailed = true;
				print_stack_trace([](const char* message) {
					WTSLogger::error(message);
				});
			}
		}
		if (startupFailed)
			_shutdown_requested.store(true, std::memory_order_release);
		if (startupFailed || _shutdown_requested.load(std::memory_order_acquire))
		{
			shutdownRuntime(true);
			return;
		}
	}

	try
	{
		/*
		 *	By Wesley @ 2023.12.11 
		 *	搬到这里来的原因是，用自定义的signal_handler覆盖其他模块的signal_handler
		 */
		install_signal_hooks([](const char* message) {
			WTSLogger::error(message);
		}, [this](bool bStopped) {
			_to_exit.store(bStopped, std::memory_order_release);
			WTSLogger::info("Exit flag is {}", bStopped);
		});

		if(!bAsync)
		{
			while(!_to_exit.load(std::memory_order_acquire))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}
	catch (...)
	{
		print_stack_trace([](const char* message) {
			WTSLogger::error(message);
		});
	}
}

const char* LOG_TAGS[] = {
	"all",
	"debug",
	"info",
	"warn",
	"error",
	"fatal",
	"none"
};

void WtRunner::handleLogAppend(WTSLogLevel ll, const char* msg)
{
	_notifier.notify_log(LOG_TAGS[ll - 100], msg);
}

bool WtRunner::initEvtNotifier()
{
	WTSVariant* cfg = _config->get("notifier");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}
