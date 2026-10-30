#include "WtExecRunner.h"

#include "../WtCore/WtHelper.h"
#include "../WtCore/WtDiffExecuter.h"
#include "../WtCore/WtDistExecuter.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../WTSUtils/SignalHook.hpp"

#include <vector>
#include <exception>

namespace
{
struct PendingTraderBinding
{
	TraderAdapterPtr trader;
	ExecCmdPtr owner; // Keep the sink alive until deferred registration.
	ITrdNotifySink* sink;
};

struct ScopedVariantRelease
{
	explicit ScopedVariantRelease(WTSVariant* value) : value(value) {}
	~ScopedVariantRelease() { if (value != NULL) value->release(); }
	WTSVariant* value;
};
}

#ifdef _MSC_VER
#include "../Common/mdump.h"
#include <filesystem>
namespace fs = std::filesystem;
const char* getModuleName()
{
	static char MODULE_NAME[250] = { 0 };
	if (strlen(MODULE_NAME) == 0)
	{

		GetModuleFileName(g_dllModule, MODULE_NAME, 250);
		fs::path p(MODULE_NAME);
		strcpy(MODULE_NAME, p.filename().string().c_str());
	}

	return MODULE_NAME;
}
#endif

WtExecRunner::WtExecRunner()
	: _config_failed(false)
	, _shutdown_complete(false)
	, _shutdown_in_progress(false)
	, _accepting_runtime_calls(true)
	, _accepting_context_calls(true)
	, _configuration_attempted(false)
	, _run_started(false)
	, _factory_operation_active(false)
	, _active_runtime_calls(0)
	, _active_context_calls(0)
	, _quiesce_coordinator(std::vector<WtExecuterMgr*>{ &_exe_mgr })
{
}

thread_local WtExecRunner::RuntimeOperation* WtExecRunner::RuntimeOperation::_current = NULL;

WtExecRunner::RuntimeOperation::RuntimeOperation(WtExecRunner* runner, Kind kind)
	: _runner(runner)
	, _previous(NULL)
	, _kind(kind)
	, _entered(false)
	, _counted(false)
	, _nested(false)
{
	if (_runner == NULL)
		return;
	for (RuntimeOperation* frame = _current; frame != NULL; frame = frame->_previous)
	{
		if (frame->_runner == _runner)
		{
			_nested = true;
			break;
		}
	}

	{
		std::lock_guard<std::mutex> lock(_runner->_lifecycle_mutex);
		const bool accepting = _kind == RuntimeCall ?
			_runner->_accepting_runtime_calls : _runner->_accepting_context_calls;
		if (!accepting && !_nested)
		{
			// Even a rejected callback briefly touches the runner's gate. Count it
			// so a control thread can drain calls that arrived during provider release.
			if (_kind == RuntimeCall)
				++_runner->_active_runtime_calls;
			else
				++_runner->_active_context_calls;
			_counted = true;
			return;
		}
		if (_kind == RuntimeCall)
			++_runner->_active_runtime_calls;
		else
			++_runner->_active_context_calls;
		_counted = true;
	}

	_previous = _current;
	_current = this;
	_entered = true;
}

WtExecRunner::RuntimeOperation::~RuntimeOperation()
{
	if (!_counted)
		return;
	if (_entered)
	{
		if (_current != this)
			std::terminate();
		_current = _previous;
	}
	{
		std::lock_guard<std::mutex> lock(_runner->_lifecycle_mutex);
		std::size_t& active = _kind == RuntimeCall ?
			_runner->_active_runtime_calls : _runner->_active_context_calls;
		if (active == 0)
			std::terminate();
		--active;
	}
	_runner->_lifecycle_condition.notify_all();
}

bool WtExecRunner::RuntimeOperation::isCurrentThreadIn(const WtExecRunner* runner)
{
	for (RuntimeOperation* frame = _current; frame != NULL; frame = frame->_previous)
		if (frame->_runner == runner)
			return true;
	return false;
}

