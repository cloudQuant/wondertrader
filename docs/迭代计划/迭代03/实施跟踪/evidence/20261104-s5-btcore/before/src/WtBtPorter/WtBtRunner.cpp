/*!
 * \file WtBtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtBtRunner.h"
#include "ExpCtaMocker.h"
#include "ExpSelMocker.h"
#include "ExpHftMocker.h"

#include <iomanip>
#include <exception>

#include "../WtBtCore/ExecMocker.h"
#include "../WtBtCore/WtHelper.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/ModuleHelper.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"

#ifdef _MSC_VER
#include "../Common/mdump.h"
#include <filesystem>
namespace fs = std::filesystem;

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

WtBtRunner::WtBtRunner()
	: _cb_cta_init(NULL)
	, _cb_cta_sessevt(NULL)
	, _cb_cta_tick(NULL)
	, _cb_cta_calc(NULL)
	, _cb_cta_calc_done(NULL)
	, _cb_cta_bar(NULL)
	, _cb_cta_cond_trigger(NULL)

	, _cb_sel_init(NULL)
	, _cb_sel_sessevt(NULL)

	, _cb_sel_tick(NULL)
	, _cb_sel_calc(NULL)
	, _cb_sel_calc_done(NULL)
	, _cb_sel_bar(NULL)
	, _cb_hft_init(NULL)
	, _cb_hft_sessevt(NULL)

	, _cb_hft_tick(NULL)
	, _cb_hft_bar(NULL)
	, _cb_hft_chnl(NULL)

	, _cb_hft_ord(NULL)
	, _cb_hft_trd(NULL)
	, _cb_hft_entrust(NULL)
	, _cb_hft_ordque(NULL)
	, _cb_hft_orddtl(NULL)
	, _cb_hft_trans(NULL)

	, _ext_fnl_bar_loader(NULL)
	, _ext_raw_bar_loader(NULL)
	, _ext_adj_fct_loader(NULL)
	, _ext_tick_loader(NULL)
	, _loader_auto_trans(true)

	, _cta_mocker(NULL)
	, _sel_mocker(NULL)
	, _exec_mocker(NULL)
	, _hft_mocker(NULL)

	, _inited(false)
	, _config_failed_permanently(false)
	, _running(false)
	, _stop_requested(false)
	, _release_requested(false)
	, _released(false)
	, _async(false)
	, _feeder_bars(NULL)
	, _cfg(NULL)
{
}


WtBtRunner::~WtBtRunner()
{
	if (isReplayThread())
	{
		requestReplayStop(true);
		// The worker still uses this runner and its embedded replayer. Their
		// destruction cannot be deferred from inside the worker callback.
		std::terminate();
	}

	stop();
	if (_exec_mocker)
	{
		release();
		// The runner owns the replay context referenced by ExecMocker. If plugin
		// quiescence cannot be completed here, destroying the context would leave
		// a live plugin with a dangling ExecuteContext.
		if (_exec_mocker)
			std::terminate();
	}

	if (_cfg)
	{
		_cfg->release();
		_cfg = NULL;
	}
}

bool WtBtRunner::loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb)
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

bool WtBtRunner::loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb)
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

bool WtBtRunner::loadAllAdjFactors(void* obj, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader("");
}

bool WtBtRunner::loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader(stdCode);
}

bool WtBtRunner::loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_tick_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_ticks = cb;

	return _ext_tick_loader(stdCode, uDate);
}

void WtBtRunner::feedRawBars(WTSBarStruct* bars, uint32_t count)
{
	if(_ext_fnl_bar_loader == NULL && _ext_raw_bar_loader == NULL)
	{
		WTSLogger::error("Cannot feed bars because of no extented bar loader registered.");
		return;
	}
	if (_feeder_bars == NULL)
	{
		WTSLogger::error("Cannot feed bars because no bar feeder is loaded.");
		return;
	}

	_feeder_bars(_feed_obj, bars, count);
}

void WtBtRunner::feedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
{
	if(_ext_adj_fct_loader == NULL)
	{
		WTSLogger::error("Cannot feed adjusting factors because of no extented adjusting factor loader registered.");
		return;
	}

	_feeder_fcts(_feed_obj, stdCode, dates, factors, count);
}

void WtBtRunner::feedRawTicks(WTSTickStruct* ticks, uint32_t count)
{
	if (_ext_tick_loader == NULL)
	{
		WTSLogger::error("Cannot feed ticks because of no extented tick loader registered.");
		return;
	}

	// The external tick loader calls this synchronously while loadRawHisTicks
	// holds _feed_mtx. Do not reacquire the non-recursive feed lock here.
	_feeder_ticks(_feed_obj, ticks, count);
}

void WtBtRunner::registerCtaCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, 
	FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone /* = NULL */, FuncStraCondTriggerCallback cbCondTrigger /* = NULL */)
{
	_cb_cta_init = cbInit;
	_cb_cta_tick = cbTick;
	_cb_cta_calc = cbCalc;
	_cb_cta_bar = cbBar;
	_cb_cta_sessevt = cbSessEvt;

	_cb_cta_calc_done = cbCalcDone;
	_cb_cta_cond_trigger = cbCondTrigger;

	WTSLogger::info("Callbacks of CTA engine registration done");
}

