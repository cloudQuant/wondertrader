/*!
 * \file WtRtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtRtRunner.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>
#include "ExpCtaContext.h"
#include "ExpSelContext.h"
#include "ExpHftContext.h"

#include "ExpParser.h"
#include "ExpExecuter.h"

#include "../WtCore/WtHelper.h"
#include "../WtCore/CtaStraContext.h"
#include "../WtCore/HftStraContext.h"
#include "../WtCore/SelStraContext.h"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSUtils/SignalHook.hpp"

#include "../Share/TimeUtils.hpp"
#include "../Share/ModuleHelper.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"

namespace
{
	template <typename Function>
	class ScopeExit
	{
	public:
		explicit ScopeExit(Function function) : _function(std::move(function)) {}
		~ScopeExit() noexcept { _function(); }
		ScopeExit(const ScopeExit&) = delete;
		ScopeExit& operator=(const ScopeExit&) = delete;

	private:
		Function _function;
	};

	struct RunnerScopeNode
	{
		const WtRtRunner* runner;
		RunnerScopeNode* previous;
	};

	struct RunnerFactoryParticipant
	{
		const void* identity;
		IExecFactoryQuiesceLifecycle* lifecycle;
	};

	bool preflight_runner_exec_quiesce(const std::vector<WtExecuterMgr*>& managers)
	{
		std::vector<ExecCmdPtr> hosts;
		bool complete = true;
		for (WtExecuterMgr* manager : managers)
		{
			if (manager == NULL)
			{
				complete = false;
				continue;
			}
			try
			{
				manager->enum_executer([&hosts](ExecCmdPtr executer) {
					hosts.push_back(executer);
				});
			}
			catch (...)
			{
				complete = false;
			}
		}

		// Close every host ingress before probing any participant. This stage is
		// non-blocking; the coordinator repeats the idempotent requests and owns
		// the later drain, factory-unit gate, and plugin waits.
		for (const ExecCmdPtr& host : hosts)
		{
			IExecCommandQuiesce* quiesce = host == NULL ? NULL :
				dynamic_cast<IExecCommandQuiesce*>(host.get());
			IExecCommandQuiesceStages* stages = host == NULL ? NULL :
				dynamic_cast<IExecCommandQuiesceStages*>(host.get());
			if (quiesce == NULL || stages == NULL)
			{
				complete = false;
				continue;
			}
			try { quiesce->requestQuiesce(); }
			catch (...) { complete = false; }
		}

		std::vector<RunnerFactoryParticipant> factories;
		for (const ExecCmdPtr& host : hosts)
		{
			IExecCommandQuiesceStages* stages = host == NULL ? NULL :
				dynamic_cast<IExecCommandQuiesceStages*>(host.get());
			if (stages == NULL)
				continue;
			WtExecuterFactory* factory = NULL;
			try { factory = stages->quiesceFactory(); }
			catch (...) { complete = false; continue; }
			if (factory == NULL)
				continue;
			IExecFactoryQuiesceLifecycle* lifecycle =
				dynamic_cast<IExecFactoryQuiesceLifecycle*>(factory);
			if (lifecycle == NULL)
			{
				complete = false;
				continue;
			}
			const void* identity = lifecycle->quiesceFactoryIdentity();
			if (identity == NULL)
			{
				complete = false;
				continue;
			}
			auto existing = std::find_if(factories.begin(), factories.end(),
				[identity](const RunnerFactoryParticipant& participant) {
					return participant.identity == identity;
				});
			if (existing == factories.end())
			{
				factories.push_back({ identity, lifecycle });
			}
			else if (existing->lifecycle != lifecycle)
			{
				complete = false;
			}
		}

		for (const RunnerFactoryParticipant& factory : factories)
			factory.lifecycle->closeFactoryConfigAdmissions();

		// Probe all hosts and factories before release() reaches any blocking
		// producer shutdown. A callback-stack caller stays pending for control-
		// thread retry without self-joining its parser or executor worker.
		for (const ExecCmdPtr& host : hosts)
		{
			IExecCommandQuiesceStages* stages = host == NULL ? NULL :
				dynamic_cast<IExecCommandQuiesceStages*>(host.get());
			if (stages == NULL)
			{
				complete = false;
				continue;
			}
			try
			{
				if (!stages->preflightQuiesceOnCurrentThread())
					complete = false;
			}
			catch (...)
			{
				complete = false;
			}
		}
		for (const RunnerFactoryParticipant& factory : factories)
		{
			try
			{
				if (!factory.lifecycle->preflightQuiesceFactoriesOnCurrentThread())
					complete = false;
			}
			catch (...)
			{
				complete = false;
			}
		}
		return complete;
	}

	thread_local RunnerScopeNode* g_exec_status_call_scope = NULL;
	thread_local RunnerScopeNode* g_runner_startup_scope = NULL;
	thread_local RunnerScopeNode* g_runner_teardown_scope = NULL;
	thread_local RunnerScopeNode* g_runner_callback_scope = NULL;
	volatile std::sig_atomic_t g_runner_signal_run_active = 0;
	volatile std::sig_atomic_t g_runner_signal_exit_requested = 0;
	volatile std::sig_atomic_t g_runner_signal_log_requested = 0;

	bool is_synchronous_fatal_signal(int signum)
	{
		if (signum == SIGILL || signum == SIGFPE || signum == SIGSEGV || signum == SIGABRT)
			return true;
#if defined(SIGABRT_COMPAT) && SIGABRT_COMPAT != SIGABRT
		if (signum == SIGABRT_COMPAT)
			return true;
#endif
#ifdef SIGBUS
		if (signum == SIGBUS)
			return true;
#endif
#ifdef SIGTRAP
		if (signum == SIGTRAP)
			return true;
#endif
#ifdef SIGSYS
		if (signum == SIGSYS)
			return true;
#endif
		return false;
	}

	void runner_signal_log_callback(const char*)
	{
		g_runner_signal_log_requested = 1;
	}

	// SignalHook's ExitHandler signature is void(int); the argument is signum.
	void runner_signal_exit_callback(int signum)
	{
		if (is_synchronous_fatal_signal(signum))
		{
			// Returning from a synchronous fault can re-execute the same faulting
			// instruction indefinitely; fail fast instead of deferring it to run().
			std::_Exit(signum);
		}
		if (g_runner_signal_run_active != 0)
		{
			g_runner_signal_exit_requested = signum != 0 ? signum : 1;
			return;
		}

		// SignalHook has no unregister/restore token. Preserve termination once
		// no synchronous runner loop remains to consume the signal request.
		std::_Exit(signum);
	}

	bool is_in_runner_scope(RunnerScopeNode* scope, const WtRtRunner* runner)
	{
		for (RunnerScopeNode* node = scope; node != NULL; node = node->previous)
		{
			if (node->runner == runner)
				return true;
		}
		return false;
	}

	class ExecStatusCallScope
	{
	public:
		explicit ExecStatusCallScope(const WtRtRunner* runner)
			: _node{ runner, g_exec_status_call_scope }
		{
			g_exec_status_call_scope = &_node;
		}
		~ExecStatusCallScope() { g_exec_status_call_scope = _node.previous; }

	private:
		RunnerScopeNode _node;
	};

	class RunnerStartupScope
	{
	public:
		explicit RunnerStartupScope(const WtRtRunner* runner)
			: _node{ runner, g_runner_startup_scope }
		{
			g_runner_startup_scope = &_node;
		}
		~RunnerStartupScope() { g_runner_startup_scope = _node.previous; }

	private:
		RunnerScopeNode _node;
	};

	class RunnerTeardownScope
	{
	public:
		explicit RunnerTeardownScope(const WtRtRunner* runner)
			: _node{ runner, g_runner_teardown_scope }
		{
			g_runner_teardown_scope = &_node;
		}
		~RunnerTeardownScope() { g_runner_teardown_scope = _node.previous; }

	private:
		RunnerScopeNode _node;
	};

	class RunnerCallbackScope
	{
	public:
		explicit RunnerCallbackScope(const WtRtRunner* runner)
			: _node{ runner, g_runner_callback_scope }
		{
			g_runner_callback_scope = &_node;
		}
		~RunnerCallbackScope() { g_runner_callback_scope = _node.previous; }

	private:
		RunnerScopeNode _node;
	};
}

#ifdef _MSC_VER
#include "../Common/mdump.h"

#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;


 //这个主要是给MiniDumper用的
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

WtRtRunner::WtRtRunner()
	: _cb_cta_init(NULL)
	, _cb_cta_sessevt(NULL)

	, _cb_cta_tick(NULL)
	, _cb_cta_calc(NULL)
	, _cb_cta_bar(NULL)
	, _cb_cta_cond_trigger(NULL)
	, _cb_sel_init(NULL)
	, _cb_sel_sessevt(NULL)

	, _cb_sel_tick(NULL)
	, _cb_sel_calc(NULL)
	, _cb_sel_bar(NULL)
	, _cb_hft_init(NULL)
	, _cb_hft_sessevt(NULL)

	, _cb_hft_tick(NULL)
	, _cb_hft_bar(NULL)
	, _cb_hft_chnl(NULL)

	, _cb_hft_ord(NULL)
	, _cb_hft_trd(NULL)
	, _cb_hft_entrust(NULL)
	, _cb_hft_position(NULL)
	, _cb_hft_ordque(NULL)
	, _cb_hft_orddtl(NULL)
	, _cb_hft_trans(NULL)
	, _cb_evt(NULL)
	, _cb_parser_evt(NULL)
	, _cb_parser_sub(NULL)

	, _cb_exec_cmd(NULL)
	, _cb_exec_init(NULL)

	, _data_store(NULL)
	, _is_hft(false)
	, _is_sel(false)
	, _config_failed(true)

	, _to_exit(false)

	, _ext_fnl_bar_loader(NULL)
	, _ext_raw_bar_loader(NULL)
	, _ext_adj_fct_loader(NULL)
	, _feed_obj(NULL)
	, _feeder_bars(NULL)
	, _feeder_fcts(NULL)
{
	_exec_quiesce_coordinator.reset(new WtExecQuiesceCoordinator({
		&_cta_engine.getExecuterMgr(),
		&_sel_engine.getExecuterMgr()
	}));
}


WtRtRunner::~WtRtRunner()
{
	const bool hasResources = _lifecycle_state != LifecycleState::UNCONFIGURED ||
		_engine != NULL || !_traders.getAdapters().empty() || !_parsers._adapters.empty() ||
		!_quarantined_parser_owners.empty();
	if (!hasResources)
		return;

	release();
	std::lock_guard<std::mutex> lock(_status_config_mutex);
	if (_lifecycle_state != LifecycleState::RELEASED)
		std::terminate(); // A runner cannot be destroyed while deferred sink teardown still owns its sinks.
}

bool WtRtRunner::init(const char* logCfg /* = "logcfg.prop" */, bool isFile /* = true */, const char* genDir)
{
#ifdef _MSC_VER
	CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str());