bool WtExecRunner::closeRuntimeAdmissionAndDrain()
{
	std::unique_lock<std::mutex> lock(_lifecycle_mutex);
	_accepting_runtime_calls = false;
	_lifecycle_condition.notify_all();
	if (RuntimeOperation::isCurrentThreadIn(this))
		return false;
	_lifecycle_condition.wait(lock, [this]() {
		return _active_runtime_calls == 0 && !_factory_operation_active;
	});
	return true;
}

bool WtExecRunner::closeContextAdmissionAndDrain()
{
	std::unique_lock<std::mutex> lock(_lifecycle_mutex);
	_accepting_context_calls = false;
	_lifecycle_condition.notify_all();
	if (RuntimeOperation::isCurrentThreadIn(this))
		return false;
	_lifecycle_condition.wait(lock, [this]() { return _active_context_calls == 0; });
	return true;
}

bool WtExecRunner::beginConfiguration()
{
	std::unique_lock<std::mutex> lock(_lifecycle_mutex);
	if (!_accepting_runtime_calls || _configuration_attempted || _run_started)
		return false;
	while (_factory_operation_active && _accepting_runtime_calls)
		_lifecycle_condition.wait(lock);
	if (!_accepting_runtime_calls || _configuration_attempted || _run_started)
		return false;
	_configuration_attempted = true;
	return true;
}

WtExecRunner::~WtExecRunner()
{
	if (!shutdownRuntime())
		std::terminate();
}
bool WtExecRunner::shutdownRuntime()
{
	if (_shutdown_complete.load())
		return true;
	bool expected = false;
	if (!_shutdown_in_progress.compare_exchange_strong(expected, true))
		return false;

	_config_failed = true;
	if (!closeRuntimeAdmissionAndDrain())
	{
		_shutdown_in_progress = false;
		return false;
	}
	_traders.requestStopSinkNotifications();
	if (_traders.isInSinkCallback())
	{
		_shutdown_in_progress = false;
		return false;
	}

	if (!_traders.waitSinkNotificationsStopped())
	{
		_shutdown_in_progress = false;
		return false;
	}
	// A parser whose init failed may still own a registered SPI or provider API.
	// Keep the parser candidate, runner context, factory, and executor owners
	// alive; releasing it before the provider can prove callback drain is unsafe.
	if (!_parser_quarantine.empty())
	{
		WTSLogger::log_raw(LL_ERROR, "Failed parser initialization is quarantined; runner owners remain retained");
		_shutdown_in_progress = false;
		return false;
	}
	// The legacy IParserApi contract does not state that release() drains every
	// SPI callback. This call preserves existing shutdown behavior, but external
	// provider callback quiescence remains unverified until a versioned barrier exists.
	_parsers.release();
	if (!closeRuntimeAdmissionAndDrain())
	{
		_shutdown_in_progress = false;
		return false;
	}
	if (!_quiesce_coordinator.quiesce_all())
	{
		// Keep adapters and the execution factory alive while any executor or
		// plugin owner is still draining. The same coordinator must be retried
		// from a control thread so its manager-bound completion token is reused.
		_shutdown_in_progress = false;
		return false;
	}
	if (!closeContextAdmissionAndDrain())
	{
		_shutdown_in_progress = false;
		return false;
	}
	for (const auto& item : _traders.getAdapters())
		item.second->release();
	if (_config != NULL)
	{
		_config->release();
		_config = NULL;
	}
	_shutdown_complete = true;
	_shutdown_in_progress = false;
	return true;
}

bool WtExecRunner::init(const char* logCfg /* = "logcfgexec.json" */, bool isFile /* = true */)
{
#ifdef _MSC_VER
	CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str());
#endif

	if(isFile)
	{
		std::string path = WtHelper::getCWD() + logCfg;
		WTSLogger::init(path.c_str(), true);
	}
	else
	{
		WTSLogger::init(logCfg, false);
	}
	

	WtHelper::setInstDir(getBinDir());
	return true;
}

