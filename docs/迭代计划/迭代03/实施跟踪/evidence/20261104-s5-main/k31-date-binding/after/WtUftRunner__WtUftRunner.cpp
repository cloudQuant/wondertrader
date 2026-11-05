/*!
 * /file WtUftRunner.cpp
 * /project	WonderTrader
 *
 * /author Wesley
 * /date 2020/03/30
 * 
 * /brief 
 */
#include "WtUftRunner.h"
#include "../WtUftCore/ShareManager.h"

#include <exception>
#include <algorithm>
#include <unordered_set>
#include <memory>

#include "../WtUftCore/WtHelper.h"
#include "../WtUftCore/UftStraContext.h"

#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSUtils/SignalHook.hpp"
#include "../Share/StrUtil.hpp"

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



WtUftRunner::WtUftRunner()
	:_config(NULL)
	, _to_exit(false)
{
	install_signal_hooks([](const char* message) {
		WTSLogger::error(message);
	}, [this](bool bStopped) {
		_to_exit.store(bStopped, std::memory_order_release);
		WTSLogger::info("Exit flag is {}", bStopped);
	});
}


WtUftRunner::~WtUftRunner()
{
	if (!shutdown())
	{
		// Destroying members while a parser/trader owner may still call back would be
		// use-after-free. Parser owners merely retained for lack of drain proof are kept
		// alive by the process entry (main.cpp never destroys such a runner); reaching
		// here is an unrecoverable teardown failure (I03-NEW-058).
		WTSLogger::error("UFT runner cannot be destroyed before callback owners are quiescent (parser owners retained: {})",
			parserOwnersRetained());
		std::terminate();
	}
}

bool WtUftRunner::shutdown()
{
	if (_shutdown_complete.load(std::memory_order_acquire))
		return true;

	if (_shutdown_running.test_and_set(std::memory_order_acquire))
		return false;
	struct ShutdownFlagGuard
	{
		std::atomic_flag& flag;
		~ShutdownFlagGuard() { flag.clear(std::memory_order_release); }
	} guard{ _shutdown_running };

	_parser_owners_retained.store(false, std::memory_order_release);
	_shutdown_requested.store(true, std::memory_order_release);
	_to_exit.store(true, std::memory_order_release);
	if (_config_in_progress.load(std::memory_order_acquire))
		return false;
	{
		std::lock_guard<std::mutex> lock(_parser_start_mutex);
		// A full runner startup retains every dependency until it leaves its
		// lease. Never wait here: arbitrary startup callbacks can reenter
		// shutdown from another thread. The owner must retry after startup.
		if (_parser_start_active && !_parser_start_attempted)
			return false;
	}
	if (isInParserCallbackOnCurrentThread())
	{
		WTSLogger::warn("UFT shutdown requested from a parser callback; defer teardown until the callback returns");
		return false;
	}
	if (!closeParserStartAndDrain())
		return false;
	_traders.requestStopSinkNotifications();
	_uft_engine.request_stop();
	if (_share_domain_attached.load(std::memory_order_acquire))
		ShareManager::self().request_stop_watching();
	if (_share_domain_attached.load(std::memory_order_acquire)
		&& !ShareManager::self().wait_watching_stopped())
		return false;
	if (!_uft_engine.wait_stopped())
		return false;

	if (_parser_quarantine_pending.load(std::memory_order_acquire))
	{
		_parser_owners_retained.store(true, std::memory_order_release);
		WTSLogger::error("UFT runner shutdown cannot release a failed parser while callback quiescence is unproven; keep the runner alive");
		return false;
	}

	if (!_inputs_stopped.load(std::memory_order_acquire))
	{
		// Stop producers while the engine and its strategy contexts are still alive.
		_parsers.release();
		if (_parsers.hasRetainedAdapters())
		{
			_parser_owners_retained.store(true, std::memory_order_release);
			WTSLogger::error("UFT parser teardown remains pending; retain the runner graph and retry shutdown after provider quiescence");
			return false;
		}
		_uft_engine.stop();
		_inputs_stopped.store(true, std::memory_order_release);
	}

	if (!_traders.waitSinkNotificationsStopped())
		return false;

	_traders.release();
	if (_config != NULL)
	{
		_config->release();
		_config = NULL;
	}
	_shutdown_complete.store(true, std::memory_order_release);
	return true;
}