#endif

	if(isFile)
	{
		std::string path = WtHelper::getCWD() + logCfg;
		WTSLogger::init(path.c_str(), true, this);
	}
	else
	{
		WTSLogger::init(logCfg, false, this);
	}
	

	WtHelper::setInstDir(getBinDir());
	WtHelper::setGenerateDir(StrUtil::standardisePath(genDir).c_str());
	return true;
}

void WtRtRunner::registerEvtCallback(FuncEventCallback cbEvt)
{
	_cb_evt = cbEvt;

	_cta_engine.regEventListener(this);
	_hft_engine.regEventListener(this);
	_sel_engine.regEventListener(this);
}

void WtRtRunner::registerParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)
{
	_cb_parser_evt = cbEvt;
	_cb_parser_sub = cbSub;

	WTSLogger::info("Callbacks of Extented Parser registration done");
}

void WtRtRunner::registerExecuterPorter(FuncExecInitCallback cbInit, FuncExecCmdCallback cbExec)
{
	_cb_exec_init = cbInit;
	_cb_exec_cmd = cbExec;

	WTSLogger::info("Callbacks of Extented Executer registration done");
}

void WtRtRunner::registerCtaCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, 
		FuncSessionEvtCallback cbSessEvt, FuncStraCondTriggerCallback cbCondTrigger /* = NULL */)
{
	_cb_cta_init = cbInit;
	_cb_cta_tick = cbTick;
	_cb_cta_calc = cbCalc;
	_cb_cta_bar = cbBar;
	_cb_cta_sessevt = cbSessEvt;
	_cb_cta_cond_trigger = cbCondTrigger;

	WTSLogger::info("Callbacks of CTA engine registration done");
}

void WtRtRunner::registerSelCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt)
{
	_cb_sel_init = cbInit;
	_cb_sel_tick = cbTick;
	_cb_sel_calc = cbCalc;
	_cb_sel_bar = cbBar;

	_cb_sel_sessevt = cbSessEvt;

	WTSLogger::info("Callbacks of SEL engine registration done");
}

void WtRtRunner::registerHftCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar, 
	FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
	FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt, FuncHftPosCallback cbPosition)
{
	_cb_hft_init = cbInit;
	_cb_hft_tick = cbTick;
	_cb_hft_bar = cbBar;

	_cb_hft_chnl = cbChnl;
	_cb_hft_ord = cbOrd;
	_cb_hft_trd = cbTrd;
	_cb_hft_entrust = cbEntrust;

	_cb_hft_orddtl = cbOrdDtl;
	_cb_hft_ordque = cbOrdQue;
	_cb_hft_trans = cbTrans;

	_cb_hft_sessevt = cbSessEvt;

	_cb_hft_position = cbPosition;

	WTSLogger::info("Callbacks of HFT engine registration done");
}

bool WtRtRunner::loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_fnl_bar_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_bars = cb;

	switch (period)
	{
	case KP_DAY:
		return _ext_fnl_bar_loader(stdCode, "d1");
	case KP_Minute1:
		return _ext_fnl_bar_loader(stdCode, "m1");
	case KP_Minute5:
		return _ext_fnl_bar_loader(stdCode, "m5");
	default:
	{
		WTSLogger::error("Unsupported period of extended data loader");
		return false;
	}
	}
}

bool WtRtRunner::loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_raw_bar_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_bars = cb;

	switch (period)
	{
	case KP_DAY:
		return _ext_raw_bar_loader(stdCode, "d1");
	case KP_Minute1:
		return _ext_raw_bar_loader(stdCode, "m1");
	case KP_Minute5:
		return _ext_raw_bar_loader(stdCode, "m5");
	default:
	{
		WTSLogger::error("Unsupported period of extended data loader");
		return false;
	}
	}
}

bool WtRtRunner::loadAllAdjFactors(void* obj, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader("");
}

bool WtRtRunner::loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader(stdCode);
}

void WtRtRunner::feedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
{
	_feeder_fcts(_feed_obj, stdCode, dates, factors, count);
}


void WtRtRunner::feedRawBars(WTSBarStruct* bars, uint32_t count)
{
	if (_ext_fnl_bar_loader == NULL && _ext_raw_bar_loader == NULL)
	{
		WTSLogger::error("Cannot feed bars because no extended bar loader is registered.");
		return;
	}
	if (_feeder_bars == NULL)
	{
		WTSLogger::error("Cannot feed bars because no bar feeder is loaded.");
		return;
	}

	_feeder_bars(_feed_obj, bars, count);
}

void WtRtRunner::handle_push_quote(WTSTickData* curTick)
{
	RunnerCallbackScope callbackScope(this);
	if (_engine != NULL)
		_engine->handle_push_quote(curTick);
}

void WtRtRunner::handle_push_order_detail(WTSOrdDtlData* curOrdDtl)
{
	RunnerCallbackScope callbackScope(this);
	if (_engine != NULL)
		_engine->handle_push_order_detail(curOrdDtl);
}

void WtRtRunner::handle_push_order_queue(WTSOrdQueData* curOrdQue)
{
	RunnerCallbackScope callbackScope(this);
	if (_engine != NULL)
		_engine->handle_push_order_queue(curOrdQue);
}

void WtRtRunner::handle_push_transaction(WTSTransData* curTrans)
{
	RunnerCallbackScope callbackScope(this);
	if (_engine != NULL)
		_engine->handle_push_transaction(curTrans);
}