bool WtExecRunner::config(const char* cfgFile, bool isFile /* = true */)
{
	bool configured = false;
	{
		RuntimeOperation operation(this, RuntimeOperation::RuntimeCall);
		if (!operation.entered() || operation.isNested() || !beginConfiguration())
			return false;
		_config_failed = true;
		try
		{
			configured = [this, cfgFile, isFile]() -> bool {
				if (cfgFile == NULL)
					return false;
				_config = isFile ? WTSCfgLoader::load_from_file(cfgFile) :
					WTSCfgLoader::load_from_content(cfgFile, false);
				if (_config == NULL)
				{
					WTSLogger::log_raw(LL_ERROR, "Loading config file failed");
					return false;
				}

				WTSVariant* cfgBF = _config->get("basefiles");
				if (cfgBF == NULL)
					return false;
				if (cfgBF->get("session"))
				{
					_bd_mgr.loadSessions(cfgBF->getCString("session"));
					WTSLogger::info("Trading sessions loaded");
				}

				WTSVariant* cfgItem = cfgBF->get("commodity");
				if (cfgItem)
				{
					if (cfgItem->type() == WTSVariant::VT_String)
						_bd_mgr.loadCommodities(cfgItem->asCString());
					else if (cfgItem->type() == WTSVariant::VT_Array)
						for (uint32_t i = 0; i < cfgItem->size(); i++)
							_bd_mgr.loadCommodities(cfgItem->get(i)->asCString());
				}

				cfgItem = cfgBF->get("contract");
				if (cfgItem)
				{
					if (cfgItem->type() == WTSVariant::VT_String)
						_bd_mgr.loadContracts(cfgItem->asCString());
					else if (cfgItem->type() == WTSVariant::VT_Array)
						for (uint32_t i = 0; i < cfgItem->size(); i++)
							_bd_mgr.loadContracts(cfgItem->get(i)->asCString());
				}

				if (cfgBF->get("holiday"))
				{
					_bd_mgr.loadHolidays(cfgBF->getCString("holiday"));
					WTSLogger::info("Holidays loaded");
				}

				if (_config->get("data") != NULL && !initDataMgr())
					return false;
				if (!initActionPolicy())
					return false;

				const char* cfgParser = _config->getCString("parsers");
				if (cfgParser != NULL && cfgParser[0] != '\0')
				{
					if (!StdFile::exists(cfgParser))
					{
						WTSLogger::error("Parser config {} does not exist", cfgParser);
						return false;
					}
					WTSLogger::info("Reading parser config from {}...", cfgParser);
					WTSVariant* var = WTSCfgLoader::load_from_file(cfgParser);
					if (var == NULL)
					{
						WTSLogger::error("Loading parser config {} failed", cfgParser);
						return false;
					}
					ScopedVariantRelease releaseParserConfig(var);
					const bool parsersLoaded = initParsers(var);
					if (!parsersLoaded)
					{
						WTSLogger::error("Loading parsers failed");
						return false;
					}
				}

				WTSVariant* cfgTraders = _config->get("traders");
				if (cfgTraders)
				{
					if (cfgTraders->type() != WTSVariant::VT_String)
					{
						WTSLogger::error("Trader configuration must be a file path");
						return false;
					}
					const char* traderFile = cfgTraders->asCString();
					if (traderFile == NULL || traderFile[0] == '\0' || !StdFile::exists(traderFile))
					{
						WTSLogger::error("Trader configuration {} not exists", traderFile ? traderFile : "");
						return false;
					}
					WTSLogger::info("Reading trader config from {}...", traderFile);
					WTSVariant* var = WTSCfgLoader::load_from_file(traderFile);
					if (var == NULL)
					{
						WTSLogger::error("Loading trader config {} failed", traderFile);
						return false;
					}
					ScopedVariantRelease releaseTraderConfig(var);
					const bool tradersLoaded = initTraders(var);
					if (!tradersLoaded)
					{
						WTSLogger::error("Loading traders failed");
						return false;
					}
				}

				const char* cfgExecuters = _config->getCString("executers");
				if (cfgExecuters != NULL && cfgExecuters[0] != '\0')
				{
					if (!StdFile::exists(cfgExecuters))
					{
						WTSLogger::error("Executer config {} does not exist", cfgExecuters);
						return false;
					}
					WTSLogger::info("Reading executer config from {}...", cfgExecuters);
					WTSVariant* var = WTSCfgLoader::load_from_file(cfgExecuters);
					if (var == NULL)
					{
						WTSLogger::error("Loading executer config {} failed", cfgExecuters);
						return false;
					}
					ScopedVariantRelease releaseExecuterConfig(var);
					const bool executersLoaded = initExecuters(var);
					if (!executersLoaded)
					{
						WTSLogger::error("Loading executers failed");
						return false;
					}
				}

				return true;
			}();
		}
		catch (...)
		{
			WTSLogger::log_raw(LL_ERROR, "Runner configuration failed with an exception");
			configured = false;
		}
		if (configured)
			_config_failed = false;
	}

	if (!configured)
	{
		_config_failed = true;
		if (!shutdownRuntime())
			WTSLogger::log_raw(LL_WARN, "Runner configuration cleanup is pending on a control thread");
	}
	return configured;
}


