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
#include <boost/asio/post.hpp>
#include "ExpParser.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "../WtDtCore/WtHelper.h"

#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"

#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/SignalHook.hpp"

namespace
{
// ParserAdapterMgr's map, dynamic-create reservations and quarantine ownership
// share one registry lock. Provider callbacks must run after releasing it.
std::mutex g_parser_registry_mutex;
std::condition_variable g_parser_registry_changed;
std::map<const WtDtRunner*, std::set<std::string>> g_parser_creating;

class ParserCreateReservation
{
public:
	ParserCreateReservation(const WtDtRunner* runner, const std::string& id,
		const bool* registryStopped, const std::atomic<bool>* exitRequested)
		: _runner(runner)
	{
		try
		{
			_id = id;
			std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
			if ((registryStopped != NULL && *registryStopped) ||
				(exitRequested != NULL && exitRequested->load()))
			{
				_stopped = true;
				return;
			}
			_acquired = g_parser_creating[_runner].insert(_id).second;
		}
		catch (...)
		{
			_failed = true;
		}
	}

	~ParserCreateReservation()
	{
		if (_acquired)
		{
			{
				std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
				auto it = g_parser_creating.find(_runner);
				if (it != g_parser_creating.end())
				{
					it->second.erase(_id);
					if (it->second.empty())
						g_parser_creating.erase(it);
				}
			}
			g_parser_registry_changed.notify_all();
		}
	}

	bool acquired() const { return _acquired; }
	bool failed() const { return _failed; }
	bool stopped() const { return _stopped; }

private:
	const WtDtRunner* _runner;
	std::string _id;
	bool _acquired { false };
	bool _failed { false };
	bool _stopped { false };
};

bool insert_parser_adapter_locked(ParserAdapterMgr& parsers, const std::string& id,
	const ParserAdapterPtr& adapter)
{
	if (adapter == NULL || id.empty())
		return false;
	return parsers._adapters.emplace(id, adapter).second;
}

std::string validate_runner_config(WTSVariant* config, WTSVariant*& parserFileConfig)
{
	WTSVariant* basefiles = config == NULL ? NULL : config->get("basefiles");
	if (basefiles == NULL || basefiles->type() != WTSVariant::VT_Object)
		return "basefiles";

	auto requireString = [](WTSVariant* value, const std::string& path) -> std::string {
		return value == NULL || value->type() != WTSVariant::VT_String ? path : std::string();
	};
	auto requireStringOrStringArray = [](WTSVariant* value, const std::string& path) -> std::string {
		if (value == NULL)
			return std::string();
		if (value->type() == WTSVariant::VT_String)
			return std::string();
		if (value->type() != WTSVariant::VT_Array)
			return path;
		for (uint32_t idx = 0; idx < value->size(); ++idx)
		{
			WTSVariant* item = value->get(idx);
			if (item == NULL || item->type() != WTSVariant::VT_String)
				return path + "[" + std::to_string(idx) + "]";
		}
		return std::string();
	};

	for (const char* key : { "session", "holiday", "hot", "second" })
	{
		if (!basefiles->has(key))
			continue;
		std::string invalid = requireString(basefiles->get(key), std::string("basefiles.") + key);
		if (!invalid.empty())
			return invalid;
	}
	for (const char* key : { "commodity", "contract" })
	{
		if (!basefiles->has(key))
			continue;
		std::string invalid = requireStringOrStringArray(basefiles->get(key), std::string("basefiles.") + key);
		if (!invalid.empty())
			return invalid;
	}
	WTSVariant* rules = basefiles->get("rules");
	if (rules != NULL)
	{
		if (rules->type() != WTSVariant::VT_Object)
			return "basefiles.rules";
		for (const std::string& tag : rules->memberNames())
		{
			std::string invalid = requireString(rules->get(tag), std::string("basefiles.rules.") + tag);
			if (!invalid.empty())
				return invalid;
		}
	}

	WTSVariant* writer = config->get("writer");
	if (writer == NULL || writer->type() != WTSVariant::VT_Object)
		return "writer";
	if (writer->has("module"))
	{
		std::string invalid = requireString(writer->get("module"), "writer.module");
		if (!invalid.empty())
			return invalid;
	}

	WTSVariant* parsers = config->get("parsers");
	if (parsers == NULL)
		return std::string(); // The top-level parser section is optional.

	auto validateParserArray = [](WTSVariant* array, const std::string& path) -> std::string {
		if (array == NULL || array->type() != WTSVariant::VT_Array)
			return path;
		for (uint32_t idx = 0; idx < array->size(); ++idx)
		{
			WTSVariant* item = array->get(idx);
			if (item == NULL || item->type() != WTSVariant::VT_Object)
				return path + "[" + std::to_string(idx) + "]";
		}
		return std::string();
	};

	if (parsers->type() == WTSVariant::VT_Array)
		return validateParserArray(parsers, "parsers");
	if (parsers->type() != WTSVariant::VT_String)
		return "parsers";

	const char* filename = parsers->asCString();
	if (filename == NULL || !StdFile::exists(filename))
		return std::string(); // A missing optional parser file has historically been skipped.

	parserFileConfig = WTSCfgLoader::load_from_file(filename);
	if (parserFileConfig == NULL || parserFileConfig->type() != WTSVariant::VT_Object)
		return "parsers.file";
	return validateParserArray(parserFileConfig->get("parsers"), "parsers.file.parsers");
}
}