void WtBtRunner::registerSelCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
	FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone/* = NULL*/)
{
	_cb_sel_init = cbInit;
	_cb_sel_tick = cbTick;
	_cb_sel_calc = cbCalc;
	_cb_sel_bar = cbBar;
	_cb_sel_sessevt = cbSessEvt;

	_cb_sel_calc_done = cbCalcDone;

	WTSLogger::info("Callbacks of SEL engine registration done");
}

void WtBtRunner::registerHftCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar,
	FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust, 
	FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt)
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

	WTSLogger::info("Callbacks of HFT engine registration done");
}

uint32_t WtBtRunner::initCtaMocker(const char* name, int32_t slippage /* = 0 */, bool hook /* = false */, 
	bool persistData /* = true */, bool bIncremental /* = false */, bool isRatioSlp /* = false */)
{
	if(_cta_mocker)
	{
		delete _cta_mocker;
		_cta_mocker = NULL;
	}

	_cta_mocker = new ExpCtaMocker(&_replayer, name, slippage, persistData, &_notifier, isRatioSlp);
	if (bIncremental)
	{
		_cta_mocker->load_incremental_data(name);
	}
	if(hook) _cta_mocker->install_hook();
	_replayer.register_sink(_cta_mocker, name);
	return _cta_mocker->id();
}

uint32_t WtBtRunner::initHftMocker(const char* name, bool hook/* = false*/)
{
	if (_hft_mocker)
	{
		delete _hft_mocker;
		_hft_mocker = NULL;
	}

	_hft_mocker = new ExpHftMocker(&_replayer, name);
	if (hook) _hft_mocker->install_hook();
	_replayer.register_sink(_hft_mocker, name);
	return _hft_mocker->id();
}

uint32_t WtBtRunner::initSelMocker(const char* name, uint32_t date, uint32_t time, const char* period, 
	const char* trdtpl /* = "CHINA" */, const char* session /* = "TRADING" */, int32_t slippage /* = 0 */, bool isRatioSlp /* = false */)
{
	if (name == nullptr || !HisDataReplayer::validate_task_parameters(period, trdtpl, session))
		return 0;

	if (_sel_mocker)
	{
		delete _sel_mocker;
		_sel_mocker = NULL;
	}

	_sel_mocker = new ExpSelMocker(&_replayer, name, slippage, isRatioSlp);
	_replayer.register_sink(_sel_mocker, name);

	_replayer.register_task(_sel_mocker->id(), date, time, period, trdtpl, session);
	return _sel_mocker->id();
}

void WtBtRunner::ctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType/*= ET_CTA*/)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_bar) _cb_cta_bar(id, stdCode, period, newBar); break;
	case ET_HFT: if (_cb_hft_bar) _cb_hft_bar(id, stdCode, period, newBar); break;
	case ET_SEL: if (_cb_sel_bar) _cb_sel_bar(id, stdCode, period, newBar); break;
	default:
		break;
	}
}

void WtBtRunner::ctx_on_calc(uint32_t id, uint32_t curDate, uint32_t curTime, EngineType eType /* = ET_CTA */)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_calc) _cb_cta_calc(id, curDate, curTime); break;
	case ET_SEL: if (_cb_sel_calc) _cb_sel_calc(id, curDate, curTime); break;
	default:
		break;
	}
}