void WtExecRunner::run()
{
	RuntimeOperation operation(this, RuntimeOperation::RuntimeCall);
	if (!operation.entered() || operation.isNested())
		return;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (!_configuration_attempted || _config_failed || _run_started || !_accepting_runtime_calls)
		{
			WTSLogger::error("Runner is not configured or has already been run; refusing to run");
			return;
		}
		_run_started = true;
	}

	try
	{
		_parsers.run();
		_traders.run();
	}
	catch (...)
	{
		print_stack_trace([](const char* message) {
			WTSLogger::error(message);
		});
	}
}

bool WtExecRunner::initParsers(WTSVariant* cfgParser)
{
	if (cfgParser == NULL)
		return false;
	WTSVariant* cfg = cfgParser->get("parsers");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (cfgItem == NULL)
			return false;
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		std::string realid = id ? id : "";
		if (realid.empty())
		{
			static uint32_t auto_parserid = 1000;
			realid = fmtutil::format<64>("auto_parser_{}", auto_parserid++);
		}

		ParserAdapterPtr adapter(new ParserAdapter);
		// Keep the candidate before entering provider code. A failed init can
		// still have registered the SPI or retained the API/module.
		_parser_quarantine.push_back(adapter);
		if (!adapter->init(realid.c_str(), cfgItem, this, &_bd_mgr, &_hot_mgr))
		{
			if (!adapter->hasApi())
			{
				// The adapter failed before entering provider creation, so it owns
				// no API that can call back into this runner. Let normal cleanup
				// proceed instead of treating every parser init failure as unsafe.
				_parser_quarantine.pop_back();
				WTSLogger::error("Initializing parser {} failed before API creation", realid);
			}
			else
			{
				WTSLogger::error("Initializing parser {} failed; retaining its uncertain provider owner", realid);
			}
			return false;
		}
		if (!_parsers.addAdapter(realid.c_str(), adapter))
		{
			WTSLogger::error("Registering parser {} failed; retaining its owner", realid);
			return false;
		}
		_parser_quarantine.pop_back();
		count++;
	}

	WTSLogger::info("{} parsers loaded", count);
	return true;
}