WtDtRunner::WtDtRunner()
	: _cb_parser_evt(NULL)
	, _cb_parser_sub(NULL)
	, _dumper_for_bars(NULL)
	, _dumper_for_ticks(NULL)
	, _dumper_for_ordque(NULL)
	, _dumper_for_orddtl(NULL)
	, _dumper_for_trans(NULL)
	, _to_exit(false)
	, _allday_mode(false)
	, _startup_blocked(false)
	, _parser_registry_started(false)
	, _parser_registry_stopped(false)
{
}


WtDtRunner::~WtDtRunner()
{
	// StateMonitor borrows _data_mgr and _bd_mgr. Its worker must be joined
	// while those members are still alive, before member destruction begins.
	// The owner must not be destroyed from a monitor callback or concurrently
	// with other runner operations.
	_state_mon.stop();
}

void WtDtRunner::start(bool bAsync /* = false */, bool bAlldayMode /* = false */)
{
	std::vector<ParserAdapterPtr> parserSnapshot;
	bool startupBlocked = false;
	bool parserRegistryStopped = false;
	bool initialSnapshot = false;
	try
	{
		std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
		startupBlocked = _startup_blocked || _config_load_failed;
		parserRegistryStopped = _parser_registry_stopped || _to_exit.load();
		if (!startupBlocked && !parserRegistryStopped && !_parser_registry_started)
		{
			parserSnapshot.reserve(_parsers._adapters.size());
			for (const auto& entry : _parsers._adapters)
				parserSnapshot.emplace_back(entry.second);
			_parser_registry_started = true;
			initialSnapshot = true;
		}
	}
	catch (...)
	{
		WTSLogger::error("Could not snapshot data parsers; runner startup is deferred");
		return;
	}
	if (startupBlocked)
	{
		WTSLogger::error("Data runner startup blocked after a required component failed");
		return;
	}
	if (parserRegistryStopped)
	{
		WTSLogger::error("Data runner start rejected after its parser registry stopped");
		return;
	}
	// run() invokes provider connect callbacks and may re-enter the runner.
	bool allSubmitted = true;
	for (const ParserAdapterPtr& adapter : parserSnapshot)
	{
		if (adapter != NULL)
		{
			try
			{
				if (!adapter->run())
				{
					allSubmitted = false;
					WTSLogger::error("Parser {} connect command was not submitted", adapter->id());
				}
			}
			catch (...)
			{
				allSubmitted = false;
				WTSLogger::error("Parser {} connect threw during startup", adapter->id());
			}
		}
		else
			allSubmitted = false;
	}
	if (initialSnapshot)
		_initial_parser_commands_submitted.store(allSubmitted);

    if(!bAsync)
    {
		install_signal_hooks([this](const char* message) {
			if(!_to_exit.load())
				WTSLogger::error(message);
		}, [this](bool toExit) {
			if (_to_exit.load())
				return;
			_to_exit.store(toExit);
			WTSLogger::info("Exit flag is {}", _to_exit.load());
		});

		boost::asio::post(_async_io, [this, bAlldayMode]() {
			if(!bAlldayMode)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				_state_mon.run();
			}
		});

		StdThread trd([this] {
			while (!_to_exit.load())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				_async_io.run_one();
			}
		});

		trd.join();
		// Stop admitting parser creation once the runtime loop exits, then wait
		// for admitted initializations/connects to finish before start() returns.
		std::unique_lock<std::mutex> registryLock(g_parser_registry_mutex);
		_parser_registry_stopped = true;
		g_parser_registry_changed.wait(registryLock, [this] {
			auto creating = g_parser_creating.find(this);
			return creating == g_parser_creating.end() || creating->second.empty();
		});
    }
	else
	{
		if (!bAlldayMode)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			_state_mon.run();
		}
	}
}