bool WtUftRunner::startParsers()
{
	return startParsers(false);
}

bool WtUftRunner::startParsers(bool runnerLease)
{
	{
		std::lock_guard<std::mutex> lock(_parser_start_mutex);
		if (runnerLease)
		{
			if (!_parser_start_active || _parser_start_attempted
				|| _parser_start_owner != std::this_thread::get_id()
				|| _parser_start_closed || _shutdown_requested.load(std::memory_order_acquire)
				|| _parser_quarantine_pending.load(std::memory_order_acquire))
				return false;
		}
		else
		{
			if (_parser_start_closed || _parser_start_attempted || _parser_start_active
				|| _shutdown_requested.load(std::memory_order_acquire)
				|| _parser_quarantine_pending.load(std::memory_order_acquire))
				return false;
			_parser_start_attempted = true;
			_parser_start_active = true;
			_parser_start_owner = std::this_thread::get_id();
		}
	}

	try
	{
		// Never hold _parser_start_mutex across plugin code.
		_parsers.run();
	}
	catch (...)
	{
		if (!runnerLease) finishParserStart();
		throw;
	}
	if (!runnerLease) finishParserStart();
	return true;
}

void WtUftRunner::finishParserStart()
{
	{
		std::lock_guard<std::mutex> lock(_parser_start_mutex);
		_parser_start_active = false;
		_parser_start_attempted = true;
		_parser_start_owner = std::thread::id();
	}
	_parser_start_cv.notify_all();
}

bool WtUftRunner::closeParserStartAndDrain()
{
	std::unique_lock<std::mutex> lock(_parser_start_mutex);
	_parser_start_closed = true;
	_parser_start_cv.notify_all();
	if (_parser_start_active && _parser_start_owner == std::this_thread::get_id())
		return false;
	_parser_start_cv.wait(lock, [this]() { return !_parser_start_active; });
	return true;
}

bool WtUftRunner::isInParserCallbackOnCurrentThread() const
{
	for (const auto& item : _parsers._adapters)
		if (item.second != NULL && item.second->isInParserCallbackOnCurrentThread())
			return true;
	for (const ParserAdapterPtr& candidate : _quarantined_parsers)
		if (candidate != NULL && candidate->isInParserCallbackOnCurrentThread())
			return true;
	return false;
}

void WtUftRunner::init(const std::string& filename)
{
	WTSLogger::init(filename.c_str());

	WtHelper::setInstDir(getBinDir());
}