bool WtExecRunner::initExecuters(WTSVariant* cfgExecuter)
{
	if (cfgExecuter == NULL)
		return false;
	WTSVariant* cfg = cfgExecuter->get("executers");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	std::string path = WtHelper::getInstDir() + "executer//";
	_exe_factory.loadFactories(path.c_str());

	std::vector<PendingTraderBinding> pendingTraderBindings;
	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (cfgItem == NULL)
			return false;
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		if (id == NULL || id[0] == '\0' || _exe_mgr.find_executer(id) != NULL)
		{
			WTSLogger::error("Invalid or duplicate executer id {}", id ? id : "");
			return false;
		}
		std::string name = cfgItem->getCString("name");
		if (name.empty())
			name = "local";

		if (name == "local")
		{
			ExecCmdPtr owner(new WtLocalExecuter(&_exe_factory, id, &_data_mgr));
			WtLocalExecuter* executer = static_cast<WtLocalExecuter*>(owner.get());
			executer->setStub(this);
			_exe_mgr.add_executer(owner);
			if (_exe_mgr.find_executer(id) != owner)
				return false;
			if (!executer->init(cfgItem))
			{
				WTSLogger::error("Initializing local executer {} failed; retaining its owner", id);
				return false;
			}

			const char* tid = cfgItem->getCString("trader");
			if (tid == NULL || tid[0] == '\0')
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					pendingTraderBindings.push_back({ trader, owner, executer });
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
				}
			}
		}
		else if (name == "diff")
		{
			ExecCmdPtr owner(new WtDiffExecuter(&_exe_factory, id, &_data_mgr, &_bd_mgr));
			WtDiffExecuter* executer = static_cast<WtDiffExecuter*>(owner.get());
			executer->setStub(this);
			_exe_mgr.add_executer(owner);
			if (_exe_mgr.find_executer(id) != owner)
				return false;
			if (!executer->init(cfgItem))
			{
				WTSLogger::error("Initializing diff executer {} failed; retaining its owner", id);
				return false;
			}

			const char* tid = cfgItem->getCString("trader");
			if (tid == NULL || tid[0] == '\0')
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					pendingTraderBindings.push_back({ trader, owner, executer });
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
				}
			}
		}
		else
		{
			ExecCmdPtr owner(new WtDistExecuter(id));
			WtDistExecuter* executer = static_cast<WtDistExecuter*>(owner.get());
			executer->setStub(this);
			_exe_mgr.add_executer(owner);
			if (_exe_mgr.find_executer(id) != owner)
				return false;
			if (!executer->init(cfgItem))
			{
				WTSLogger::error("Initializing dist executer {} failed; retaining its owner", id);
				return false;
			}
		}
		count++;
	}

	for (const PendingTraderBinding& binding : pendingTraderBindings)
		binding.trader->addSink(binding.sink);

	WTSLogger::info("{} executers loaded", count);
	return true;
}

bool WtExecRunner::initTraders(WTSVariant* cfgTrader)
{
	WTSVariant* cfg = cfgTrader->get("traders");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		TraderAdapterPtr adapter(new TraderAdapter);
		if (!adapter->init(id, cfgItem, &_bd_mgr, &_act_policy))
		{
			WTSLogger::error("Initializing trader {} failed", id);
			return false;
		}

		if (!_traders.addAdapter(id, adapter))
		{
			WTSLogger::error("Registering trader {} failed", id);
			return false;
		}
		count++;
	}

	WTSLogger::info("{} traders loaded", count);

	return true;
}

bool WtExecRunner::initDataMgr()
{
	WTSVariant* cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	if (!_data_mgr.init(cfg, this))
	{
		WTSLogger::error("Data Manager initialization failed");
		return false;
	}

	WTSLogger::info("Data Manager initialized");
	return true;
}

bool WtExecRunner::addExeFactories(const char* folder)
{
	RuntimeOperation operation(this, RuntimeOperation::RuntimeCall);
	if (!operation.entered() || operation.isNested())
		return false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (!_accepting_runtime_calls || _configuration_attempted || _factory_operation_active)
			return false;
		_factory_operation_active = true;
	}

	bool loaded = false;
	try
	{
		loaded = _exe_factory.loadFactories(folder);
	}
	catch (...)
	{
		loaded = false;
	}
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_factory_operation_active = false;
	}
	_lifecycle_condition.notify_all();
	return loaded;
}