void WtDtRunner::initialize(const char* cfgFile, const char* logCfg, const char* modDir /* = "" */, bool bCfgFile /* = true */, bool bLogCfgFile /* = true */)
{
	WTSLogger::init(logCfg, bLogCfgFile);
	WtHelper::set_module_dir(modDir);

	WTSVariant* config = NULL;
	if (bCfgFile)
		config = WTSCfgLoader::load_from_file(cfgFile);
	else
		config = WTSCfgLoader::load_from_content(cfgFile, false);

	if(config == NULL)
	{
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			_config_load_failed = true;
		}
		WTSLogger::error("Loading config file {} failed", cfgFile);
		return;
	}

	WTSVariant* parserFileConfig = NULL;
	const std::string invalidField = validate_runner_config(config, parserFileConfig);
	if (!invalidField.empty())
	{
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			_startup_blocked = true;
		}
		WTSLogger::error("Invalid data runner configuration at {}; startup is blocked", invalidField);
		if (parserFileConfig != NULL)
			parserFileConfig->release();
		config->release();
		return;
	}
	{
		std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
		_config_load_failed = false;
	}

	//基础数据文件
	WTSVariant* cfgBF = config->get("basefiles");
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
		WTSLogger::log_raw(LL_INFO, "Hot rules loaded");
	}

	if (cfgBF->get("second"))
	{
		_hot_mgr.loadSeconds(cfgBF->getCString("second"));
		WTSLogger::log_raw(LL_INFO, "Second rules loaded");
	}

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

	// Initialize date-dependent state before starting any caster, writer or parser.
	_allday_mode = config->getBoolean("allday");
	if (!_allday_mode)
	{
		if (!_state_mon.initialize(config->getCString("statemonitor"), &_bd_mgr, &_data_mgr))
		{
			{
				std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
				_startup_blocked = true;
			}
			if (parserFileConfig != NULL)
				parserFileConfig->release();
			config->release();
			WTSLogger::error("State monitor initialization failed; runner startup is blocked");
			return;
		}
	}
	else
	{
		WTSLogger::info("datakit will run in allday mode");
	}

	if (config->has("shmcaster"))
	{
		WTSVariant* shmCfg = config->get("shmcaster");
		if (_shm_caster.init(shmCfg))
		{
			_data_mgr.add_caster(&_shm_caster);
		}
		else if (shmCfg != NULL && shmCfg->getBoolean("active"))
		{
			// initialize() is a void API; keep the failure in runner state so
			// start() cannot run parsers or the data writer after a required caster
			// failed. Do not register the uninitialized caster.
			{
				std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
				_startup_blocked = true;
			}
			WTSLogger::error("Active shared-memory caster initialization failed; runner startup is blocked");
			if (parserFileConfig != NULL)
				parserFileConfig->release();
			config->release();
			return;
		}
	}

	if(config->has("broadcaster"))
	{
		_udp_caster.init(config->get("broadcaster"), &_bd_mgr, &_data_mgr);
		_data_mgr.add_caster(&_udp_caster);
	}

	initDataMgr(config->get("writer"), _allday_mode);

	if (config->has("index"))
	{
		std::string path = fmt::format("{}index/", WtHelper::get_cwd());
		_idx_factory.loadFactories(path.c_str());

		//如果存在指数模块要，配置指数
		const char* filename = config->getCString("index");
		WTSLogger::info("Reading index config from {}...", filename);
		WTSVariant* var = WTSCfgLoader::load_from_file(filename);
		if (var)
		{
			_idx_factory.init(var, &_hot_mgr, &_bd_mgr, &_data_mgr);
			var->release();
		}
		else
		{
			WTSLogger::error("Loading index config {} failed", filename);
		}
	}

	WTSVariant* cfgParser = config->get("parsers");
	if (cfgParser)
	{
		if (cfgParser->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgParser->asCString();
			if (parserFileConfig != NULL)
			{
				WTSLogger::info("Reading parser config from {}...", filename);
				initParsers(parserFileConfig->get("parsers"));
			}
			else
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
			}
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			initParsers(cfgParser);
		}
	}
	else
		WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers");

	if (parserFileConfig != NULL)
		parserFileConfig->release();
	config->release();
}