void WtBtRunner::ctx_on_calc_done(uint32_t id, uint32_t curDate, uint32_t curTime, EngineType eType /* = ET_CTA */)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_calc_done) _cb_cta_calc_done(id, curDate, curTime); break;
	case ET_SEL: if (_cb_sel_calc_done) _cb_sel_calc_done(id, curDate, curTime); break;
	default:
		break;
	}
}

void WtBtRunner::ctx_on_init(uint32_t id, EngineType eType/*= ET_CTA*/)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_init) _cb_cta_init(id); break;
	case ET_HFT: if (_cb_hft_init) _cb_hft_init(id); break;
	case ET_SEL: if (_cb_sel_init) _cb_sel_init(id); break;
	default:
		break;
	}
}

void WtBtRunner::ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType /* = ET_CTA */)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_cond_trigger) _cb_cta_cond_trigger(id, stdCode, target, price, usertag); break;
	default:
		break;
	}
}

void WtBtRunner::ctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin /* = true */, EngineType eType /* = ET_CTA */)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_sessevt) _cb_cta_sessevt(id, curTDate, isBegin); break;
	case ET_HFT: if (_cb_hft_sessevt) _cb_hft_sessevt(id, curTDate, isBegin); break;
	case ET_SEL: if (_cb_sel_sessevt) _cb_sel_sessevt(id, curTDate, isBegin); break;
	default:
		break;
	}
}

void WtBtRunner::ctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType/*= ET_CTA*/)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_tick) _cb_cta_tick(id, stdCode, &newTick->getTickStruct()); break;
	case ET_HFT: if (_cb_hft_tick) _cb_hft_tick(id, stdCode, &newTick->getTickStruct()); break;
	case ET_SEL: if (_cb_sel_tick) _cb_sel_tick(id, stdCode, &newTick->getTickStruct()); break;
	default:
		break;
	}
}

void WtBtRunner::hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue)
{
	if (_cb_hft_ordque)
		_cb_hft_ordque(id, stdCode, &newOrdQue->getOrdQueStruct());
}

void WtBtRunner::hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	if (_cb_hft_orddtl)
		_cb_hft_orddtl(id, stdCode, &newOrdDtl->getOrdDtlStruct());
}

void WtBtRunner::hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTrans)
{
	if (_cb_hft_trans)
		_cb_hft_trans(id, stdCode, &newTrans->getTransStruct());
}

void WtBtRunner::hft_on_channel_ready(uint32_t cHandle, const char* trader)
{
	if (_cb_hft_chnl)
		_cb_hft_chnl(cHandle, trader, 1000/*CHNL_EVENT_READY*/);
}

void WtBtRunner::hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)
{
	if (_cb_hft_entrust)
		_cb_hft_entrust(cHandle, localid, stdCode, bSuccess, message, userTag);
}

void WtBtRunner::hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	if (_cb_hft_ord)
		_cb_hft_ord(cHandle, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);
}

void WtBtRunner::hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)
{
	if (_cb_hft_trd)
		_cb_hft_trd(cHandle, localid, stdCode, isBuy, vol, price, userTag);
}

void WtBtRunner::init(const char* logProfile /* = "" */, bool isFile /* = true */, const char* outDir/* = "./outputs_bt"*/)
{
#ifdef _MSC_VER
	CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str());
#endif

	WTSLogger::init(logProfile, isFile);

	WtHelper::setInstDir(getBinDir());
	WtHelper::setOutputDir(outDir);
}

void WtBtRunner::config(const char* cfgFile, bool isFile /* = true */)
{
	std::string error;
	if (!config_v2(cfgFile, isFile, &error) && !error.empty())
		WTSLogger::error("Backtest configuration failed: {}", error);
}