bool WtUftRunner::config(const std::string& filename)
{
	if (_shutdown_requested.load(std::memory_order_acquire)
		|| _shutdown_complete.load(std::memory_order_acquire))
	{
		WTSLogger::error("UFT runner cannot be configured after shutdown has started");
		return false;
	}

	bool expected = false;
	if (!_config_attempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
	{
		WTSLogger::error("UFT runner configuration can only be attempted once");
		return false;
	}

	_config_in_progress.store(true, std::memory_order_release);
	struct ConfigProgressGuard
	{
		std::atomic<bool>& inProgress;
		~ConfigProgressGuard() { inProgress.store(false, std::memory_order_release); }
	} progressGuard{ _config_in_progress };
	if (_shutdown_requested.load(std::memory_order_acquire)
		|| _shutdown_complete.load(std::memory_order_acquire))
	{
		WTSLogger::error("UFT runner cannot be configured after shutdown has started");
		return false;
	}

	const bool configured = configImpl(filename);
	if (configured && !_shutdown_requested.load(std::memory_order_acquire))
		_config_succeeded.store(true, std::memory_order_release);

	if (!configured || _shutdown_requested.load(std::memory_order_acquire))
	{
		progressGuard.inProgress.store(false, std::memory_order_release);
		shutdown();
		return false;
	}

	return true;
}

bool WtUftRunner::configImpl(const std::string& filename)
{
	_config = WTSCfgLoader::load_from_file(filename.c_str());
	if(_config == NULL)
	{
		WTSLogger::error("Loading config file {} failed", filename);
		return false;
	}

	//基础数据文件
	WTSVariant* cfgBF = _config->get("basefiles");
	if (cfgBF == NULL)
	{
		WTSLogger::error("UFT basefiles configuration is missing");
		return false;
	}
	if (cfgBF->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("UFT basefiles configuration must be an object");
		return false;
	}

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

	WTSVariant* cfgEnv = _config->get("env");
	if (cfgEnv == NULL)
	{
		WTSLogger::error("UFT environment configuration is missing");
		return false;
	}

	const char* sessionID = "ALLDAY";
	if (cfgEnv->has("product"))
	{
		WTSVariant* cfgProd = cfgEnv->get("product");
		sessionID = cfgProd == NULL ? "" : cfgProd->getCString("session");
	}
	if (sessionID == NULL || sessionID[0] == '\0' || _bd_mgr.getSession(sessionID) == NULL)
	{
		WTSLogger::error("UFT session '{}' is missing from the loaded base data", sessionID == NULL ? "" : sessionID);
		return false;
	}

	if(!_act_policy.init(_config->getCString("bspolicy")))
	{
		WTSLogger::error("ActionPolicyMgr init failed, please check config");
		// Validate the mandatory action policy before initializing the engine,
		// shared domain, parsers, traders, or strategy contexts. Invalidate the
		// config as a startup latch so run() also refuses callers that ignore false.
		_config->release();
		_config = NULL;
		return false;
	}

	//初始化运行环境
	if (!initEngine())
	{
		WTSLogger::error("UFT engine initialization failed");
		return false;
	}

	//初始化数据管理
	if (!initDataMgr())
	{
		WTSLogger::error("UFT data manager initialization failed");
		return false;
	}

	if (_config->has("share_domain"))
	{
		WTSVariant* cfg = _config->get("share_domain");
		ShareManager::self().set_engine(&_uft_engine);
		_share_domain_attached.store(true, std::memory_order_release);

		ShareManager::self().initialize(cfg->getCString("module"));
		ShareManager::self().init_domain(cfg->getCString("name"));
	}

	//初始化行情通道
	WTSVariant* cfgParser = _config->get("parsers");
	bool parsersOk = true;
	if (cfgParser)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if(var)
				{
					if (!initParsers(var->get("parsers")))
					{
						parsersOk = false;
						WTSLogger::error("Loading parsers failed");
					}
					var->release();
				}
				else
				{
					WTSLogger::error("Loading parser config {} failed", filename);
					parsersOk = false;
				}
			}
			else
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
				parsersOk = false;
			}
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			if (!initParsers(cfgParser))
			{
				parsersOk = false;
				WTSLogger::error("Loading parsers failed");
			}
		}
		else
		{
			WTSLogger::error("Parser configuration must be an array or a file name");
			parsersOk = false;
		}
	}
	if (!parsersOk)
		return false;

	//初始化交易通道
	WTSVariant* cfgTraders = _config->get("traders");
	bool tradersOk = true;
	if (cfgTraders)
	{
		if (cfgTraders->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgTraders->asCString();
			if (StdFile::exists(filename))
			{
				WTSLogger::info("Reading trader config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					if (!initTraders(var->get("traders")))
					{
						tradersOk = false;
						WTSLogger::error("Loading traders failed");
					}
					var->release();
				}
				else
				{
					WTSLogger::error("Loading trader config {} failed", filename);
					tradersOk = false;
				}
			}
			else
			{
				WTSLogger::error("Trader configuration {} not exists", filename);
				tradersOk = false;
			}
		}
		else if (cfgTraders->type() == WTSVariant::VT_Array)
		{
			tradersOk = initTraders(cfgTraders);
		}
		else
		{
			WTSLogger::error("Trader configuration must be an array or a file name");
			tradersOk = false;
		}
	}
	if (!tradersOk)
		return false;

	if (!initUftStrategies())
	{
		WTSLogger::error("UFT strategy configuration failed");
		return false;
	}
	
	return true;
}