void WtDtRunner::initDataMgr(WTSVariant* config, bool bAlldayMode /* = false */)
{
	_data_mgr.init(config, &_bd_mgr, bAlldayMode ? NULL : &_state_mon);
}

void WtDtRunner::initParsers(WTSVariant* cfg)
{
	auto blockStartup = [this](const char* message, const std::string& id) {
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			_startup_blocked = true;
		}
		WTSLogger::error(message, id);
	};

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
			static uint32_t auto_parserid = 1000;
			realid = fmtutil::format<64>("auto_parser_{}", auto_parserid++);
		}

		ParserCreateReservation reservation(this, realid, &_parser_registry_stopped, &_to_exit);
		if (!reservation.acquired())
		{
			if (reservation.stopped())
			{
				WTSLogger::error("Parser {} configuration rejected because runner has stopped", realid);
				return;
			}
			blockStartup(reservation.failed() ?
				"Parser {} configuration reservation failed; startup blocked" :
				"Parser {} is already being created; startup blocked", realid);
			return;
		}

		bool alreadyExists = false;
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			alreadyExists = _parsers.getAdapter(realid.c_str()) != NULL;
		}
		if (alreadyExists)
		{
			blockStartup("Parser {} is already registered; startup blocked", realid);
			return;
		}

		bool capacityReserved = false;
		try
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			auto creating = g_parser_creating.find(this);
			const size_t pending = creating == g_parser_creating.end() ? 0 : creating->second.size();
			if (pending <= _quarantined_ext_parsers.max_size() - _quarantined_ext_parsers.size())
			{
				_quarantined_ext_parsers.reserve(_quarantined_ext_parsers.size() + pending);
				capacityReserved = true;
			}
		}
		catch (...)
		{
			// No API has been handed to provider code yet.
		}
		if (!capacityReserved)
		{
			blockStartup("Parser {} quarantine reservation failed; startup blocked", realid);
			return;
		}

		ParserAdapterPtr adapter;
		try { adapter.reset(new ParserAdapter(&_bd_mgr, &_data_mgr, &_idx_factory)); }
		catch (...)
		{
			blockStartup("Parser {} candidate allocation failed; startup blocked", realid);
			return;
		}
		bool blockedBeforeInit = false;
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			blockedBeforeInit = _startup_blocked || _parser_registry_stopped || _to_exit.load();
			if (!blockedBeforeInit)
			{
				// Keep the adapter and its borrowed graph alive before init() can call
				// the provider. Capacity was reserved for all current candidates.
				_quarantined_ext_parsers.emplace_back(adapter);
			}
		}
		if (blockedBeforeInit)
		{
			WTSLogger::error("Parser {} initialization rejected after runner shutdown or failure", realid);
			return;
		}

		bool initialized = false;
		try { initialized = adapter->init(realid.c_str(), cfgItem); }
		catch (...)
		{
			blockStartup("Parser {} initialization threw; candidate quarantined and startup blocked", realid);
			return;
		}
		if (!initialized)
		{
			blockStartup("Parser {} initialization failed; candidate quarantined and startup blocked", realid);
			return;
		}

		bool added = false;
		bool runNow = false;
		bool blockedAtPublication = false;
		{
			std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
			blockedAtPublication = _startup_blocked || _parser_registry_stopped || _to_exit.load();
			try
			{
				if (!blockedAtPublication)
					added = insert_parser_adapter_locked(_parsers, realid, adapter);
			}
			catch (...)
			{
				try
				{
					auto partial = _parsers._adapters.find(realid);
					if (partial != _parsers._adapters.end() && partial->second == adapter)
						_parsers._adapters.erase(partial);
				}
				catch (...)
				{
					std::terminate();
				}
			}
			runNow = added && _parser_registry_started;
			if (added && !runNow)
			{
				auto quarantined = std::find(_quarantined_ext_parsers.begin(),
					_quarantined_ext_parsers.end(), adapter);
				if (quarantined != _quarantined_ext_parsers.end())
					_quarantined_ext_parsers.erase(quarantined);
			}
		}
		if (!added)
		{
			if (!blockedAtPublication)
				blockStartup("Parser {} registration failed; candidate quarantined and startup blocked", realid);
			return;
		}
		if (runNow)
		{
			bool stoppedBeforeConnect = false;
			{
				std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
				stoppedBeforeConnect = _parser_registry_stopped || _startup_blocked || _to_exit.load();
				if (stoppedBeforeConnect)
				{
					auto registered = _parsers._adapters.find(realid);
					if (registered != _parsers._adapters.end() && registered->second == adapter)
						_parsers._adapters.erase(registered);
				}
			}
			if (stoppedBeforeConnect)
			{
				WTSLogger::error("Parser {} connect rejected after runner shutdown began; candidate quarantined", realid);
				return;
			}

			bool started = false;
			try { started = adapter->run(); }
			catch (...) { }
			if (!started)
			{
				{
					std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
					auto registered = _parsers._adapters.find(realid);
					if (registered != _parsers._adapters.end() && registered->second == adapter)
						_parsers._adapters.erase(registered);
					_startup_blocked = true;
				}
				WTSLogger::error("Parser {} connect failed; candidate quarantined and startup blocked", realid);
				return;
			}
			bool stoppedAfterConnect = false;
			{
				std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
				stoppedAfterConnect = _parser_registry_stopped || _startup_blocked || _to_exit.load();
				if (stoppedAfterConnect)
				{
					auto registered = _parsers._adapters.find(realid);
					if (registered != _parsers._adapters.end() && registered->second == adapter)
						_parsers._adapters.erase(registered);
				}
				else
				{
					auto quarantined = std::find(_quarantined_ext_parsers.begin(),
						_quarantined_ext_parsers.end(), adapter);
					if (quarantined != _quarantined_ext_parsers.end())
						_quarantined_ext_parsers.erase(quarantined);
				}
			}
			if (stoppedAfterConnect)
			{
				WTSLogger::error("Parser {} completed connect after runner shutdown began; candidate quarantined", realid);
				return;
			}
		}
	}

	size_t parserCount = 0;
	{
		std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
		parserCount = _parsers.size();
	}
	WTSLogger::info("{} market data parsers loaded in total", parserCount);
}