bool WtBtRunner::config_v2(const char* cfgFile, bool isFile, std::string* error)
{
	std::unique_lock<std::mutex> controlLock(_lifecycle_control_mutex);
	if (error)
		error->clear();
	if (_released)
	{
		if (error) *error = "WtBtEngine has been released";
		return false;
	}
	if(_inited)
	{
		if (error) *error = "WtBtEngine has already been initialized";
		return false;
	}
	if (_config_failed_permanently)
	{
		if (error) *error = "a previous configuration attempt initialized the replayer and failed; retry is unsupported";
		return false;
	}
	if (!cfgFile || cfgFile[0] == '\0')
	{
		if (error) *error = "configuration path or content is empty";
		return false;
	}

	_cfg = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);
	if(_cfg == NULL)
	{
		if (error) *error = "loading config failed";
		return false;
	}
	auto fail_preflight = [this, error](const std::string& message) {
		if (error) *error = message;
		_cfg->release();
		_cfg = NULL;
		return false;
	};

	// Validate the configuration boundary before initializing the replayer. Its
	// init() mutates replay state and may load base files, so malformed mocker
	// configuration must not leave a partially initialized runner behind.
	WTSVariant* cfgReplayer = _cfg->get("replayer");
	if (!cfgReplayer || !cfgReplayer->isObject())
		return fail_preflight("replayer must be an object");

	WTSVariant* cfgEnv = _cfg->get("env");
	if (!cfgEnv || !cfgEnv->isObject())
		return fail_preflight("env must be an object");

	WTSVariant* cfgMockerMode = cfgEnv->get("mocker");
	if (!cfgMockerMode || cfgMockerMode->type() != WTSVariant::VT_String)
		return fail_preflight("env.mocker must be a string");
	const char* mode = cfgMockerMode->asCString();
	if (!mode || mode[0] == '\0')
		return fail_preflight("env.mocker is missing");

	const bool noMockerMode = strcmp(mode, "none") == 0;
	if (!noMockerMode && strcmp(mode, "cta") != 0 && strcmp(mode, "hft") != 0 &&
		strcmp(mode, "sel") != 0 && strcmp(mode, "exec") != 0)
		return fail_preflight(std::string("unsupported backtest mocker mode: ") + mode);

	WTSVariant* cfgMode = _cfg->get(mode);
	if (!noMockerMode && (!cfgMode || !cfgMode->isObject()))
		return fail_preflight(std::string(mode) + " mode configuration is missing or invalid");
	const char* mockerName = cfgMode ? cfgMode->getCString("name") : NULL;
	if (!noMockerMode && (!mockerName || mockerName[0] == '\0'))
		return fail_preflight(std::string(mode) + " mode name is missing");

	if (!_replayer.init(cfgReplayer, &_notifier, _ext_fnl_bar_loader != NULL ? this : NULL))
	{
		if (error) *error = "initializing historical data replayer failed";
		_cfg->release();
		_cfg = NULL;
		return false;
	}

	// Replayer initialization is retryable on failure. Initialize the notifier
	// only after it succeeds, so a failed attempt cannot create a server that a
	// corrected retry would overwrite in EventNotifier.
	initEvtNotifier(_cfg->get("notifier"));

	auto fail_mode_initialization = [this, error](const std::string& message) {
		if (error) *error = message;
		_config_failed_permanently = true;
		// An ExecMocker that could not be quiesced still owns the plugin unit,
		// factory and module. Keep its configuration/context alive for retry.
		if (_cfg && !_exec_mocker)
		{
			_cfg->release();
			_cfg = NULL;
		}
		return false;
	};
	if (strcmp(mode, "cta") == 0 && cfgMode)
	{
		int32_t slippage = cfgMode->getInt32("slippage");
		ExpCtaMocker* candidate = new ExpCtaMocker(&_replayer, mockerName, slippage, true, &_notifier);
		if (!candidate->init_cta_factory(cfgMode))
		{
			delete candidate;
			return fail_mode_initialization("initializing CTA strategy factory failed");
		}
		_cta_mocker = candidate;
		_replayer.register_sink(_cta_mocker, mockerName);
	}
	else if (strcmp(mode, "hft") == 0 && cfgMode)
	{
		ExpHftMocker* candidate = new ExpHftMocker(&_replayer, mockerName);
		if (!candidate->init_hft_factory(cfgMode))
		{
			delete candidate;
			return fail_mode_initialization("initializing HFT strategy factory failed");
		}
		_hft_mocker = candidate;
		_replayer.register_sink(_hft_mocker, mockerName);
	}
	else if (strcmp(mode, "sel") == 0 && cfgMode)
	{
		WTSVariant* cfgTask = cfgMode->get("task");
		uint32_t taskDate = 0;
		uint32_t taskTime = 0;
		const char* taskPeriod = nullptr;
		const char* taskTradingTemplate = nullptr;
		const char* taskSession = nullptr;
		if (cfgTask)
		{
			taskDate = cfgTask->getUInt32("date");
			taskTime = cfgTask->getUInt32("time");
			taskPeriod = cfgTask->getCString("period");
			taskTradingTemplate = cfgTask->getCString("trdtpl");
			taskSession = cfgTask->getCString("session");
			if (!HisDataReplayer::validate_task_parameters(taskPeriod, taskTradingTemplate, taskSession))
				return fail_mode_initialization("invalid SEL task parameters");
		}

		int32_t slippage = cfgMode->getInt32("slippage");
		ExpSelMocker* candidate = new ExpSelMocker(&_replayer, mockerName, slippage);
		if (!candidate->init_sel_factory(cfgMode))
		{
			delete candidate;
			return fail_mode_initialization("initializing SEL strategy factory failed");
		}
		_sel_mocker = candidate;
		_replayer.register_sink(_sel_mocker, mockerName);

		if (cfgTask)
			_replayer.register_task(_sel_mocker->id(), taskDate, taskTime, taskPeriod, taskTradingTemplate, taskSession);
	}
	else if (strcmp(mode, "exec") == 0)
	{
		std::string initError;
		bool initSucceeded = false;
		{
			std::lock_guard<std::mutex> execLock(_exec_mocker_lifecycle_mutex);
			_exec_mocker = new ExecMocker(&_replayer);
			initSucceeded = _exec_mocker->init(cfgMode, &initError);
		}
		if (!initSucceeded)
		{
			std::string quiesceError;
			if (!quiesceAndDestroyExecMocker(&quiesceError) && !quiesceError.empty())
			{
				initError += "; plugin cleanup is pending: " + quiesceError;
			}
			return fail_mode_initialization(initError.empty() ? "initializing execution mocker failed" : initError);
		}
		_replayer.register_sink(_exec_mocker, mockerName);
	}
	else if (!noMockerMode && strcmp(mode, "cta") != 0 && strcmp(mode, "hft") != 0 && strcmp(mode, "sel") != 0)
	{
		if (error) *error = std::string("unsupported backtest mocker mode: ") + mode;
		_config_failed_permanently = true;
		_cfg->release();
		_cfg = NULL;
		return false;
	}

	_inited = true;
	return true;
}