bool WtRtRunner::createExtParser(const char* id)
{
	if (id == NULL || id[0] == '\0')
		return false;
	std::string parserId;
	try { parserId.assign(id); }
	catch (...) { return false; }
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state != LifecycleState::CONFIGURED || _manager_release_in_progress)
			return false;
		++_active_ext_parser_creations;
	}
	ScopeExit creationFinished([this]() {
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			--_active_ext_parser_creations;
		}
		_lifecycle_changed.notify_all();
	});
	bool reservationActive = false;
	auto eraseReservation = [this, &parserId]() {
		auto reserved = std::find(_creating_ext_parser_ids.begin(), _creating_ext_parser_ids.end(), parserId);
		if (reserved != _creating_ext_parser_ids.end())
			_creating_ext_parser_ids.erase(reserved);
	};
	ScopeExit reservationFinished([this, &reservationActive, &eraseReservation]() {
		if (!reservationActive)
			return;
		std::lock_guard<std::mutex> parserRegistryLock(_parser_registry_mutex);
		eraseReservation();
		reservationActive = false;
	});

	try
	{
		std::lock_guard<std::mutex> parserRegistryLock(_parser_registry_mutex);
		const bool alreadyCreating = std::find(_creating_ext_parser_ids.begin(),
			_creating_ext_parser_ids.end(), parserId) != _creating_ext_parser_ids.end();
		if (_engine == NULL || alreadyCreating || _parsers.getAdapter(parserId.c_str()) != NULL ||
			std::any_of(_quarantined_parser_owners.begin(), _quarantined_parser_owners.end(),
				[](const ParserAdapterPtr& owner) { return owner != NULL && owner->hasApi(); }))
			return false;
		_creating_ext_parser_ids.emplace_back(parserId);
		reservationActive = true;
		// Each outstanding creation may need one quarantine slot if init or
		// publication fails. Reserve before any candidate acquires a plugin API.
		_quarantined_parser_owners.reserve(
			_quarantined_parser_owners.size() + _creating_ext_parser_ids.size());
	}
	catch (...)
	{
		WTSLogger::error("Extended parser {} reservation failed before API initialization", parserId);
		return false;
	}

	ParserAdapterPtr adapter;
	try { adapter.reset(new ParserAdapter); }
	catch (...) { return false; }
	auto quarantineCandidate = [this, &adapter, &reservationActive, &eraseReservation]() noexcept {
		try
		{
			std::lock_guard<std::mutex> parserRegistryLock(_parser_registry_mutex);
			_quarantined_parser_owners.emplace_back(adapter);
			eraseReservation();
			reservationActive = false;
		}
		catch (...)
		{
			// Losing an initialized parser API would invalidate its borrowed SPI
			// graph. Capacity was reserved before init; any failure here is fatal.
			std::terminate();
		}
	};

	ExpParser* parser = NULL;
	try { parser = new ExpParser(parserId.c_str()); }
	catch (...) { return false; }
	bool initialized = false;
	try
	{
		// ExpParser::init synchronously invokes user parser callbacks. The id is
		// reserved above, but the registry mutex must not cross that callback.
		initialized = adapter->initExt(parserId.c_str(), parser, this,
			_engine->get_basedata_mgr(), _engine->get_hot_mgr());
	}
	catch (...)
	{
		quarantineCandidate();
		WTSLogger::error("Extended parser {} initialization threw", parserId);
		return false;
	}
	if (!initialized)
	{
		// initExt may already have transferred the API to the adapter. Keep the
		// failed candidate separate from the runnable parser manager.
		quarantineCandidate();
		return false;
	}

	bool registered = false;
	bool publishAllowed = false;
	bool publicationThrew = false;
	{
		try
		{
			// Lock order matches quote admission: lifecycle first, registry second.
			// This makes STOPPING and publication mutually exclusive.
			std::unique_lock<std::mutex> configLock(_status_config_mutex);
			std::lock_guard<std::mutex> parserRegistryLock(_parser_registry_mutex);
			publishAllowed = _lifecycle_state == LifecycleState::CONFIGURED && !_manager_release_in_progress;
			_quarantined_parser_owners.emplace_back(adapter);
			if (publishAllowed)
			{
				try
				{
					registered = _parsers.addAdapter(parserId.c_str(), adapter);
				}
				catch (...)
				{
					publicationThrew = true;
					// The candidate is already quarantined. Remove a partially
					// inserted manager entry if the container threw after mutation.
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
			}
			if (registered)
			{
				auto quarantined = std::find_if(_quarantined_parser_owners.begin(),
					_quarantined_parser_owners.end(), [&adapter](const ParserAdapterPtr& owner) {
						return owner == adapter;
					});
				if (quarantined == _quarantined_parser_owners.end())
					std::terminate();
				_quarantined_parser_owners.erase(quarantined);
			}
			eraseReservation();
			reservationActive = false;
		}
		catch (...)
		{
			// The initialized candidate is already in quarantine; if the publish
			// transaction cannot finish, continuing could lose its borrowed graph.
			std::terminate();
		}
	}
	if (!publishAllowed)
	{
		WTSLogger::warn("Extended parser {} initialization completed after runner teardown began; candidate quarantined", parserId);
		return false;
	}
	if (!registered)
	{
		if (publicationThrew)
			WTSLogger::error("Extended parser {} registration threw; candidate quarantined", parserId);
		return false;
	}
	try { WTSLogger::info("Extended parser created"); }
	catch (...) { }
	return true;
}

bool WtRtRunner::createExtExecuter(const char* id)
{
	ExpExecuter* executer = new ExpExecuter(id);
	executer->init();
	_cta_engine.addExecuter(ExecCmdPtr(executer));
	WTSLogger::info("Extended Executer created");
	return true;
}

uint32_t WtRtRunner::createCtaContext(const char* name, int32_t slippage /* = 0 */)
{
	ExpCtaContext* ctx = new ExpCtaContext(&_cta_engine, name, slippage);
	_cta_engine.addContext(CtaContextPtr(ctx));
	return ctx->id();
}

uint32_t WtRtRunner::createHftContext(const char* name, const char* trader, bool bAgent, int32_t slippage /* = 0 */)
{
	ExpHftContext* ctx = new ExpHftContext(&_hft_engine, name, bAgent, slippage);
	_hft_engine.addContext(HftContextPtr(ctx));
	TraderAdapterPtr trdPtr = _traders.getAdapter(trader);
	if(trdPtr)
	{
		ctx->setTrader(trdPtr.get());
		trdPtr->addSink(ctx);
	}
	else
	{
		WTSLogger::error("Trader {} not exists, Binding trader to HFT strategy failed", trader);
	}
	return ctx->id();
}

uint32_t WtRtRunner::createSelContext(const char* name, uint32_t date, uint32_t time, const char* period, int32_t slippage, const char* trdtpl /* = "CHINA" */, const char* session/* ="TRADING" */)
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

	ExpSelContext* ctx = new ExpSelContext(&_sel_engine, name, slippage);

	_sel_engine.addContext(SelContextPtr(ctx), date, time, ptype, true, trdtpl, session);

	return ctx->id();
}

const char* WtRtRunner::get_raw_stdcode(const char* stdCode)
{
	static thread_local std::string s;
	s = _engine->get_rawcode(stdCode);
	return s.c_str();
}


CtaContextPtr WtRtRunner::getCtaContext(uint32_t id)
{
	return _cta_engine.getContext(id);
}

HftContextPtr WtRtRunner::getHftContext(uint32_t id)
{
	return _hft_engine.getContext(id);
}

SelContextPtr WtRtRunner::getSelContext(uint32_t id)
{
	return _sel_engine.getContext(id);
}

void WtRtRunner::on_initialize_event()
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_evt)
		_cb_evt(EVENT_ENGINE_INIT, 0, 0);
}

void WtRtRunner::on_schedule_event(uint32_t uDate, uint32_t uTime)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_evt)
		_cb_evt(EVENT_ENGINE_SCHDL, uDate, uTime);
}

void WtRtRunner::on_session_event(uint32_t uDate, bool isBegin)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_evt)
		_cb_evt(isBegin ? EVENT_SESSION_BEGIN : EVENT_SESSION_END, uDate, 0);
}

void WtRtRunner::ctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType /* = ET_CTA */)
{
	RunnerCallbackScope callbackScope(this);
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_bar) _cb_cta_bar(id, stdCode, period, newBar); break;
	case ET_HFT: if (_cb_hft_bar) _cb_hft_bar(id, stdCode, period, newBar); break;
	case ET_SEL: if (_cb_sel_bar) _cb_sel_bar(id, stdCode, period, newBar); break;
	default:
		break;
	}
}

void WtRtRunner::ctx_on_calc(uint32_t id, uint32_t curDate, uint32_t curTime, EngineType eType /* = ET_CTA */)
{
	RunnerCallbackScope callbackScope(this);
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_calc) _cb_cta_calc(id, curDate, curTime); break;
	case ET_SEL: if (_cb_sel_calc) _cb_sel_calc(id, curDate, curTime); break;
	default:
		break;
	}
}

void WtRtRunner::ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType /* = ET_CTA */)
{
	RunnerCallbackScope callbackScope(this);
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_cond_trigger) _cb_cta_cond_trigger(id, stdCode, target, price, usertag); break;
	default:
		break;
	}
}

void WtRtRunner::ctx_on_init(uint32_t id, EngineType eType/* = ET_CTA*/)
{
	RunnerCallbackScope callbackScope(this);
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_init) _cb_cta_init(id); break;
	case ET_HFT: if (_cb_hft_init) _cb_hft_init(id); break;
	case ET_SEL: if (_cb_sel_init) _cb_sel_init(id); break;
	default:
		break;
	}
}

void WtRtRunner::ctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin /* = true */, EngineType eType /* = ET_CTA */)
{
	RunnerCallbackScope callbackScope(this);
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_sessevt) _cb_cta_sessevt(id, curTDate, isBegin); break;
	case ET_HFT: if (_cb_hft_sessevt) _cb_hft_sessevt(id, curTDate, isBegin); break;
	case ET_SEL: if (_cb_sel_sessevt) _cb_sel_sessevt(id, curTDate, isBegin); break;
	default:
		break;
	}
}

void WtRtRunner::ctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType /* = ET_CTA */)
{
	RunnerCallbackScope callbackScope(this);
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_tick) _cb_cta_tick(id, stdCode, &newTick->getTickStruct()); break;
	case ET_HFT: if (_cb_hft_tick) _cb_hft_tick(id, stdCode, &newTick->getTickStruct()); break;
	case ET_SEL: if (_cb_sel_tick) _cb_sel_tick(id, stdCode, &newTick->getTickStruct()); break;
	default:
		break;
	}
}