#pragma region "Extended Parser"
void WtDtRunner::registerParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)
{
	_cb_parser_evt = cbEvt;
	_cb_parser_sub = cbSub;

	WTSLogger::info("Callbacks of Extented Parser registration done");
}

void WtDtRunner::parser_init(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_INIT, id);
}

void WtDtRunner::parser_connect(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_CONNECT, id);
}

void WtDtRunner::parser_disconnect(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_DISCONNECT, id);
}

void WtDtRunner::parser_release(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_RELEASE, id);
}

void WtDtRunner::parser_subscribe(const char* id, const char* code)
{
	if (_cb_parser_sub)
		_cb_parser_sub(id, code, true);
}

void WtDtRunner::parser_unsubscribe(const char* id, const char* code)
{
	if (_cb_parser_sub)
		_cb_parser_sub(id, code, false);
}

void WtDtRunner::on_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag)
{
	ParserAdapterPtr adapter;
	{
		std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
		adapter = _parsers.getAdapter(id);
	}
	if (adapter)
	{
		WTSTickData* newTick = WTSTickData::create(*curTick);
		adapter->handleQuote(newTick, uProcFlag);
		newTick->release();
	}
	else
	{
		WTSLogger::warn("Parser {} not exists", id);
	}
}

bool WtDtRunner::createExtParser(const char* id)
{
	bool startupBlocked = false;
	bool registryStopped = false;
	{
		std::lock_guard<std::mutex> registryLock(g_parser_registry_mutex);
		startupBlocked = _startup_blocked;
		registryStopped = _parser_registry_stopped || _to_exit.load();
	}
	if (registryStopped)
	{
		WTSLogger::error("Cannot create parser after runner stopped");
		return false;
	}
	if (startupBlocked)
	{
		WTSLogger::error("Cannot create parser: data runner startup is blocked");
		return false;
	}

	if (id == NULL || id[0] == '\0')
	{
		WTSLogger::error("Extended parser id must not be empty");
		return false;
	}
	std::string parserId;
	try { parserId.assign(id); }
	catch (...)
	{
		WTSLogger::error("Extended parser id could not be copied");
		return false;
	}

	// Creation is valid before start (start will snapshot this adapter) and while
	// running (the adapter is connected before success is returned).
	// Reserve the id before initExt: ExpParser::init forwards callbacks, which
	// may re-enter this method, and duplicate requests must not initialize twice.
	ParserCreateReservation reservation(this, parserId, &_parser_registry_stopped, &_to_exit);
	if (!reservation.acquired())
	{
		if (reservation.stopped())
		{
			WTSLogger::error("Extended parser creation rejected after runner stopped");
			return false;
		}
		WTSLogger::error(reservation.failed() ?
			"Parser {} creation reservation failed" :
			"Parser {} is already being created", parserId);
		return false;
	}

	bool alreadyExists = false;
	{
		std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
		alreadyExists = _parsers.getAdapter(parserId.c_str()) != NULL;
	}
	if (alreadyExists)
	{
		WTSLogger::error("Extended parser {} already exists", parserId);
		return false;
	}

	// Reserve enough quarantine slots for every outstanding creation on this
	// runner before any API can retain an adapter SPI. No registry lock is held
	// across initExt(), which invokes parser/user callbacks synchronously.
	bool capacityOverflow = false;
	try
	{
		std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
		auto creating = g_parser_creating.find(this);
		const size_t pending = creating == g_parser_creating.end() ? 0 : creating->second.size();
		if (pending > _quarantined_ext_parsers.max_size() - _quarantined_ext_parsers.size())
			capacityOverflow = true;
		else
			_quarantined_ext_parsers.reserve(_quarantined_ext_parsers.size() + pending);
	}
	catch (...)
	{
		WTSLogger::error("Extended parser {} quarantine reservation failed before API initialization", parserId);
		return false;
	}
	if (capacityOverflow)
	{
		WTSLogger::error("Extended parser {} quarantine capacity overflow", parserId);
		return false;
	}

	ParserAdapterPtr adapter;
	std::unique_ptr<ExpParser> parser;
	try
	{
		adapter.reset(new ParserAdapter(&_bd_mgr, &_data_mgr, &_idx_factory));
		parser.reset(new ExpParser(parserId.c_str()));
	}
	catch (...)
	{
		WTSLogger::error("Extended parser {} candidate allocation failed", parserId);
		return false;
	}
	bool blockedBeforeInit = false;
	{
		// Publish the candidate to quarantine before handing over the API pointer.
		// Capacity was reserved above for all in-flight creations, so this does
		// not allocate after provider code can retain the adapter.
		std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
		blockedBeforeInit = _startup_blocked || _parser_registry_stopped || _to_exit.load();
		if (!blockedBeforeInit)
			_quarantined_ext_parsers.emplace_back(adapter);
	}
	if (blockedBeforeInit)
	{
		WTSLogger::error("Extended parser {} initialization rejected after runner shutdown or failure", parserId);
		return false;
	}
	ExpParser* api = parser.release();
	bool initialized = false;
	try
	{
		initialized = adapter->initExt(parserId.c_str(), api);
	}
	catch (...)
	{
		WTSLogger::error("Extended parser {} initialization threw; candidate quarantined", parserId);
		return false;
	}
	if (!initialized)
	{
		WTSLogger::error("Extended parser {} initialization failed; candidate quarantined", parserId);
		return false;
	}

	bool added = false;
	bool registrationThrew = false;
	bool runNow = false;
	bool blockedAtPublication = false;
	{
		std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
		blockedAtPublication = _startup_blocked || _parser_registry_stopped || _to_exit.load();
		try
		{
			if (!blockedAtPublication)
				added = insert_parser_adapter_locked(_parsers, parserId, adapter);
		}
		catch (...)
		{
			registrationThrew = true;
			// Keep a partially published adapter out of the runnable manager;
			// the pre-existing quarantine owner remains alive for safe retry.
			try
			{
				auto partial = _parsers._adapters.find(parserId);
				if (partial != _parsers._adapters.end() && partial->second == adapter)
					_parsers._adapters.erase(partial);
			}
			catch (...)
			{
				std::terminate();
			}
		}
		if (added)
		{
			runNow = _parser_registry_started;
			if (!runNow)
			{
				auto quarantined = std::find(_quarantined_ext_parsers.begin(),
					_quarantined_ext_parsers.end(), adapter);
				if (quarantined != _quarantined_ext_parsers.end())
					_quarantined_ext_parsers.erase(quarantined);
			}
		}
	}
	if (!added)
	{
		WTSLogger::error("Extended parser {} publication {}; candidate quarantined", parserId,
			blockedAtPublication ? "blocked by runner state" :
				(registrationThrew ? "threw" : "failed"));
		return false;
	}
	if (runNow)
	{
		bool stoppedBeforeConnect = false;
		{
			std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
			stoppedBeforeConnect = _parser_registry_stopped || _startup_blocked || _to_exit.load();
			if (stoppedBeforeConnect)
			{
				auto registered = _parsers._adapters.find(parserId);
				if (registered != _parsers._adapters.end() && registered->second == adapter)
					_parsers._adapters.erase(registered);
			}
		}
		if (stoppedBeforeConnect)
		{
			WTSLogger::error("Extended parser {} connect rejected after runner shutdown began; candidate quarantined", parserId);
			return false;
		}

		bool started = false;
		bool connectThrew = false;
		try { started = adapter->run(); }
		catch (...) { connectThrew = true; }
		if (!started)
		{
			{
				std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
				try
				{
					auto partial = _parsers._adapters.find(parserId);
					if (partial != _parsers._adapters.end() && partial->second == adapter)
						_parsers._adapters.erase(partial);
				}
				catch (...)
				{
					std::terminate();
				}
			}
			WTSLogger::error("Extended parser {} connect {}; candidate quarantined", parserId,
				connectThrew ? "threw" : "failed");
			return false;
		}
		bool stoppedAfterConnect = false;
		{
			std::lock_guard<std::mutex> lock(g_parser_registry_mutex);
			stoppedAfterConnect = _parser_registry_stopped || _startup_blocked || _to_exit.load();
			if (stoppedAfterConnect)
			{
				auto registered = _parsers._adapters.find(parserId);
				if (registered != _parsers._adapters.end() && registered->second == adapter)
					_parsers._adapters.erase(registered);
			}
			else
			{
				auto quarantined = std::find(_quarantined_ext_parsers.begin(),
					_quarantined_ext_parsers.end(), adapter);
				if (quarantined != _quarantined_ext_parsers.end())
					_quarantined_ext_parsers.erase(quarantined);
			}
		}
		if (stoppedAfterConnect)
		{
			WTSLogger::error("Extended parser {} completed connect after runner shutdown began; candidate quarantined", parserId);
			return false;
		}
	}

	WTSLogger::info("Extended parser {} created", parserId);
	return true;
}