WTSSessionInfo* WtExecRunner::get_session_info(const char* sid, bool isCode /* = true */)
{
	RuntimeOperation operation(this, RuntimeOperation::ContextCall);
	if (!operation.entered())
		return NULL;
	if (!isCode)
		return _bd_mgr.getSession(sid);

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, NULL);
	WTSCommodityInfo* cInfo = _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

void WtExecRunner::handle_push_quote(WTSTickData* quote)
{
	RuntimeOperation operation(this, RuntimeOperation::RuntimeCall);
	if (!operation.entered())
		return;
	if (quote == NULL)
		return;
	if (_config_failed)
	{
		WTSLogger::error("Runner is not configured; ignoring quote");
		return;
	}

	uint32_t uDate = quote->actiondate();
	uint32_t uTime = quote->actiontime();
	uint32_t curMin = uTime / 100000;
	uint32_t curSec = uTime % 100000;
	WtHelper::setTime(uDate, curMin, curSec);
	WtHelper::setTDate(quote->tradingdate());

	_data_mgr.handle_push_quote(quote->code(), quote);

	_exe_mgr.handle_tick(quote->code(), quote);
}

void WtExecRunner::release()
{
	if (!shutdownRuntime())
	{
		WTSLogger::log_raw(LL_WARN, "Runner shutdown is deferred; retry release from a control thread");
		return;
	}
	WTSLogger::stop();
}


void WtExecRunner::setPosition(const char* stdCode, double targetPos)
{
	RuntimeOperation operation(this, RuntimeOperation::RuntimeCall);
	if (!operation.entered() || stdCode == NULL)
		return;
	if (_config_failed.load())
		return;
	std::lock_guard<std::mutex> lock(_positions_mutex);
	_positions[stdCode] = targetPos;
}

void WtExecRunner::commitPositions()
{
	RuntimeOperation operation(this, RuntimeOperation::RuntimeCall);
	if (!operation.entered())
		return;
	if (_config_failed.load())
		return;
	wt_hashmap<std::string, double> positions;
	{
		std::lock_guard<std::mutex> lock(_positions_mutex);
		positions.swap(_positions);
	}
	_exe_mgr.set_positions(positions);
}

bool WtExecRunner::initActionPolicy()
{
	const char* action_file = _config->getCString("bspolicy");
	if (strlen(action_file) <= 0)
		return false;

	bool ret = _act_policy.init(action_file);
	WTSLogger::info("Action policies initialized");
	return ret;
}

uint64_t WtExecRunner::get_real_time()
{
	RuntimeOperation operation(this, RuntimeOperation::ContextCall);
	if (!operation.entered())
		return 0;
	return TimeUtils::makeTime(_data_mgr.get_date(), _data_mgr.get_raw_time() * 100000 + _data_mgr.get_secs());
}

WTSCommodityInfo* WtExecRunner::get_comm_info(const char* stdCode)
{
	RuntimeOperation operation(this, RuntimeOperation::ContextCall);
	if (!operation.entered() || stdCode == NULL)
		return NULL;
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	return _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
}

WTSSessionInfo* WtExecRunner::get_sess_info(const char* stdCode)
{
	RuntimeOperation operation(this, RuntimeOperation::ContextCall);
	if (!operation.entered() || stdCode == NULL)
		return NULL;
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	WTSCommodityInfo* cInfo = _bd_mgr.getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

IHotMgr* WtExecRunner::get_hot_mon()
{
	RuntimeOperation operation(this, RuntimeOperation::ContextCall);
	return operation.entered() ? &_hot_mgr : NULL;
}

uint32_t WtExecRunner::get_trading_day()
{
	RuntimeOperation operation(this, RuntimeOperation::ContextCall);
	if (!operation.entered())
		return 0;
	return _data_mgr.get_trading_day();
}