void WtRtRunner::hft_on_channel_lost(uint32_t cHandle, const char* trader)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_chnl)
		_cb_hft_chnl(cHandle, trader, CHNL_EVENT_LOST);
}

void WtRtRunner::hft_on_channel_ready(uint32_t cHandle, const char* trader)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_chnl)
		_cb_hft_chnl(cHandle, trader, CHNL_EVENT_READY);
}

void WtRtRunner::hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_entrust)
		_cb_hft_entrust(cHandle, localid, stdCode, bSuccess, message, userTag);
}

void WtRtRunner::hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_ord)
		_cb_hft_ord(cHandle, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);
}

void WtRtRunner::hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_trd)
		_cb_hft_trd(cHandle, localid, stdCode, isBuy, vol, price, userTag);
}

void WtRtRunner::hft_on_position(uint32_t cHandle, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_position)
		_cb_hft_position(cHandle, stdCode, isLong, prevol, preavail, newvol, newavail);
}

bool WtRtRunner::config(const char* cfgFile, bool isFile /* = true */)
{
	if (is_in_runner_scope(g_exec_status_call_scope, this))
	{
		WTSLogger::error("Runner configuration from execution-unit status provider rejected; call config after the query returns");
		return false;
	}
	{
		// Claim configuration under the state lock, then release it before any
		// plugin/user callback. addSink() can synchronously deliver READY, and
		// that callback may reenter release() on this same thread.
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state == LifecycleState::CONFIGURING ||
			_lifecycle_state == LifecycleState::CONFIGURED ||
			_lifecycle_state == LifecycleState::STARTING ||
			_lifecycle_state == LifecycleState::RUNNING ||
			_lifecycle_state == LifecycleState::STOPPING ||
			_lifecycle_state == LifecycleState::RELEASED)
		{
			WTSLogger::error("Runtime reconfiguration is not supported after configuration has started");
			return false;
		}
		if (_config != NULL)
		{
			// A parsed config means initialization may have created owners whose
			// rollback is not yet transactional. Keep them intact. A failed file
			// load leaves _config null and remains safely retryable.
			WTSLogger::error("Runtime reconfiguration is blocked while a parsed configuration may still own runtime resources");
			return false;
		}
		if (_active_ext_parser_creations != 0)
		{
			WTSLogger::error("Runtime reconfiguration is blocked while an external parser is being created");
			return false;
		}
		if (std::any_of(_quarantined_parser_owners.begin(), _quarantined_parser_owners.end(),
			[](const ParserAdapterPtr& owner) { return owner != NULL && owner->hasApi(); }))
		{
			WTSLogger::error("Runtime reconfiguration is blocked while a failed parser API owner is quarantined");
			return false;
		}
		_lifecycle_state = LifecycleState::CONFIGURING;
		_release_deferred_for_config = false;
		_status_ready.store(false, std::memory_order_release);
	}
	{
		std::unique_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
		_config_failed = true;
	}
	ScopeExit finalizeConfig([this]() {
		bool completeDeferredRelease = false;
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			if (_lifecycle_state == LifecycleState::CONFIGURING)
				_lifecycle_state = LifecycleState::UNCONFIGURED;
			if (_release_deferred_for_config)
			{
				_release_deferred_for_config = false;
				_teardown_pending = false;
				_teardown_in_progress = true;
				completeDeferredRelease = true;
			}
		}
		if (completeDeferredRelease)
		{
			std::unique_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
			_config_failed = true;
			lifecycleLock.unlock();
			completeRelease();
		}
		_lifecycle_changed.notify_all();
	});
	_config = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);
	if (_config == NULL)
	{
		WTSLogger::error("Loading runner config failed");
		return false;
	}

	//基础数据文件
	WTSVariant* cfgBF = _config->get("basefiles");
	if (cfgBF == NULL || cfgBF->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("Runner config requires basefiles to be an object");
		_config->release();
		_config = NULL;
		return false;
	}
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
		WTSLogger::log_raw(LL_INFO, "Holidays loaded");
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

	auto& ayHotTags = _hot_mgr.getHotTags();
	WTSArray* ayContracts = _bd_mgr.getContracts();
	for (auto it = ayContracts->begin(); it != ayContracts->end(); it++)
	{
		WTSContractInfo* cInfo = (WTSContractInfo*)(*it);

		for(const std::string& hotTag : ayHotTags)
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


	//初始化运行环境
	if (!initEngine())
	{
		WTSLogger::error("Initializing runner engine failed");
		return false;
	}

	//初始化数据管理
	if (!initDataMgr())
	{
		WTSLogger::error("Initializing configured data store failed");
		return false;
	}

	//初始化开平策略
	if (!initActionPolicy())
	{
		return false;
	}

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
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					if (!initParsers(var->get("parsers")))
					{
						WTSLogger::error("Loading parsers failed");
						var->release();
						return false;
					}
					var->release();
				}
				else
				{
					WTSLogger::error("Loading parser config {} failed", filename);
					return false;
				}
			}
			else
			{
				WTSLogger::error("Parser configuration {} not exists", filename);
				return false;
			}
		}
		else if (cfgParser->type() == WTSVariant::VT_Array)
		{
			if (!initParsers(cfgParser))
				return false;
		}
		else
		{
			WTSLogger::error("Parser configuration must be an array or a file path");
			return false;
		}
	}

	//初始化交易通道
	WTSVariant* cfgTraders = _config->get("traders");
	if(cfgTraders)
	{
		if (cfgTraders->type() == WTSVariant::VT_String)
		{
			const char* filename = cfgTraders->asCString();
			if (filename != NULL && filename[0] != '\0' && StdFile::exists(filename))
			{
				WTSLogger::info("Reading trader config from {}...", filename);
				WTSVariant* var = WTSCfgLoader::load_from_file(filename);
				if (var)
				{
					if (!initTraders(var->get("traders")))
					{
						WTSLogger::error("Loading traders failed");
						var->release();
						return false;
					}
					var->release();
				}
				else
				{
					WTSLogger::error("Loading trader config {} failed", filename);
					return false;
				}
			}
			else
			{
				WTSLogger::error("Trader configuration {} not exists", filename ? filename : "");
				return false;
			}
		}
		else if (cfgTraders->type() == WTSVariant::VT_Array)
		{
			if (!initTraders(cfgTraders))
			{
				WTSLogger::error("Loading traders failed");
				return false;
			}
		}
		else
		{
			WTSLogger::error("Trader configuration must be an array or a file path");
			return false;
		}
	}

	//初始化事件推送器
	initEvtNotifier();

	//如果不是高频引擎,则需要配置执行模块
	if (!_is_hft)
	{
		WTSVariant* cfgExec = _config->get("executers");
		if(cfgExec != NULL)
		{
			if(cfgExec->type() == WTSVariant::VT_String)
			{
				const char* filename = cfgExec->asCString();
				if (StdFile::exists(filename))
				{
					WTSLogger::info("Reading executer config from {}...", filename);
					WTSVariant* var = WTSCfgLoader::load_from_file(filename);
					if (var)
					{
						if (!initExecuters(var->get("executers")))
							WTSLogger::error("Loading executers failed");

						WTSVariant* c = var->get("routers");
						if (c != NULL)
							_cta_engine.loadRouterRules(c);

						var->release();
					}
					else
					{
						WTSLogger::error("Loading executer config {} failed", filename);
					}
				}
				else
				{
					WTSLogger::error("Trader configuration {} not exists", filename);
				}
			}
			else if(cfgExec->type() == WTSVariant::VT_Array)
			{
				initExecuters(cfgExec);
			}
		}

		WTSVariant* cfgRouter = _config->get("routers");
		if (cfgRouter != NULL)
			_cta_engine.loadRouterRules(cfgRouter);
		
	}

	if (_is_hft)
	{
		if (!initHftStrategies())
		{
			WTSLogger::error("Initializing configured HFT strategies failed");
			return false;
		}
	}
	else if (!initCtaStrategies())
	{
		WTSLogger::error("Initializing configured CTA strategies failed");
		return false;
	}
	
	bool releaseRequestedDuringConfig = false;
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		releaseRequestedDuringConfig = _release_deferred_for_config ||
			_lifecycle_state == LifecycleState::STOPPING;
		if (!releaseRequestedDuringConfig && _lifecycle_state == LifecycleState::CONFIGURING)
		{
			std::unique_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
			_config_failed = false;
			_status_ready.store(true, std::memory_order_release);
			_lifecycle_state = LifecycleState::CONFIGURED;
		}
	}
	_lifecycle_changed.notify_all();
	return !releaseRequestedDuringConfig;
}