void WtBtRunner::run(bool bNeedDump /* = false */, bool bAsync /* = false */)
{
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (_running || _released || _release_requested)
			return;
	}

	// Do not wait behind prepare(), a current run, stop(), or release(). A
	// concurrent run request must be rejected rather than replayed afterward.
	std::unique_lock<std::mutex> controlLock(_lifecycle_control_mutex, std::try_to_lock);
	if (!controlLock.owns_lock())
		return;
	if (!_inited)
	{
		WTSLogger::error("Backtesting cannot run before successful configuration");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (_running || _released || _release_requested)
			return;
	}

	// A false _running state means the previous worker has left replay. Join
	// only that completed worker before replacing its thread handle.
	if (_worker)
	{
		if (_worker->get_id() == std::this_thread::get_id())
		{
			WTSLogger::error("Backtest worker cannot join itself");
			return;
		}
		if (!canStopExecutionMocker())
		{
			WTSLogger::error("A completed backtest worker cannot be joined from this control thread");
			return;
		}
		if (_worker->joinable())
			_worker->join();
		_worker.reset();
	}

	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (_running || _released || _release_requested)
			return;
		// Mark the run active before prepare()/worker launch. This makes an
		// immediate stop visible even while the worker has not started yet.
		_running = true;
		_stop_requested = false;
		_replay_thread_id = std::this_thread::get_id();
	}

	const bool previousAsync = _async;
	_async = bAsync;

	WTSLogger::info("Backtesting will run in {} mode", _async ? "async" : "sync");

	bool prepared = false;
	try
	{
		prepared = _replayer.prepare();
	}
	catch (...)
	{
		WTSLogger::error("Preparing historical data replayer raised an exception");
	}
	if (!prepared)
	{
		_async = previousAsync;
		finishReplayRun();
		controlLock.unlock();
		finalizeDeferredReleaseAtSyncBoundary();
		WTSLogger::error("Preparing historical data replayer failed; backtest will not run");
		return;
	}

	// stop() can arrive after the runner is marked active but before the
	// replayer's prepare() publishes its own running state. Preserve that request
	// and forward it once prepare is complete, before an async worker can replay.
	bool stopRequested = false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		stopRequested = _stop_requested;
	}
	if (stopRequested)
		_replayer.stop();

	if (_cta_mocker)
		_cta_mocker->enable_hook(_async);
	else if (_hft_mocker)
		_hft_mocker->enable_hook(_async);

	if (!bAsync)
	{
		controlLock.unlock();
		try
		{
			_replayer.run(bNeedDump);
		}
		catch (...)
		{
			WTSLogger::error("Exception raised while synchronous backtest was running");
		}
		finishReplayRun();
		finalizeDeferredReleaseAtSyncBoundary();
	}
	else
	{
		{
			std::lock_guard<std::mutex> lock(_lifecycle_mutex);
			_replay_thread_id = std::thread::id();
		}
		try
		{
			_worker.reset(new StdThread([this, bNeedDump]() {
				{
					std::lock_guard<std::mutex> lock(_lifecycle_mutex);
					_replay_thread_id = std::this_thread::get_id();
				}
				try
				{
					_replayer.run(bNeedDump);
				}
				catch (...)
				{
					WTSLogger::error("Exception raised while worker running");
				}
				WTSLogger::debug("Worker thread of backtest finished");
				finishReplayRun();
			}));
		}
		catch (...)
		{
			WTSLogger::error("Creating asynchronous backtest worker failed; terminating replay");
			{
				std::lock_guard<std::mutex> lock(_lifecycle_mutex);
				_replay_thread_id = std::this_thread::get_id();
			}
			_async = false;
			controlLock.unlock();
			try
			{
				_replayer.report_replay_error(WT_REPLAY_ERROR_INTERNAL, "Creating asynchronous backtest worker failed");
				_replayer.run(bNeedDump);
			}
			catch (...)
			{
				WTSLogger::error("Exception raised while terminating failed backtest");
			}
			finishReplayRun();
			finalizeDeferredReleaseAtSyncBoundary();
			return;
		}
		controlLock.unlock();
	}
}