bool WtUftRunner::initUftStrategies()
{
	if (_config == NULL)
		return false;

	WTSVariant* strategies = _config->get("strategies");
	if (strategies == NULL)
		return true;
	if (strategies->type() != WTSVariant::VT_Object)
		return true;

	WTSVariant* cfg = strategies->get("uft");
	if (cfg == NULL)
		return true;
	if (cfg->type() != WTSVariant::VT_Array)
		return true;

	bool hasActiveStrategy = false;
	std::unordered_set<std::string> activeStrategyIds;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (cfgItem == NULL)
			continue;
		if (!cfgItem->getBoolean("active"))
			continue;

		hasActiveStrategy = true;
		const char* id = cfgItem->getCString("id");
		const std::string strategyId = id == NULL ? "" : id;
		if (!activeStrategyIds.insert(strategyId).second)
		{
			WTSLogger::error("Duplicate active UFT strategy id {}", strategyId);
			return false;
		}
	}
	if (!hasActiveStrategy)
		return true;

	std::string path = WtHelper::getCWD() + "uft/";
	if (!_uft_stra_mgr.loadFactories(path.c_str()))
	{
		WTSLogger::error("Loading UFT strategy factories from {} failed", path);
		return false;
	}

	struct PendingStrategyRegistration
	{
		std::shared_ptr<UftStraContext> context;
		TraderAdapterPtr trader;
	};
	std::vector<PendingStrategyRegistration> pending;
	pending.reserve(cfg->size());

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (cfgItem == NULL || !cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");

		UftStrategyPtr stra = _uft_stra_mgr.createStrategy(name, id);
		if (stra == NULL || stra->self() == NULL)
		{
			WTSLogger::error("UFT Strategy {}({}) create failed", name, id);
			return false;
		}
		WTSLogger::info("UFT Strategy {}({}) created", name, id);

		if (!stra->self()->init(cfgItem->get("params")))
		{
			WTSLogger::error("UFT Strategy {}({}) initialization failed", name, id);
			return false;
		}

		std::shared_ptr<UftStraContext> ctx(new UftStraContext(&_uft_engine, id));
		ctx->set_strategy(stra->self());

		const char* traderid = cfgItem->getCString("trader");
		TraderAdapterPtr trader = _traders.getAdapter(traderid);
		if (trader)
		{
			ctx->setTrader(trader.get());
		}
		else
		{
			WTSLogger::error("Trader {} not exists, binding trader to UFT strategy failed", traderid);
		}

		pending.push_back({ ctx, trader });
	}

	// addSink may synchronously deliver READY. Publish every initialized context
	// before attaching any sink so callbacks cannot observe partial registration.
	for (const PendingStrategyRegistration& registration : pending)
		_uft_engine.addContext(registration.context);

	for (const PendingStrategyRegistration& registration : pending)
		if (registration.trader)
			registration.trader->addSink(registration.context.get());

	return true;
}

bool WtUftRunner::initEngine()
{
	WTSVariant* cfg = _config->get("env");
	if (cfg == NULL)
		return false;

	WTSLogger::info("Trading enviroment initialzied with engine: UFT");
	_uft_engine.init(cfg, &_bd_mgr, &_data_mgr, &_notifier);

	_uft_engine.set_adapter_mgr(&_traders);

	return true;
}


bool WtUftRunner::initDataMgr()
{
	WTSVariant*cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	_data_mgr.init(cfg, &_uft_engine);
	WTSLogger::info("Data manager initialized");

	return true;
}