ExecUnitStatusApiResult WtRtRunner::queryExecUnitInitStatus(uint32_t engineType,
	const char* executorId, const char* stdCode, ExecUnitInitStatus& status) const
{
	status = ExecUnitInitStatus();
	if (executorId == NULL || executorId[0] == '\0' || stdCode == NULL || stdCode[0] == '\0')
	{
		status.error = "executor ID and standard code are required";
		return EXEC_UNIT_STATUS_API_INVALID_ARGUMENT;
	}

	if (engineType == ET_HFT)
	{
		status.error = "HFT does not support execution-unit initialization status";
		return EXEC_UNIT_STATUS_API_UNSUPPORTED;
	}
	if (engineType != ET_CTA && engineType != ET_SEL)
	{
		status.error = "unknown engine type";
		return EXEC_UNIT_STATUS_API_INVALID_ARGUMENT;
	}
	if (!_status_ready.load(std::memory_order_acquire))
	{
		status.error = "runner is not configured";
		return EXEC_UNIT_STATUS_API_NOT_READY;
	}
	if (is_in_runner_scope(g_exec_status_call_scope, this))
	{
		status.error = "reentrant execution-unit status query rejected";
		return EXEC_UNIT_STATUS_API_INTERNAL_ERROR;
	}

	ExecCmdPtr executor;
	{
		std::shared_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
		if (!_status_ready.load(std::memory_order_acquire) || _config_failed || _engine == NULL)
		{
			status.error = "runner is not configured";
			return EXEC_UNIT_STATUS_API_NOT_READY;
		}
		if ((engineType == ET_CTA && (_is_hft || _is_sel)) ||
			(engineType == ET_SEL && !_is_sel))
		{
			status.error = "requested engine type does not match the configured engine";
			return EXEC_UNIT_STATUS_API_ENGINE_MISMATCH;
		}
		executor = engineType == ET_SEL
			? _sel_engine.findExecuter(executorId)
			: _cta_engine.findExecuter(executorId);
	}

	if (executor == NULL)
	{
		status.query_result = ExecUnitInitQueryResult::EXECUTOR_NOT_FOUND;
		status.error = "executor is not registered";
		return EXEC_UNIT_STATUS_API_EXECUTOR_NOT_FOUND;
	}
	std::shared_ptr<const IExecUnitInitStatusProvider> provider(
		executor, dynamic_cast<const IExecUnitInitStatusProvider*>(executor.get()));
	executor.reset();
	if (provider == NULL)
	{
		status.query_result = ExecUnitInitQueryResult::UNSUPPORTED;
		status.error = "executor does not provide unit initialization status";
		return EXEC_UNIT_STATUS_API_UNSUPPORTED;
	}

	try
	{
		ExecStatusCallScope statusCallScope(this);
		status = provider->queryUnitInitStatus(stdCode);
	}
	catch (...)
	{
		status = ExecUnitInitStatus();
		status.error = "failed to query execution-unit initialization status";
		return EXEC_UNIT_STATUS_API_INTERNAL_ERROR;
	}

	switch (status.query_result)
	{
	case ExecUnitInitQueryResult::OK:
		return EXEC_UNIT_STATUS_API_OK;
	case ExecUnitInitQueryResult::EXECUTOR_NOT_FOUND:
		return EXEC_UNIT_STATUS_API_EXECUTOR_NOT_FOUND;
	case ExecUnitInitQueryResult::UNSUPPORTED:
		return EXEC_UNIT_STATUS_API_UNSUPPORTED;
	case ExecUnitInitQueryResult::INVALID_ARGUMENT:
		return EXEC_UNIT_STATUS_API_INVALID_ARGUMENT;
	default:
		status.error = "execution-unit manager returned an unknown query result";
		return EXEC_UNIT_STATUS_API_INTERNAL_ERROR;
	}
}

ExecUnitStatusApiResult WtRtRunner::clearFailedExecUnitInitStatus(uint32_t engineType,
	const char* executorId, const char* stdCode)
{
	if (executorId == NULL || executorId[0] == '\0' || stdCode == NULL || stdCode[0] == '\0')
		return EXEC_UNIT_STATUS_API_INVALID_ARGUMENT;
	if (engineType == ET_HFT)
		return EXEC_UNIT_STATUS_API_UNSUPPORTED;
	if (engineType != ET_CTA && engineType != ET_SEL)
		return EXEC_UNIT_STATUS_API_INVALID_ARGUMENT;
	if (!_status_ready.load(std::memory_order_acquire))
		return EXEC_UNIT_STATUS_API_NOT_READY;
	if (is_in_runner_scope(g_exec_status_call_scope, this))
		return EXEC_UNIT_STATUS_API_INTERNAL_ERROR;

	ExecCmdPtr executor;
	{
		std::shared_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
		if (!_status_ready.load(std::memory_order_acquire) || _config_failed || _engine == NULL)
			return EXEC_UNIT_STATUS_API_NOT_READY;
		if ((engineType == ET_CTA && (_is_hft || _is_sel)) ||
			(engineType == ET_SEL && !_is_sel))
			return EXEC_UNIT_STATUS_API_ENGINE_MISMATCH;
		executor = engineType == ET_SEL
			? _sel_engine.findExecuter(executorId)
			: _cta_engine.findExecuter(executorId);
	}

	if (executor == NULL)
		return EXEC_UNIT_STATUS_API_EXECUTOR_NOT_FOUND;
	std::shared_ptr<IExecUnitInitStatusProvider> provider(
		executor, dynamic_cast<IExecUnitInitStatusProvider*>(executor.get()));
	executor.reset();
	if (provider == NULL)
		return EXEC_UNIT_STATUS_API_UNSUPPORTED;

	try
	{
		ExecStatusCallScope statusCallScope(this);
		ExecUnitInitClearResult result = provider->clearFailedUnitInitStatus(stdCode);
		switch (result)
		{
		case ExecUnitInitClearResult::CLEARED: return EXEC_UNIT_STATUS_API_OK;
		case ExecUnitInitClearResult::EXECUTOR_NOT_FOUND: return EXEC_UNIT_STATUS_API_EXECUTOR_NOT_FOUND;
		case ExecUnitInitClearResult::UNSUPPORTED: return EXEC_UNIT_STATUS_API_UNSUPPORTED;
		case ExecUnitInitClearResult::INVALID_ARGUMENT: return EXEC_UNIT_STATUS_API_INVALID_ARGUMENT;
		case ExecUnitInitClearResult::NOT_FOUND: return EXEC_UNIT_STATUS_API_CODE_NOT_FOUND;
		case ExecUnitInitClearResult::NOT_FAILED: return EXEC_UNIT_STATUS_API_NOT_FAILED;
		case ExecUnitInitClearResult::RETRY_BLOCKED: return EXEC_UNIT_STATUS_API_RETRY_BLOCKED;
		default: return EXEC_UNIT_STATUS_API_INTERNAL_ERROR;
		}
	}
	catch (...)
	{
		return EXEC_UNIT_STATUS_API_INTERNAL_ERROR;
	}
}

bool WtRtRunner::initCtaStrategies()
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

	// Resolve every active entry before init or context publication. Otherwise a
	// valid earlier strategy can leave a live context when a later factory/unit
	// is invalid and config() returns false.
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
		try
		{
			stra = _cta_mgr.createStrategy(name, id);
		}
		catch (...)
		{
			WTSLogger::error("Creating CTA strategy {} ({}) threw an exception", name, id);
			return false;
		}
		CtaStrategy* strategy = stra == NULL ? NULL : stra->self();
		if (strategy == NULL)
		{
			WTSLogger::error("Creating CTA strategy {} ({}) failed", name, id);
			return false;
		}
		pending.push_back({ cfgItem, id, name, slippage, stra });
	}

	// Keep init/context publication in the original per-strategy order. The
	// preflight above only moves factory resolution before context installation.
	for (const PendingCtaStrategy& item : pending)
	{
		item.strategy->self()->init(item.config->get("params"));
		CtaStraContext* ctx = new CtaStraContext(&_cta_engine, item.id, item.slippage);
		ctx->set_strategy(item.strategy->self());
		_cta_engine.addContext(CtaContextPtr(ctx));
	}

	return true;
}

bool WtRtRunner::initSelStrategies()
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

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		int32_t slippage = cfgItem->getInt32("slippage");

		uint32_t date = cfgItem->getUInt32("date");
		uint32_t time = cfgItem->getUInt32("time");
		const char* period = cfgItem->getCString("period");
		if (name == NULL || name[0] == '\0')
		{
			WTSLogger::error("Creating SEL strategy {} failed: strategy name is required", id);
			return false;
		}

		TaskPeriodType ptype;
		if (wt_stricmp(period, "d") == 0)
			ptype = TPT_Daily;
		else if (wt_stricmp(period, "w") == 0)
			ptype = TPT_Weekly;
		else if (wt_stricmp(period, "m") == 0)
			ptype = TPT_Monthly;
		else if (wt_stricmp(period, "y") == 0)
			ptype = TPT_Yearly;
		else
			ptype = TPT_None;

		SelStrategyPtr stra = _sel_mgr.createStrategy(name, id);
		SelStrategy* strategy = stra == NULL ? NULL : stra->self();
		if (strategy == NULL)
		{
			WTSLogger::error("Creating SEL strategy {} ({}) failed", name, id);
			return false;
		}
		strategy->init(cfgItem->get("params"));
		SelStraContext* ctx = new SelStraContext(&_sel_engine, id, slippage);
		ctx->set_strategy(strategy);
		_sel_engine.addContext(SelContextPtr(ctx), date, time, ptype);
	}

	return true;
}