void WtBtRunner::stop()
{
	if (!canStopExecutionMocker() || isReplayThread())
	{
		requestReplayStop(false);
		WTSLogger::debug("Backtest stop deferred from a callback/replay thread");
		return;
	}

	// Register the stop before waiting for lifecycle control. run() holds that
	// lock while prepare() invokes strategy callbacks, so waiting first can make
	// an external stop miss the window before an asynchronous worker starts.
	requestReplayStop(false);

	std::unique_lock<std::mutex> controlLock(_lifecycle_control_mutex);
	if (!canStopExecutionMocker())
	{
		requestReplayStop(false);
		WTSLogger::debug("Backtest stop deferred because execution plugin preflight failed");
		return;
	}
	bool wasRunning = false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		wasRunning = _running;
	}
	requestReplayStop(false);
	if (!wasRunning)
	{
		waitForReplayRun();
		return;
	}

	WTSLogger::debug("Notify to finish last round");

	if (_cta_mocker)
		_cta_mocker->step_calc();

	if (_hft_mocker)
		_hft_mocker->step_tick();

	WTSLogger::debug("Last round ended");

	waitForReplayRun();

	WTSLogger::freeAllDynLoggers();

	WTSLogger::debug("Backtest stopped");
}

void WtBtRunner::release()
{
	if (!canStopExecutionMocker() || isReplayThread())
	{
		requestReplayStop(true);
		WTSLogger::debug("Backtest release deferred from a callback/replay thread");
		return;
	}

	std::unique_lock<std::mutex> controlLock(_lifecycle_control_mutex);
	if (_released)
		return;
	if (!canStopExecutionMocker())
	{
		requestReplayStop(true);
		WTSLogger::error("Backtest release deferred because execution plugin preflight failed; retry on the control thread");
		return;
	}

	requestReplayStop(true);
	waitForReplayRun();
	std::string error;
	if (!finalizeRelease(&error))
	{
		WTSLogger::error("Backtest release is pending; runner, execution plugin and replay context are retained: {}", error);
		return;
	}
	WTSLogger::stop();
}