#pragma endregion

bool WtDtRunner::createExtDumper(const char* id)
{
	ExpDumperPtr dumper(new ExpDumper(id));
	_dumpers[id] = dumper;

	_data_mgr.add_ext_dumper(id, dumper.get());

	WTSLogger::info("Extended dumper {} created", id);
	return true;
}

void WtDtRunner::registerExtDumper(FuncDumpBars barDumper, FuncDumpTicks tickDumper)
{
	_dumper_for_bars = barDumper;
	_dumper_for_ticks = tickDumper;
}

void WtDtRunner::registerExtHftDataDumper(FuncDumpOrdQue ordQueDumper, FuncDumpOrdDtl ordDtlDumper, FuncDumpTrans transDumper)
{
	_dumper_for_ordque = ordQueDumper;
	_dumper_for_orddtl = ordDtlDumper;
	_dumper_for_trans = transDumper;
}

bool WtDtRunner::dumpHisTicks(const char* id, const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count)
{
	if (NULL == _dumper_for_ticks)
	{
		WTSLogger::error("Extended tick dumper not enabled");
		return false;
	}

	return _dumper_for_ticks(id, stdCode, uDate, ticks, count);
}

bool WtDtRunner::dumpHisBars(const char* id, const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count)
{
	if (NULL == _dumper_for_bars)
	{
		WTSLogger::error("Extended bar dumper not enabled");
		return false;
	}

	return _dumper_for_bars(id, stdCode, period, bars, count);
}

bool WtDtRunner::dumpHisOrdDtl(const char* id, const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count)
{
	if (NULL == _dumper_for_orddtl)
	{
		WTSLogger::error("Extended order detail dumper not enabled");
		return false;
	}

	return _dumper_for_orddtl(id, stdCode, uDate, items, count);
}

bool WtDtRunner::dumpHisOrdQue(const char* id, const char* stdCode, uint32_t uDate, WTSOrdQueStruct* items, uint32_t count)
{
	if (NULL == _dumper_for_ordque)
	{
		WTSLogger::error("Extended order queue dumper not enabled");
		return false;
	}

	return _dumper_for_ordque(id, stdCode, uDate, items, count);
}

bool WtDtRunner::dumpHisTrans(const char* id, const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count)
{
	if (NULL == _dumper_for_trans)
	{
		WTSLogger::error("Extended transaction dumper not enabled");
		return false;
	}

	return _dumper_for_trans(id, stdCode, uDate, items, count);
}