bool WtRtRunner::initHftStrategies()
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

	// Resolve all active factories before any HFT context or trader sink is
	// installed, so a later invalid unit cannot leave earlier registrations.
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
		bool bAgent = cfgItem->getBoolean("agent");
		int32_t slippage = cfgItem->getInt32("slippage");
		if (id == NULL || id[0] == '\0' || name == NULL || name[0] == '\0')
		{
			WTSLogger::error("Creating HFT strategy failed: strategy id and name are required");
			return false;
		}

		HftStrategyPtr stra;
		try
		{
			stra = _hft_mgr.createStrategy(name, id);
		}
		catch (...)
		{
			WTSLogger::error("Creating HFT strategy {} ({}) threw an exception", name, id);
			return false;
		}
		HftStrategy* strategy = stra == NULL ? NULL : stra->self();
		if (strategy == NULL)
		{
			WTSLogger::error("Creating HFT strategy {} ({}) failed", name, id);
			return false;
		}

		pending.push_back({ cfgItem, id, name, bAgent, slippage, stra });
	}

	for (const PendingHftStrategy& item : pending)
	{
		item.strategy->self()->init(item.config->get("params"));
		HftStraContext* ctx = new HftStraContext(&_hft_engine, item.id, item.agent, item.slippage);
		ctx->set_strategy(item.strategy->self());
		// Publish the context before addSink: addSink may synchronously deliver
		// the initial READY event when the adapter is already connected.
		_hft_engine.addContext(HftContextPtr(ctx));

		const char* traderid = item.config->getCString("trader");
		TraderAdapterPtr trader = _traders.getAdapter(traderid);
		if (trader)
		{
			ctx->setTrader(trader.get());
			trader->addSink(ctx);
		}
		else
		{
			WTSLogger::error("Trader {} not exists, Binding trader to HFT strategy failed", traderid);
		}
	}

	return true;
}

bool WtRtRunner::initEngine()
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
		WTSLogger::info("Trading environment initialized, engine name: HFT");
		_hft_engine.init(cfg, &_bd_mgr, &_data_mgr, &_hot_mgr, &_notifier);
		_engine = &_hft_engine;
	}
	else if (_is_sel)
	{
		WTSLogger::info("Trading environment initialized, engine name: SEL");
		_sel_engine.init(cfg, &_bd_mgr, &_data_mgr, &_hot_mgr, &_notifier);
		_engine = &_sel_engine;
	}
	else
	{
		WTSLogger::info("Trading environment initialized, engine name: CTA");
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

bool WtRtRunner::initDataMgr()
{
	WTSVariant* cfg = _config->get("data");
	if (cfg == NULL)
		return true;

	_data_mgr.regsiter_loader(this);

	const bool initialized = _data_mgr.init(cfg, _engine, true);
	if (!initialized && cfg->get("store") != NULL)
		return false;

	WTSLogger::log_raw(LL_INFO, "Data manager initialized");
	return true;
}

bool WtRtRunner::initParsers(WTSVariant* cfgParsers)
{
	if (cfgParsers == NULL || cfgParsers->type() != WTSVariant::VT_Array || _engine == NULL)
		return false;

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgParsers->size(); idx++)
	{
		WTSVariant* cfgItem = cfgParsers->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		// By Wesley @ 2021.12.14
		// 如果id为空，则生成自动id
		std::string realid = id;
		if (realid.empty())
		{
			static uint32_t auto_parserid = 1000;
			realid = fmt::format("auto_parser_{}", auto_parserid++);
		}
		if (_parsers.getAdapter(realid.c_str()) != NULL)
		{
			WTSLogger::error("Parser {} is already registered", realid);
			return false;
		}

		ParserAdapterPtr adapter(new ParserAdapter);
		bool initialized = false;
		try
		{
			initialized = adapter->init(realid.c_str(), cfgItem, this,
				_engine->get_basedata_mgr(), _engine->get_hot_mgr());
		}
		catch (...)
		{
			_quarantined_parser_owners.emplace_back(adapter);
			WTSLogger::error("Parser {} initialization threw", realid);
			return false;
		}
		if (!initialized)
		{
			// Failed init can still leave an API registered to this SPI. Preserve
			// it without making ParserAdapterMgr::run() treat it as runnable.
			_quarantined_parser_owners.emplace_back(adapter);
			return false;
		}
		if (!_parsers.addAdapter(realid.c_str(), adapter))
		{
			// Preserve an initialized candidate if concurrent registration made
			// the final map insertion fail.
			_quarantined_parser_owners.emplace_back(adapter);
			return false;
		}

		count++;
	}

	WTSLogger::info("{} parsers loaded", count);

	return true;
}

bool WtRtRunner::initExecuters(WTSVariant* cfgExecuter)
{
	if (cfgExecuter == NULL || cfgExecuter->type() != WTSVariant::VT_Array)
		return false;

	//先加载自带的执行器工厂
	std::string path = WtHelper::getInstDir() + "executer//";
	_exe_factory.loadFactories(path.c_str());

	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgExecuter->size(); idx++)
	{
		WTSVariant* cfgItem = cfgExecuter->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		std::string name = cfgItem->getCString("name");	//local,diff,dist
		if (name.empty())
			name = "local";

		if(name == "local")
		{
			WtLocalExecuter* executer = new WtLocalExecuter(&_exe_factory, id, &_data_mgr);
			if (!executer->init(cfgItem))
				return false;
			// addSink can synchronously deliver READY. Keep the executor owned by
			// the engine before exposing its raw sink pointer to the adapter.
			_cta_engine.addExecuter(ExecCmdPtr(executer));

			const char* tid = cfgItem->getCString("trader");
			if(strlen(tid) == 0)
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					trader->addSink(executer);
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
				}
			}
		}
		else if (name == "diff")
		{
			WtDiffExecuter* executer = new WtDiffExecuter(&_exe_factory, id, &_data_mgr, &_bd_mgr);
			if (!executer->init(cfgItem))
				return false;
			_cta_engine.addExecuter(ExecCmdPtr(executer));

			const char* tid = cfgItem->getCString("trader");
			if (strlen(tid) == 0)
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					trader->addSink(executer);
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
				}
			}
		}
		else if (name == "arbi")
		{
			WtArbiExecuter* executer = new WtArbiExecuter(&_exe_factory, id, &_data_mgr);
			if (!executer->init(cfgItem))
				return false;
			_cta_engine.addExecuter(ExecCmdPtr(executer));

			const char* tid = cfgItem->getCString("trader");
			if (strlen(tid) == 0)
			{
				WTSLogger::error("No Trader configured for Executer {}", id);
			}
			else
			{
				TraderAdapterPtr trader = _traders.getAdapter(tid);
				if (trader)
				{
					executer->setTrader(trader.get());
					trader->addSink(executer);
				}
				else
				{
					WTSLogger::error("Trader {} not exists, cannot configured for executer {}", tid, id);
				}
			}
		}
		else
		{
			WtDistExecuter* executer = new WtDistExecuter(id);
			if (!executer->init(cfgItem))
				return false;

			_cta_engine.addExecuter(ExecCmdPtr(executer));
		}
		
		count++;
	}

	WTSLogger::info("{} executers loaded", count);



	return true;
}

bool WtRtRunner::initEvtNotifier()
{
	WTSVariant* cfg = _config->get("notifier");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}