bool WtBtRunner::isReplayThread() const
{
	std::lock_guard<std::mutex> lock(_lifecycle_mutex);
	return _running && _replay_thread_id == std::this_thread::get_id();
}

void WtBtRunner::requestReplayStop(bool releaseRequested)
{
	bool running = false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (releaseRequested)
			_release_requested = true;
		running = _running;
		if (running)
			_stop_requested = true;
	}
	if (running)
		_replayer.stop();
}

void WtBtRunner::finishReplayRun()
{
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_running = false;
		_stop_requested = false;
		_replay_thread_id = std::thread::id();
	}
	_lifecycle_changed.notify_all();
}

void WtBtRunner::waitForReplayRun()
{
	if (_worker)
	{
		if (_worker->get_id() == std::this_thread::get_id())
		{
			WTSLogger::error("Backtest worker cannot join itself; release remains pending");
			return;
		}
		if (_worker->joinable())
			_worker->join();
		_worker.reset();
		return;
	}

	std::unique_lock<std::mutex> lock(_lifecycle_mutex);
	if (_replay_thread_id == std::this_thread::get_id())
		return;
	_lifecycle_changed.wait(lock, [this]() { return !_running; });
}

bool WtBtRunner::canStopExecutionMocker()
{
	std::unique_lock<std::mutex> execLock(_exec_mocker_lifecycle_mutex, std::try_to_lock);
	if (!execLock.owns_lock())
		return false;
	return !_exec_mocker || _exec_mocker->canStopFromCurrentThread();
}

bool WtBtRunner::quiesceAndDestroyExecMocker(std::string* error)
{
	std::lock_guard<std::mutex> execLock(_exec_mocker_lifecycle_mutex);
	if (!_exec_mocker)
		return true;
	if (!_exec_mocker->canStopFromCurrentThread())
	{
		if (error) *error = "execution plugin preflight rejected the current control thread";
		return false;
	}
	if (!_exec_mocker->requestQuiesce(error) ||
		!_exec_mocker->canWaitQuiesce(error) ||
		!_exec_mocker->waitQuiesce(error))
		return false;
	delete _exec_mocker;
	_exec_mocker = NULL;
	return true;
}

bool WtBtRunner::finalizeRelease(std::string* error)
{
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		if (_running)
		{
			if (error) *error = "historical replay has not finished";
			return false;
		}
	}

	if (_exec_mocker)
	{
		if (!quiesceAndDestroyExecMocker(error))
			return false;
	}

	if (_cfg)
	{
		_cfg->release();
		_cfg = NULL;
	}
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		_released = true;
	}
	return true;
}

void WtBtRunner::finalizeDeferredReleaseAtSyncBoundary()
{
	bool requested = false;
	{
		std::lock_guard<std::mutex> lock(_lifecycle_mutex);
		// This helper is reached only by the run/control thread at a safe
		// boundary; the asynchronous worker never calls it.
		requested = _release_requested && !_running;
	}
	if (!requested)
		return;

	std::unique_lock<std::mutex> controlLock(_lifecycle_control_mutex, std::try_to_lock);
	if (!controlLock.owns_lock())
		return;
	std::string error;
	if (!finalizeRelease(&error))
		WTSLogger::error("Deferred synchronous backtest release remains pending: {}", error);
	else
		WTSLogger::stop();
}

void WtBtRunner::set_time_range(WtUInt64 stime, WtUInt64 etime)
{
	_replayer.set_time_range(stime, etime);

	WTSLogger::info("Backtest time range is set to be [{},{}] mannually", stime, etime);
}

void WtBtRunner::enable_tick(bool bEnabled /* = true */)
{
	_replayer.enable_tick(bEnabled);

	WTSLogger::info("Tick data replaying is {}", bEnabled ? "enabled" : "disabled");
}

void WtBtRunner::clear_cache()
{
	_replayer.clear_cache();
}

const char* WtBtRunner::get_raw_stdcode(const char* stdCode)
{
	static thread_local std::string s;
	s = _replayer.get_rawcode(stdCode);
	return s.c_str();
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

bool WtBtRunner::initEvtNotifier(WTSVariant* cfg)
{
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}