bool WtUftRunner::initParsers(WTSVariant* cfgParser)
{
	if (cfgParser == NULL)
		return false;

	uint32_t count = 0;
	uint32_t failed = 0;
	for (uint32_t idx = 0; idx < cfgParser->size(); idx++)
	{
		WTSVariant* cfgItem = cfgParser->get(idx);
		if(!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id == NULL ? "" : id;
		if (realid.empty())
		{
			static uint32_t auto_parserid = 1000;
			realid = fmtutil::format<64>("auto_parser_{}", auto_parserid++);
		}
		bool duplicateId = false;
		try
		{
			duplicateId = _parsers.getAdapter(realid.c_str()) != NULL;
			for (const ParserAdapterPtr& candidate : _quarantined_parsers)
				if (candidate && realid == candidate->id())
					duplicateId = true;
		}
		catch (...)
		{
			WTSLogger::error("Parser {} duplicate-id precheck failed", realid);
			failed++;
			continue;
		}
		if (duplicateId)
		{
			WTSLogger::error("Parser {} has a duplicate id; skip initialization", realid);
			failed++;
			continue;
		}

		ParserAdapterPtr adapter(new ParserAdapter);
		// K31=A: a module parser gets the runner's DateMappingV1 table when it exports the binder.
		adapter->setDateMapping(_bd_mgr.getDateMappingV1());
		try
		{
			// Keep the adapter alive before plugin code can register its SPI.
			_quarantined_parsers.push_back(adapter);
		}
		catch (...)
		{
			WTSLogger::error("Unable to reserve parser {} before initialization", realid);
			failed++;
			continue;
		}

		bool initialized = false;
		try
		{
			initialized = adapter->init(realid.c_str(), cfgItem, &_uft_engine, &_bd_mgr);
		}
		catch (...)
		{
			WTSLogger::error("Parser {} initialization threw; adapter candidate retained for inspection", realid);
		}
		if (!initialized || !adapter->isInitialized())
		{
			WTSLogger::error("Parser {} initialization failed; adapter was not registered", realid);
			if (adapter->hasApi())
			{
				// The candidate is already retained; do not call release() as a
				// substitute for the provider-specific callback-drain barrier.
				_parser_quarantine_pending.store(true, std::memory_order_release);
			}
			else if (releaseParserCandidateIfSafe(adapter))
			{
				const auto candidate = std::find(_quarantined_parsers.begin(), _quarantined_parsers.end(), adapter);
				if (candidate != _quarantined_parsers.end())
					_quarantined_parsers.erase(candidate);
			}
			else
				_parser_quarantine_pending.store(true, std::memory_order_release);
			failed++;
			continue;
		}

		if (!registerParserCandidate(realid, adapter))
		{
			WTSLogger::error("Parser {} registration failed; candidate retained outside the active manager", realid);
			if (adapter->hasApi())
				_parser_quarantine_pending.store(true, std::memory_order_release);
			else if (releaseParserCandidateIfSafe(adapter))
			{
				const auto candidate = std::find(_quarantined_parsers.begin(), _quarantined_parsers.end(), adapter);
				if (candidate != _quarantined_parsers.end())
					_quarantined_parsers.erase(candidate);
			}
			else
				_parser_quarantine_pending.store(true, std::memory_order_release);
			failed++;
			continue;
		}

		count++;
	}

	WTSLogger::info("{} parsers loaded, {} failed", count, failed);
	return failed == 0;
}

bool WtUftRunner::registerParserCandidate(const std::string& id, ParserAdapterPtr& adapter)
{
	const auto markUnsafeCandidate = [this, &adapter]() {
		if (adapter != NULL && adapter->hasApi())
			_parser_quarantine_pending.store(true, std::memory_order_release);
	};
	if (adapter == NULL || !adapter->hasApi() || !adapter->isInitialized())
	{
		markUnsafeCandidate();
		return false;
	}

	// initParsers prechecks duplicates before calling plugin code. Repeat the
	// check here because plugin initialization can reenter application code.
	try
	{
		if (_parsers.getAdapter(id.c_str()) != NULL)
		{
			markUnsafeCandidate();
			return false;
		}
		if (!_parsers.addAdapter(id.c_str(), adapter))
		{
			markUnsafeCandidate();
			return false;
		}
	}
	catch (...)
	{
		markUnsafeCandidate();
		return false;
	}

	const auto candidate = std::find(_quarantined_parsers.begin(), _quarantined_parsers.end(), adapter);
	if (candidate != _quarantined_parsers.end())
		_quarantined_parsers.erase(candidate);
	return true;
}

bool WtUftRunner::releaseParserCandidateIfSafe(ParserAdapterPtr& adapter)
{
	if (adapter == NULL || adapter->hasApi() || adapter->isTeardownUncertain())
		return false;

	try
	{
		// This path is limited to candidates with no API or opaque provider
		// ownership. It only releases retained config/module state from an early
		// failure; it does not treat release() as a callback drain.
		adapter->release();
	}
	catch (...)
	{
		return false;
	}
	return !adapter->hasApi() && !adapter->isTeardownUncertain() && !adapter->isTeardownPending();
}

bool WtUftRunner::initTraders(WTSVariant* cfgTrader)
{
	if (cfgTrader == NULL || cfgTrader->type() != WTSVariant::VT_Array)
		return false;
	
	uint32_t count = 0;
	uint32_t failed = 0;
	for (uint32_t idx = 0; idx < cfgTrader->size(); idx++)
	{
		WTSVariant* cfgItem = cfgTrader->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		if (id == NULL || id[0] == '\0')
		{
			WTSLogger::error("Active trader at index {} has an empty id", idx);
			failed++;
			continue;
		}

		TraderAdapterPtr adapter(new TraderAdapter());
		if (!adapter->init(id, cfgItem, &_bd_mgr, &_act_policy))
		{
			WTSLogger::error("Trader {} initialization failed; adapter was not registered", id);
			adapter->release();
			failed++;
			continue;
		}

		if (!_traders.addAdapter(id, adapter))
		{
			WTSLogger::error("Trader {} registration failed; duplicate or invalid id", id);
			adapter->release();
			failed++;
			continue;
		}

		count++;
	}

	WTSLogger::info("{} traders loaded, {} failed", count, failed);

	return failed == 0;
}

bool WtUftRunner::initEvtNotifier()
{
	WTSVariant* cfg = _config->get("notifier");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}

void WtUftRunner::run(bool bAsync /* = false */)
{
	try
	{
		if (!_config_succeeded.load(std::memory_order_acquire)
			|| _config_in_progress.load(std::memory_order_acquire)
			|| _parser_quarantine_pending.load(std::memory_order_acquire)
			|| _shutdown_requested.load(std::memory_order_acquire))
		{
			WTSLogger::error("UFT runner cannot run without a successful, active configuration");
			return;
		}

		bool startupAdmitted = false;
		{
			std::lock_guard<std::mutex> lock(_parser_start_mutex);
			if (!_parser_start_closed && !_parser_start_attempted && !_parser_start_active
				&& !_shutdown_requested.load(std::memory_order_acquire)
				&& !_parser_quarantine_pending.load(std::memory_order_acquire))
			{
				// active && !attempted denotes the full runner startup lease; direct
				// startParsers uses active && attempted and retains its old drain rule.
				_parser_start_active = true;
				_parser_start_owner = std::this_thread::get_id();
				startupAdmitted = true;
			}
		}
		if (!startupAdmitted)
		{
			WTSLogger::error("UFT runner startup already attempted or shutdown requested");
			return;
		}
		auto releaseStartup = [this](WtUftRunner*) { finishParserStart(); };
		std::unique_ptr<WtUftRunner, decltype(releaseStartup)> startupLease(this, releaseStartup);

		// WtUftEngine::run has a void interface. Check its configured session here
		// as well so a failed engine startup cannot fall through to parser/trader
		// startup or the runner's wait loop.
		WTSVariant* cfgEnv = _config == NULL ? NULL : _config->get("env");
		if (cfgEnv == NULL)
		{
			WTSLogger::error("UFT runner cannot start without a valid startup configuration");
			startupLease.reset();
			if (_shutdown_requested.load(std::memory_order_acquire)) shutdown();
			return;
		}

		const char* sessionID = "ALLDAY";
		if (cfgEnv->has("product"))
		{
			WTSVariant* cfgProd = cfgEnv->get("product");
			sessionID = cfgProd == NULL ? "" : cfgProd->getCString("session");
		}

		if (sessionID == NULL || sessionID[0] == '\0'
			|| _uft_engine.get_session_info(sessionID) == NULL)
		{
			WTSLogger::error("UFT runner cannot start without a valid session: {}",
				sessionID == NULL ? "" : sessionID);
			startupLease.reset();
			if (_shutdown_requested.load(std::memory_order_acquire)) shutdown();
			return;
		}

		_uft_engine.run();
		if (_shutdown_requested.load(std::memory_order_acquire))
		{
			startupLease.reset();
			shutdown();
			return;
		}

		if (!startParsers(true))
		{
			startupLease.reset();
			if (_shutdown_requested.load(std::memory_order_acquire))
				shutdown();
			else
				WTSLogger::error("UFT parser start was rejected because it already started or shutdown closed admission");
			return;
		}
		if (_shutdown_requested.load(std::memory_order_acquire))
		{
			startupLease.reset();
			shutdown();
			return;
		}
		_traders.run();
		if (_shutdown_requested.load(std::memory_order_acquire))
		{
			startupLease.reset();
			shutdown();
			return;
		}

		ShareManager::self().start_watching(2);
		startupLease.reset();
		if (_shutdown_requested.load(std::memory_order_acquire))
		{
			shutdown();
			return;
		}

		if (!bAsync)
		{
			while (!_to_exit.load(std::memory_order_acquire))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			if (!shutdown())
				WTSLogger::warn("UFT runner shutdown is pending; keep the runner alive and retry shutdown on a control thread");
		}
	}
	catch (...)
	{
		if (_shutdown_requested.load(std::memory_order_acquire))
			shutdown();
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

void WtUftRunner::handleLogAppend(WTSLogLevel /*ll*/, const char* /*msg*/)
{
}