bool WtRtRunner::initTraders(WTSVariant* cfgTraders)
{
	if (cfgTraders == NULL || cfgTraders->type() != WTSVariant::VT_Array)
		return false;
	
	uint32_t count = 0;
	for (uint32_t idx = 0; idx < cfgTraders->size(); idx++)
	{
		WTSVariant* cfgItem = cfgTraders->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");

		TraderAdapterPtr adapter(new TraderAdapter(&_notifier));
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

void WtRtRunner::run(bool bAsync /* = false */)
{
	if (is_in_runner_scope(g_exec_status_call_scope, this))
	{
		WTSLogger::error("Runner start from execution-unit status provider rejected; call run after the query returns");
		return;
	}
	std::unique_lock<std::mutex> startupLock;
	{
		std::unique_lock<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state != LifecycleState::CONFIGURED || _config_failed)
		{
			WTSLogger::error("Runner is not configured or has been released; refusing to run");
			return;
		}
		if (_active_ext_parser_creations != 0)
		{
			WTSLogger::error("Runner start is blocked while an external parser is being created");
			return;
		}
		{
			std::shared_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
			if (!_status_ready.load(std::memory_order_acquire) || _engine == NULL)
			{
				WTSLogger::error("Runner is not configured; refusing to run");
				return;
			}
		}
		startupLock = std::unique_lock<std::mutex>(_run_start_mutex);
		_lifecycle_state = LifecycleState::STARTING;
		_release_deferred_for_startup = false;
		_deferred_release_requested.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> exitLock(_run_exit_mutex);
			_to_exit.store(false, std::memory_order_release);
		}
	}
	_lifecycle_changed.notify_all();

	bool startupFailed = false;
	{
		RunnerStartupScope startupScope(this);
		try
		{
			_parsers.run();
			if (!_to_exit.load(std::memory_order_acquire))
				_traders.run();
			if (!_to_exit.load(std::memory_order_acquire))
				_engine->run();

			if (!bAsync && !_to_exit.load(std::memory_order_acquire))
			{
				g_runner_signal_exit_requested = 0;
				g_runner_signal_log_requested = 0;
				g_runner_signal_run_active = 1;
				// SignalHook is process-global and cannot be safely unregistered by
				// one runner. These callbacks avoid runner captures and only set
				// sig_atomic_t flags; the synchronous run thread does all work.
				install_signal_hooks(&runner_signal_log_callback, &runner_signal_exit_callback);
			}
		}
		catch (...)
		{
			startupFailed = true;
			print_stack_trace([](const char* message) {
				WTSLogger::error(message);
			});
		}
	}

	startupLock.unlock();
	if (startupFailed)
		g_runner_signal_run_active = 0;
	bool completeDeferredRelease = false;
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state == LifecycleState::STARTING)
			_lifecycle_state = LifecycleState::RUNNING;
		if (_release_deferred_for_startup)
		{
			_release_deferred_for_startup = false;
			_deferred_release_requested.store(false, std::memory_order_release);
			completeDeferredRelease = true;
		}
	}
	_lifecycle_changed.notify_all();

	if (completeDeferredRelease)
	{
		{
			std::unique_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
			_config_failed = true;
		}
		completeRelease();
	}

	if (!bAsync && !startupFailed)
	{
		auto processSignalExit = [this](std::sig_atomic_t signum) {
			bool requested = false;
			{
				std::lock_guard<std::mutex> configLock(_status_config_mutex);
				if (_lifecycle_state != LifecycleState::STOPPING &&
					_lifecycle_state != LifecycleState::RELEASED)
				{
					std::lock_guard<std::mutex> exitLock(_run_exit_mutex);
					_to_exit.store(true, std::memory_order_release);
					requested = true;
				}
			}
			if (requested)
			{
				_run_exit_condition.notify_all();
				WTSLogger::info("Exit requested by signal {}", static_cast<int>(signum));
			}
		};

		while (!_to_exit.load(std::memory_order_acquire))
		{
			{
				std::unique_lock<std::mutex> exitLock(_run_exit_mutex);
				_run_exit_condition.wait_for(exitLock, std::chrono::milliseconds(10), [this]() {
					return _to_exit.load(std::memory_order_acquire);
				});
			}

			if (g_runner_signal_log_requested != 0)
			{
				g_runner_signal_log_requested = 0;
				WTSLogger::info("Runner received a signal notification");
			}
			const std::sig_atomic_t signum = g_runner_signal_exit_requested;
			if (signum != 0)
			{
				g_runner_signal_exit_requested = 0;
				processSignalExit(signum);
			}
		}

		// Hand off any signal that interrupted the final loop check before
		// deactivating the signal-safe callback. Later signals use _Exit.
		g_runner_signal_run_active = 0;
		const std::sig_atomic_t pendingSignal = g_runner_signal_exit_requested;
		if (pendingSignal != 0)
		{
			g_runner_signal_exit_requested = 0;
			processSignalExit(pendingSignal);
		}
		if (g_runner_signal_log_requested != 0)
		{
			g_runner_signal_log_requested = 0;
			WTSLogger::info("Runner received a signal notification");
		}
	}
}

const char* LOG_TAGS[] = {
	"all",
	"debug",
	"info",
	"warn",
	"error",
	"fatal",
	"none",
};

void WtRtRunner::handleLogAppend(WTSLogLevel ll, const char* msg)
{
	_notifier.notify_log(LOG_TAGS[ll-100], msg);
}

void WtRtRunner::release()
{
	if (is_in_runner_scope(g_runner_teardown_scope, this))
	{
		WTSLogger::error("Reentrant runner release during teardown rejected");
		return;
	}
	if (is_in_runner_scope(g_exec_status_call_scope, this))
	{
		WTSLogger::error("Reentrant runner release from execution-unit status provider rejected; call release after the query returns");
		return;
	}
	bool deferredUntilStartupCompletes = false;
	bool deferredUntilConfigCompletes = false;
	{
		std::unique_lock<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state == LifecycleState::RELEASED)
			return;
		if (_lifecycle_state == LifecycleState::CONFIGURING)
		{
			_lifecycle_state = LifecycleState::STOPPING;
			_status_ready.store(false, std::memory_order_release);
			_release_deferred_for_config = true;
			_teardown_pending = false;
			_teardown_inputs_stopped = false;
			_teardown_parsers_stopped = false;
			_teardown_engine_stopped = false;
			_teardown_exec_preflight_passed = false;
			_teardown_in_progress = false;
			deferredUntilConfigCompletes = true;
		}
		else if (_manager_release_in_progress)
		{
			// Do not inspect the manager map while it is being cleared. A sink
			// callback cannot remain active this late in teardown, so return safely.
			return;
		}
		else
		{
			// The status lock serializes this read against config and the final
			// manager release; no manager-map mutation can race the callback query.
			const bool inTraderSinkCallback = _traders.isInSinkCallback();
			const bool inRunnerCallback = is_in_runner_scope(g_runner_callback_scope, this);
			const bool inProducerCallback = inTraderSinkCallback || inRunnerCallback;
		if (_lifecycle_state == LifecycleState::STOPPING)
		{
			if (_release_deferred_for_config)
				return;
			if (_release_deferred_for_startup)
				return;
			if (_teardown_in_progress)
			{
				if (inProducerCallback)
				{
					WTSLogger::warn("Runner release reentered from a producer callback; active teardown will finish after the callback returns");
					return;
				}
				++_release_waiters;
				_lifecycle_changed.notify_all();
				_lifecycle_changed.wait(configLock, [this]() {
					return _lifecycle_state == LifecycleState::RELEASED || !_teardown_in_progress;
				});
				if (_lifecycle_state == LifecycleState::RELEASED)
				{
					--_release_waiters;
					_lifecycle_changed.notify_all();
					return;
				}
				if (!_teardown_pending)
				{
					--_release_waiters;
					_lifecycle_changed.notify_all();
					return;
				}
				--_release_waiters;
				_teardown_pending = false;
				_teardown_in_progress = true;
				_lifecycle_changed.notify_all();
			}
			else if (_teardown_pending)
			{
				if (inProducerCallback)
					return;
				_teardown_pending = false;
				_teardown_in_progress = true;
			}
			else
			{
				// A release requested during startup is completed by run() once startup exits.
				return;
			}
		}
		else
		{
			deferredUntilStartupCompletes = _lifecycle_state == LifecycleState::STARTING;
			_lifecycle_state = LifecycleState::STOPPING;
			_status_ready.store(false, std::memory_order_release);
			_release_deferred_for_startup = deferredUntilStartupCompletes;
			_teardown_pending = false;
			_teardown_inputs_stopped = false;
			_teardown_parsers_stopped = false;
			_teardown_engine_stopped = false;
			_teardown_exec_preflight_passed = false;
			_teardown_in_progress = !deferredUntilStartupCompletes;
			_deferred_release_requested.store(deferredUntilStartupCompletes,
				std::memory_order_release);
		}
		}
	}
	_lifecycle_changed.notify_all();
	{
		std::lock_guard<std::mutex> exitLock(_run_exit_mutex);
		_to_exit.store(true, std::memory_order_release);
	}
	_run_exit_condition.notify_all();

	if (deferredUntilStartupCompletes)
	{
		if (is_in_runner_scope(g_runner_startup_scope, this))
			WTSLogger::warn("Reentrant runner release during startup deferred until startup completes");
		else
			WTSLogger::warn("Runner release during startup deferred until startup completes");
		return;
	}
	if (deferredUntilConfigCompletes)
	{
		WTSLogger::warn("Runner release requested during config; cleanup is deferred until the config callback stack unwinds");
		return;
	}

	// Drain in-progress engine/registry lookups. Provider callbacks run outside
	// this lock and keep their executor alive through an ExecCmdPtr snapshot.
	{
		std::unique_lock<std::shared_mutex> lifecycleLock(_status_lifecycle_mutex);
		_config_failed = true;
	}

	// Wait for parser/trader/engine startup to finish before tearing any of them down.
	{
		std::unique_lock<std::mutex> startupLock(_run_start_mutex);
	}
	completeRelease();
}


void WtRtRunner::completeRelease()
{
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state == LifecycleState::RELEASED)
			return;
		_teardown_in_progress = true;
		_teardown_pending = false;
	}
	RunnerTeardownScope teardownScope(this);
	// Close trader sink admission before any retryable-quarantine exit. This is
	// non-blocking and does not release adapter or runner owners.
	try
	{
		_traders.requestStopSinkNotifications();
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending: trader sink shutdown request failed; owners remain alive for retry");
		return;
	}
	if (is_in_runner_scope(g_runner_callback_scope, this))
	{
		// Registered runner callbacks and parser-to-engine forwarding can execute
		// on parser or engine producer threads. Never stop/join a producer while
		// its callback stack still owns this runner; require a control-thread retry.
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending because release was requested from a runner callback; retry release from a control thread after the callback returns");
		return;
	}
	bool parserCreationActive = false;
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		parserCreationActive = _active_ext_parser_creations != 0;
	}
	if (parserCreationActive)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending while an admitted external parser creation finishes; retry release from a control thread");
		return;
	}
	const bool hasUnquiescedParserApi = std::any_of(
		_quarantined_parser_owners.begin(), _quarantined_parser_owners.end(),
		[](const ParserAdapterPtr& owner) { return owner != NULL && owner->hasApi(); });
	if (hasUnquiescedParserApi)
	{
		// There is no ParserAdapter/IParserApi callback-drain result for a
		// failed init candidate. Keep the candidate and its borrowed runner graph
		// alive, and do not release parser or engine owners without that proof.
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending: quarantined failed parser API owners have no callback-drain proof; runner, engine, data, base, and hot-manager owners must remain alive");
		return;
	}
	bool parserQuoteActive = false;
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		parserQuoteActive = _active_ext_parser_quotes != 0;
	}
	if (parserQuoteActive)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending while an admitted external parser quote finishes; retry release from a control thread");
		return;
	}
	for (const ParserAdapterPtr& owner : _quarantined_parser_owners)
	{
		if (owner != NULL)
			owner->release();
	}
	_quarantined_parser_owners.clear();
	if (_traders.isInSinkCallback())
	{
		// Do not stop/join parser or engine producers from a trader callback:
		// either may depend on this callback thread. Keep every owner intact and
		// let a later control-thread release perform the blocking teardown.
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner release was requested from a trader callback; control-thread release is required to stop producers and complete teardown");
		return;
	}

	// Drain in-flight trader callbacks before joining parser or engine workers.
	// A callback may be waiting on work scheduled by either producer; stopping a
	// producer first could therefore make its join wait on the callback while the
	// callback waits on that producer. Notifications have already been closed,
	// so a successful drain is stable while the producers are stopped afterward.
	if (!_traders.waitSinkNotificationsStopped())
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending; trader callbacks have not drained, so parser, engine, and adapter owners remain alive until release is retried from a control thread");
		return;
	}

	bool executerPreflightPassed = _teardown_exec_preflight_passed;
	if (!executerPreflightPassed)
	{
		try
		{
			executerPreflightPassed = preflight_runner_exec_quiesce({
				&_cta_engine.getExecuterMgr(),
				&_sel_engine.getExecuterMgr()
			});
		}
		catch (...)
		{
			WTSLogger::error("Runner execution preflight threw while stopping the runtime");
		}
		if (executerPreflightPassed)
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_exec_preflight_passed = true;
		}
	}
	if (!executerPreflightPassed)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending; CTA/SEL execution preflight did not pass, so parser, engine, adapter, factory, and context owners remain alive until release is retried from a control thread");
		return;
	}

	bool parsersStopped = true;
	if (!_teardown_parsers_stopped)
	{
		// STOPPING rejects new quote admissions, and the admitted quote and
		// creation counters were drained above. No reader can race this map clear;
		// do not hold _parser_registry_mutex across provider release callbacks.
		try { _parsers.release(); }
		catch (...) { WTSLogger::error("Parser release threw while stopping the runner"); parsersStopped = false; }
		if (parsersStopped)
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_parsers_stopped = true;
		}
	}

	if (!parsersStopped)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending; adapters and engine owners remain alive until release is retried from a control thread");
		return;
	}

	// Stop every CTA/SEL execution host and all plugin units through one stable
	// coordinator. A failed preflight or wait keeps the engines, API adapters,
	// contexts, factories, and module owners alive for a control-thread retry.
	bool executersQuiesced = false;
	try
	{
		executersQuiesced = _exec_quiesce_coordinator != NULL &&
			_exec_quiesce_coordinator->quiesce_all();
	}
	catch (...)
	{
		WTSLogger::error("Runner execution quiesce threw while stopping the runtime");
	}
	if (!executersQuiesced)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending; CTA/SEL executor or plugin work has not quiesced, so engine, adapter, factory, and context owners remain alive until release is retried from a control thread");
		return;
	}

	bool engineStopped = true;
	if (!_teardown_engine_stopped)
	{
		try { if (_engine) _engine->release(); }
		catch (...) { WTSLogger::error("Engine release threw while stopping the runner"); engineStopped = false; }
		if (engineStopped)
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_engine_stopped = true;
		}
	}

	if (!engineStopped)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		WTSLogger::warn("Runner teardown is pending; engine owners remain alive until release is retried from a control thread");
		return;
	}

	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		_teardown_inputs_stopped = true;
	}

	// CTA/SEL executor hosts and their registered execution plugin units have
	// completed the runner-wide coordinator before this point. Parser and trader
	// plugin release still rely on their own callback-drain contracts; the trader
	// sink barrier above does not prove that a provider will never emit a late SPI
	// callback after release returns.
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		_manager_release_in_progress = true;
	}
	bool adaptersReleased = true;
	try { _traders.release(); }
	catch (...) { WTSLogger::error("Trader release threw while stopping the runner"); adaptersReleased = false; }
	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		_manager_release_in_progress = false;
	}
	if (!adaptersReleased)
	{
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			_teardown_pending = true;
			_teardown_in_progress = false;
		}
		_lifecycle_changed.notify_all();
		return;
	}
	try { WTSLogger::stop(); }
	catch (...) { }

	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		_lifecycle_state = LifecycleState::RELEASED;
		_release_deferred_for_startup = false;
		_teardown_in_progress = false;
		_teardown_pending = false;
		_deferred_release_requested.store(false, std::memory_order_release);
	}
	_lifecycle_changed.notify_all();
}

bool WtRtRunner::initActionPolicy()
{
	const char* action_file = _config->getCString("bspolicy");
	if (strlen(action_file) <= 0)
		return false;

	bool ret = _act_policy.init(action_file);
	WTSLogger::info("Action policies initialized");
	return ret;
}

bool WtRtRunner::addCtaFactories(const char* folder)
{
	return _cta_mgr.loadFactories(folder);
}

bool WtRtRunner::addSelFactories(const char* folder)
{
	return _sel_mgr.loadFactories(folder);
}

bool WtRtRunner::addExeFactories(const char* folder)
{
	return _exe_factory.loadFactories(folder);
}

bool WtRtRunner::addHftFactories(const char* folder)
{
	return _hft_mgr.loadFactories(folder);
}

void WtRtRunner::hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_ordque)
		_cb_hft_ordque(id, stdCode, &newOrdQue->getOrdQueStruct());
}

void WtRtRunner::hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_orddtl)
		_cb_hft_orddtl(id, stdCode, &newOrdDtl->getOrdDtlStruct());
}

void WtRtRunner::hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTrans)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_hft_trans)
		_cb_hft_trans(id, stdCode, &newTrans->getTransStruct());
}

#pragma region "Extended Parser"
void WtRtRunner::parser_init(const char* id)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_INIT, id);
}

void WtRtRunner::parser_connect(const char* id)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_CONNECT, id);
}

void WtRtRunner::parser_disconnect(const char* id)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_DISCONNECT, id);
}

void WtRtRunner::parser_release(const char* id)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_RELEASE, id);
}

void WtRtRunner::parser_subscribe(const char* id, const char* code)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_parser_sub)
		_cb_parser_sub(id, code, true);
}

void WtRtRunner::parser_unsubscribe(const char* id, const char* code)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_parser_sub)
		_cb_parser_sub(id, code, false);
}

void WtRtRunner::on_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag)
{
	if (id == NULL || id[0] == '\0')
	{
		WTSLogger::warn("External parser quote ignored: parser id is null or empty");
		return;
	}
	if (curTick == NULL)
	{
		WTSLogger::warn("External parser quote for {} ignored: tick is null", id);
		return;
	}

	{
		std::lock_guard<std::mutex> configLock(_status_config_mutex);
		if (_lifecycle_state != LifecycleState::CONFIGURED &&
			_lifecycle_state != LifecycleState::STARTING &&
			_lifecycle_state != LifecycleState::RUNNING)
			return;
		++_active_ext_parser_quotes;
	}
	ScopeExit quoteFinished([this]() {
		{
			std::lock_guard<std::mutex> configLock(_status_config_mutex);
			--_active_ext_parser_quotes;
		}
		_lifecycle_changed.notify_all();
	});
	RunnerCallbackScope callbackScope(this);

	// Copy the strong owner while holding the registry lock, then release it
	// before any parser or engine callback can reenter the runner.
	ParserAdapterPtr adapter;
	{
		std::lock_guard<std::mutex> parserRegistryLock(_parser_registry_mutex);
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

#pragma endregion 

#pragma region "Extended Executer"
void WtRtRunner::executer_init(const char* id)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_exec_init)
		_cb_exec_init(id);
}

void WtRtRunner::executer_set_position(const char* id, const char* stdCode, double target)
{
	RunnerCallbackScope callbackScope(this);
	if (_cb_exec_cmd)
		_cb_exec_cmd(id, stdCode, target);
}
#pragma endregion
