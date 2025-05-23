/*!
 * \file WtBtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测引擎实现文件
 * \details 该文件实现了WtBtRunner类，提供完整的CTA、SEL及HFT策略回测框架支持
 *          实现历史数据加载、模拟器初始化、回调注册、回测流程控制等核心功能
 */
#include "WtBtRunner.h"
#include "ExpCtaMocker.h"
#include "ExpSelMocker.h"
#include "ExpHftMocker.h"

#include <iomanip>

#include "../WtBtCore/ExecMocker.h"
#include "../WtBtCore/WtHelper.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/ModuleHelper.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/SignalHook.hpp"

#ifdef _MSC_VER
#include "../Common/mdump.h"
#include <boost/filesystem.hpp>
 //这个主要是给MiniDumper用的
const char* getModuleName()
{
	static char MODULE_NAME[250] = { 0 };
	if (strlen(MODULE_NAME) == 0)
	{
		GetModuleFileName(g_dllModule, MODULE_NAME, 250);
		boost::filesystem::path p(MODULE_NAME);
		strcpy(MODULE_NAME, p.filename().string().c_str());
	}

	return MODULE_NAME;
}
#endif

/**
 * @brief 构造函数
 * @details 初始化回测引擎对象，将所有成员变量初始化为默认值
 *          包括各类模拟器、回调函数指针、数据加载器和状态标志
 *          并安装信号处理钩子用于捕获异常
 */
WtBtRunner::WtBtRunner()
	: _cta_mocker(NULL)
	, _sel_mocker(NULL)

	, _cb_cta_init(NULL)
	, _cb_cta_tick(NULL)
	, _cb_cta_calc(NULL)
	, _cb_cta_calc_done(NULL)
	, _cb_cta_bar(NULL)
	, _cb_cta_sessevt(NULL)
	, _cb_cta_cond_trigger(NULL)

	, _cb_sel_init(NULL)
	, _cb_sel_tick(NULL)
	, _cb_sel_calc(NULL)
	, _cb_sel_calc_done(NULL)
	, _cb_sel_bar(NULL)
	, _cb_sel_sessevt(NULL)

	, _cb_hft_init(NULL)
	, _cb_hft_tick(NULL)
	, _cb_hft_bar(NULL)
	, _cb_hft_ord(NULL)
	, _cb_hft_trd(NULL)
	, _cb_hft_entrust(NULL)
	, _cb_hft_chnl(NULL)

	, _cb_hft_orddtl(NULL)
	, _cb_hft_ordque(NULL)
	, _cb_hft_trans(NULL)

	, _cb_hft_sessevt(NULL)

	, _ext_fnl_bar_loader(NULL)
	, _ext_raw_bar_loader(NULL)
	, _ext_adj_fct_loader(NULL)
	, _ext_tick_loader(NULL)

	, _inited(false)
	, _running(false)
	, _async(false)
{
	install_signal_hooks([](const char* message) {
		WTSLogger::error(message);
	});
}


/**
 * @brief 析构函数
 * @details 释放回测引擎对象占用的资源
 *          注意实际的资源释放在release方法中完成
 */
WtBtRunner::~WtBtRunner()
{
}

/**
 * @brief 加载原始历史K线数据
 * @details 使用外部注册的原始数据加载器来加载指定品种、指定周期的原始历史K线数据
 * 
 * @param obj 数据接收对象
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param cb 读取K线数据的回调函数
 * 
 * @return bool 是否成功加载数据
 */
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

/**
 * @brief 加载处理过的历史K线数据
 * @details 使用外部注册的最终数据加载器来加载指定品种、指定周期的已处理的历史K线数据
 *          与原始数据不同，最终数据可能已经过滤、调整或其他处理
 * 
 * @param obj 数据接收对象
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param cb 读取K线数据的回调函数
 * 
 * @return bool 是否成功加载数据
 */
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

/**
 * @brief 加载所有除权因子
 * @details 使用外部注册的除权因子加载器来加载所有品种的除权因子
 *          除权因子用于处理除权日前后的数据连续性
 * 
 * @param obj 数据接收对象
 * @param cb 读取除权因子的回调函数
 * 
 * @return bool 是否成功加载数据
 */
bool WtBtRunner::loadAllAdjFactors(void* obj, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader("");
}

/**
 * @brief 加载指定品种的除权因子
 * @details 使用外部注册的除权因子加载器来加载指定品种的除权因子
 *          与loadAllAdjFactors不同，本方法只加载指定品种的除权因子
 * 
 * @param obj 数据接收对象
 * @param stdCode 标准化合约代码
 * @param cb 读取除权因子的回调函数
 * 
 * @return bool 是否成功加载数据
 */
bool WtBtRunner::loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader(stdCode);
}

/**
 * @brief 加载原始历史造市数据
 * @details 使用外部注册的tick数据加载器来加载指定品种、指定日期的原始历史tick数据
 * 
 * @param obj 数据接收对象
 * @param stdCode 标准化合约代码
 * @param uDate 日期，格式YYYYMMDD
 * @param cb 读取tick数据的回调函数
 * 
 * @return bool 是否成功加载数据
 */
bool WtBtRunner::loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_tick_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_ticks = cb;

	return _ext_tick_loader(stdCode, uDate);
}

/**
 * @brief 向回测引擎输入原始K线数据
 * @details 将加载的原始K线数据通过回调函数输入到回测引擎中
 *          该方法需要在外部数据加载器装在数据后调用
 * 
 * @param bars K线数据结构数组
 * @param count 数据数量
 */
void WtBtRunner::feedRawBars(WTSBarStruct* bars, uint32_t count)
{
	if(_ext_fnl_bar_loader == NULL && _ext_raw_bar_loader == NULL)
	{
		WTSLogger::error("Cannot feed bars because of no extented bar loader registered.");
		return;
	}

	_feeder_bars(_feed_obj, bars, count);
}

/**
 * @brief 向回测引擎输入除权因子数据
 * @details 将加载的除权因子数据通过回调函数输入到回测引擎中
 *          除权因子用于处理除权日前后的数据连续性
 * 
 * @param stdCode 标准化合约代码
 * @param dates 日期数组，格式YYYYMMDD
 * @param factors 对应的除权因子数组
 * @param count 数据数量
 */
void WtBtRunner::feedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
{
	if(_ext_adj_fct_loader == NULL)
	{
		WTSLogger::error("Cannot feed adjusting factors because of no extented adjusting factor loader registered.");
		return;
	}

	_feeder_fcts(_feed_obj, stdCode, dates, factors, count);
}

/**
 * @brief 向回测引擎输入原始tick数据
 * @details 将加载的原始tick数据通过回调函数输入到回测引擎中
 *          需要在外部数据加载器加载数据后调用
 * 
 * @param ticks tick数据结构数组
 * @param count 数据数量
 */
void WtBtRunner::feedRawTicks(WTSTickStruct* ticks, uint32_t count)
{
	if (_ext_tick_loader == NULL)
	{
		WTSLogger::error("Cannot feed ticks because of no extented tick loader registered.");
		return;
	}

	_feeder_ticks(_feed_obj, ticks, count);
}

/**
 * @brief 注册CTA策略引擎的回调函数
 * @details 注册后的回调函数将在相应事件发生时被引擎调用
 *          这些回调用于通知策略模块各类事件的发生
 *
 * @param cbInit 策略初始化回调
 * @param cbTick 市场实时行情推送回调
 * @param cbCalc 计算需要回调
 * @param cbBar K线周期数据推送回调
 * @param cbSessEvt 交易时段事件回调
 * @param cbCalcDone 计算完成回调
 * @param cbCondTrigger 条件触发回调
 */
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

/**
 * @brief 注册SEL选股策略引擎的回调函数
 * @details 注册后的回调函数将在相应事件发生时被引擎调用
 *          这些回调用于通知选股策略模块各类事件的发生
 * 
 * @param cbInit 策略初始化回调
 * @param cbTick 市场实时行情推送回调
 * @param cbCalc 计算需要回调
 * @param cbBar K线周期数据推送回调
 * @param cbSessEvt 交易时段事件回调
 * @param cbCalcDone 计算完成回调
 */
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

/**
 * @brief 注册HFT高频策略引擎的回调函数
 * @details 注册后的回调函数将在相应事件发生时被引擎调用
 *          这些回调用于通知高频策略模块各类市场数据和交易事件的发生
 * 
 * @param cbInit 策略初始化回调
 * @param cbTick 市场实时行情推送回调
 * @param cbBar K线周期数据推送回调
 * @param cbChnl 交易通道就绪事件回调
 * @param cbOrd 委托回报事件回调
 * @param cbTrd 成交回报事件回调
 * @param cbEntrust 委托宣告事件回调
 * @param cbOrdDtl 逆序委托明细事件回调
 * @param cbOrdQue 委托队列事件回调
 * @param cbTrans 逆序成交明细事件回调
 * @param cbSessEvt 交易时段事件回调
 */
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

/**
 * @brief 初始化CTA策略模拟器
 * @details 新建并初始化一个ExpCtaMocker对象，用于模拟和执行CTA策略
 *          如果已存在模拟器，则先删除原模拟器
 * 
 * @param name 策略名称
 * @param slippage 滑点设置，默认为0
 * @param hook 是否安装钩子函数，用于手动给模拟器发信号
 * @param persistData 是否持久化数据
 * @param bIncremental 是否加载增量数据
 * @param isRatioSlp 滑点是否为比例滑点
 * 
 * @return uint32_t 模拟器ID
 */
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

/**
 * @brief 初始化HFT高频策略模拟器
 * @details 新建并初始化一个ExpHftMocker对象，用于模拟和执行高频策略
 *          如果已存在模拟器，则先删除原模拟器
 * 
 * @param name 策略名称
 * @param hook 是否安装钩子函数，用于手动给模拟器发信号
 * 
 * @return uint32_t 模拟器ID
 */
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

/**
 * @brief 初始化SEL选股策略模拟器
 * @details 新建并初始化一个ExpSelMocker对象，用于模拟和执行选股策略
 *          如果已存在模拟器，则先删除原模拟器
 *          并且在回放器中注册定时任务
 * 
 * @param name 策略名称
 * @param date 任务日期，格式YYYYMMDD
 * @param time 任务时间，格式HHMMSS
 * @param period 任务周期
 * @param trdtpl 交易模板，默认为"CHINA"
 * @param session 交易时段，默认为"TRADING"
 * @param slippage 滑点设置，默认为0
 * @param isRatioSlp 滑点是否为比例滑点
 * 
 * @return uint32_t 模拟器ID
 */
uint32_t WtBtRunner::initSelMocker(const char* name, uint32_t date, uint32_t time, const char* period, 
	const char* trdtpl /* = "CHINA" */, const char* session /* = "TRADING" */, int32_t slippage /* = 0 */, bool isRatioSlp /* = false */)
{
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

/**
 * @brief K线周期数据回调函数
 * @details 根据引擎类型调用相应的K线数据回调函数
 *          当新的K线数据到达时触发此回调
 * 
 * @param id 策略ID
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param newBar 新到的K线数据
 * @param eType 引擎类型，默认为CTA引擎
 */
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

/**
 * @brief 策略周期性计算回调函数
 * @details 根据引擎类型调用相应的计算回调函数
 *          当到达计算时间点时触发此回调
 * 
 * @param id 策略ID
 * @param curDate 当前日期，格式YYYYMMDD
 * @param curTime 当前时间，格式HHMMSS
 * @param eType 引擎类型，默认为CTA引擎
 */
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

/**
 * @brief 策略周期性计算完成回调函数
 * @details 根据引擎类型调用相应的计算完成回调函数
 *          当策略周期性计算执行完成后触发此回调
 *          与普通计算回调不同，此回调在所有计算都完成后触发
 * 
 * @param id 策略ID
 * @param curDate 当前日期，格式YYYYMMDD
 * @param curTime 当前时间，格式HHMMSS
 * @param eType 引擎类型，默认为CTA引擎
 */
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

/**
 * @brief 策略初始化回调函数
 * @details 根据引擎类型调用相应的策略初始化回调函数
 *          当策略引擎初始化完成时触发此回调
 *          一般在回测开始前调用，用于策略初始化
 * 
 * @param id 策略ID
 * @param eType 引擎类型，默认为CTA引擎
 */
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

/**
 * @brief 条件触发回调函数
 * @details 当指定条件被触发时调用相应的回调函数
 *          目前只在CTA引擎中支持条件触发
 *          条件触发通常用于价格到达特定条件时的信号通知
 * 
 * @param id 策略ID
 * @param stdCode 标准化合约代码
 * @param target 目标价格
 * @param price 触发时的实际价格
 * @param usertag 用户自定义标签
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtBtRunner::ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType /* = ET_CTA */)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_cond_trigger) _cb_cta_cond_trigger(id, stdCode, target, price, usertag); break;
	default:
		break;
	}
}

/**
 * @brief 交易时段事件回调函数
 * @details 根据引擎类型调用相应的交易时段事件回调函数
 *          当交易时段开始或结束时触发此回调
 *          用于通知策略交易日切换或新交易日开始
 * 
 * @param id 策略ID
 * @param curTDate 当前交易日，格式YYYYMMDD
 * @param isBegin 是否是交易时段开始，true为开始，false为结束
 * @param eType 引擎类型，默认为CTA引擎
 */
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

/**
 * @brief 实时行情tick数据回调函数
 * @details 根据引擎类型调用相应的tick数据回调函数
 *          当收到新的实时行情数据时触发此回调
 *          tick数据包含当前合约的最新价格、成交量等信息
 * 
 * @param id 策略ID
 * @param stdCode 标准化合约代码
 * @param newTick 新的tick数据
 * @param eType 引擎类型，默认为CTA引擎
 */
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

/**
 * @brief 高频策略委托队列数据回调函数
 * @details 当接收到新的委托队列数据时触发此回调
 *          委托队列数据包含了各个价位的委托数量信息
 *          仅在HFT高频模式下有效
 * 
 * @param id 策略ID
 * @param stdCode 标准化合约代码
 * @param newOrdQue 新的委托队列数据
 */
void WtBtRunner::hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue)
{
	if (_cb_hft_ordque)
		_cb_hft_ordque(id, stdCode, &newOrdQue->getOrdQueStruct());
}

/**
 * @brief 高频策略委托明细数据回调函数
 * @details 当接收到新的委托明细数据时触发此回调
 *          委托明细数据包含了逻序委托的详细信息
 *          仅在HFT高频模式下有效
 * 
 * @param id 策略ID
 * @param stdCode 标准化合约代码
 * @param newOrdDtl 新的委托明细数据
 */
void WtBtRunner::hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	if (_cb_hft_orddtl)
		_cb_hft_orddtl(id, stdCode, &newOrdDtl->getOrdDtlStruct());
}

/**
 * @brief 高频策略逆序成交明细数据回调函数
 * @details 当接收到新的逆序成交明细数据时触发此回调
 *          逆序成交明细数据包含了逻序成交的详细信息
 *          仅在HFT高频模式下有效
 * 
 * @param id 策略ID
 * @param stdCode 标准化合约代码
 * @param newTrans 新的逆序成交明细数据
 */
void WtBtRunner::hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTrans)
{
	if (_cb_hft_trans)
		_cb_hft_trans(id, stdCode, &newTrans->getTransStruct());
}

/**
 * @brief 高频策略交易通道就绪回调函数
 * @details 当交易通道就绪时触发此回调
 *          交易通道就绪意味着可以进行交易操作
 *          仅在HFT高频模式下有效
 * 
 * @param cHandle 交易通道句柄
 * @param trader 交易器ID
 */
void WtBtRunner::hft_on_channel_ready(uint32_t cHandle, const char* trader)
{
	if (_cb_hft_chnl)
		_cb_hft_chnl(cHandle, trader, 1000/*CHNL_EVENT_READY*/);
}

/**
 * @brief 高频策略委托宣告回调函数
 * @details 当发布委托宣告后收到回报时触发此回调
 *          主要用于通知委托下单是否成功发送到交易所
 *          仅在HFT高频模式下有效
 * 
 * @param cHandle 交易通道句柄
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 委托是否成功
 * @param message 错误消息，如果有的话
 * @param userTag 用户自定义标签
 */
void WtBtRunner::hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)
{
	if (_cb_hft_entrust)
		_cb_hft_entrust(cHandle, localid, stdCode, bSuccess, message, userTag);
}

/**
 * @brief 高频策略委托回报回调函数
 * @details 当收到委托状态变化回报时触发此回调
 *          用于通知委托状态变化，如部分成交、全部成交或取消
 *          仅在HFT高频模式下有效
 * 
 * @param cHandle 交易通道句柄
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入方向
 * @param totalQty 委托总数量
 * @param leftQty 剩余数量
 * @param price 委托价格
 * @param isCanceled 是否已取消
 * @param userTag 用户自定义标签
 */
void WtBtRunner::hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	if (_cb_hft_ord)
		_cb_hft_ord(cHandle, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);
}

/**
 * @brief 高频策略成交回报回调函数
 * @details 当收到成交回报时触发此回调
 *          用于通知委托已成交的信息
 *          仅在HFT高频模式下有效
 * 
 * @param cHandle 交易通道句柄
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入方向
 * @param vol 成交数量
 * @param price 成交价格
 * @param userTag 用户自定义标签
 */
void WtBtRunner::hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)
{
	if (_cb_hft_trd)
		_cb_hft_trd(cHandle, localid, stdCode, isBuy, vol, price, userTag);
}

/**
 * @brief 初始化回测引擎
 * @details 初始化日志模块并设置输出目录
 *          在Windows平台上还会启用崩溃转储功能
 * 
 * @param logProfile 日志配置文件路径或内容
 * @param isFile 是否为文件路径，如果为false则logProfile为内容
 * @param outDir 输出目录，默认为"./outputs_bt"
 */
void WtBtRunner::init(const char* logProfile /* = "" */, bool isFile /* = true */, const char* outDir/* = "./outputs_bt"*/)
{
#ifdef _MSC_VER
	CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str());
#endif

	WTSLogger::init(logProfile, isFile);

	WtHelper::setInstDir(getBinDir());
	WtHelper::setOutputDir(outDir);
}

/**
 * @brief 设置回测引擎配置
 * @details 从配置文件或内容中加载回测引擎配置
 *          并根据指定的模式初始化相应的模拟器
 *          支持CTA、HFT、SEL和EXEC四种模式
 * 
 * @param cfgFile 配置文件路径或内容
 * @param isFile 是否为文件路径，如果为false则cfgFile为内容
 */
void WtBtRunner::config(const char* cfgFile, bool isFile /* = true */)
{
	if(_inited)
	{
		WTSLogger::error("WtBtEngine has already been inited");
		return;
	}

	_cfg = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);
	if(_cfg == NULL)
	{
		WTSLogger::error("Loading config failed");
		return;
	}

	//初始化事件推送器
	initEvtNotifier(_cfg->get("notifier"));

	_replayer.init(_cfg->get("replayer"), &_notifier, _ext_fnl_bar_loader != NULL ? this : NULL);

	WTSVariant* cfgEnv = _cfg->get("env");
	const char* mode = cfgEnv->getCString("mocker");
	WTSVariant* cfgMode = _cfg->get(mode);
	if (strcmp(mode, "cta") == 0 && cfgMode)
	{
		const char* name = cfgMode->getCString("name");
		int32_t slippage = cfgMode->getInt32("slippage");
		_cta_mocker = new ExpCtaMocker(&_replayer, name, slippage, &_notifier);
		_cta_mocker->init_cta_factory(cfgMode);
		_replayer.register_sink(_cta_mocker, name);
	}
	else if (strcmp(mode, "hft") == 0 && cfgMode)
	{
		const char* name = cfgMode->getCString("name");
		_hft_mocker = new ExpHftMocker(&_replayer, name);
		_hft_mocker->init_hft_factory(cfgMode);
		_replayer.register_sink(_hft_mocker, name);
	}
	else if (strcmp(mode, "sel") == 0 && cfgMode)
	{
		const char* name = cfgMode->getCString("name");
		int32_t slippage = cfgMode->getInt32("slippage");
		_sel_mocker = new ExpSelMocker(&_replayer, name, slippage);
		_sel_mocker->init_sel_factory(cfgMode);
		_replayer.register_sink(_sel_mocker, name);

		WTSVariant* cfgTask = cfgMode->get("task");
		if(cfgTask)
			_replayer.register_task(_sel_mocker->id(), cfgTask->getUInt32("date"), cfgTask->getUInt32("time"),
				cfgTask->getCString("period"), cfgTask->getCString("trdtpl"), cfgTask->getCString("session"));
	}
	else if (strcmp(mode, "exec") == 0 && cfgMode)
	{
		const char* name = cfgMode->getCString("name");
		_exec_mocker = new ExecMocker(&_replayer);
		_exec_mocker->init(cfgMode);
		_replayer.register_sink(_exec_mocker, name);
	}
}

/**
 * @brief 启动回测引擎
 * @details 启动历史数据回放器进行回测
 *          支持同步和异步两种模式运行
 *          异步模式下会创建新线程进行回测，不会阻塞当前线程
 * 
 * @param bNeedDump 是否需要导出回测过程的详细数据
 * @param bAsync 是否使用异步模式运行
 */
void WtBtRunner::run(bool bNeedDump /* = false */, bool bAsync /* = false */)
{
	if (_running)
		return;

	_async = bAsync;

	WTSLogger::info("Backtesting will run in {} mode", _async ? "async" : "sync");

	if (_cta_mocker)
		_cta_mocker->enable_hook(_async);
	else if (_hft_mocker)
		_hft_mocker->enable_hook(_async);

	_replayer.prepare();
	if (!bAsync)
	{
		_replayer.run(bNeedDump);
	}
	else
	{
		_worker.reset(new StdThread([this, bNeedDump]() {
			_running = true;
			try
			{
				_replayer.run(bNeedDump);
			}
			catch (...)
			{
				WTSLogger::error("Exception raised while worker running");
				//print_stack_trace([](const char* message) {
				//	WTSLogger::error(message);
				//});
			}
			WTSLogger::debug("Worker thread of backtest finished");
			_running = false;

		}));
	}
}

/**
 * @brief 停止回测引擎
 * @details 停止正在运行的回测过程
 *          如果是异步模式，会等待异步线程完成
 *          在停止前会让模拟器完成最后一轮计算
 */
void WtBtRunner::stop()
{
	if (!_running)
	{
		if (_worker)
		{
			_worker->join();
			_worker.reset();
		}
		return;
	}

	_replayer.stop();

	WTSLogger::debug("Notify to finish last round");

	if (_cta_mocker)
		_cta_mocker->step_calc();

	if (_hft_mocker)
		_hft_mocker->step_tick();

	WTSLogger::debug("Last round ended");

	if (_worker)
	{
		_worker->join();
		_worker.reset();
	}

	WTSLogger::freeAllDynLoggers();

	WTSLogger::debug("Backtest stopped");
}

/**
 * @brief 释放回测引擎资源
 * @details 关闭日志系统并释放相关资源
 *          通常在回测结束或程序退出前调用
 */
void WtBtRunner::release()
{
	WTSLogger::stop();
}

/**
 * @brief 设置回测时间范围
 * @details 手动设置回测的起止时间
 *          时间以YYYYMMDDHHMMSS格式指定
 * 
 * @param stime 起始时间，格式YYYYMMDDHHMMSS
 * @param etime 结束时间，格式YYYYMMDDHHMMSS
 */
void WtBtRunner::set_time_range(WtUInt64 stime, WtUInt64 etime)
{
	_replayer.set_time_range(stime, etime);

	WTSLogger::info("Backtest time range is set to be [{},{}] mannually", stime, etime);
}

/**
 * @brief 启用或禁用tick数据回放
 * @details 设置是否在回测中回放造市数据
 *          启用tick回放可以提高回测精度，但会降低性能
 * 
 * @param bEnabled 是否启用tick数据回放，默认为true
 */
void WtBtRunner::enable_tick(bool bEnabled /* = true */)
{
	_replayer.enable_tick(bEnabled);

	WTSLogger::info("Tick data replaying is {}", bEnabled ? "enabled" : "disabled");
}

/**
 * @brief 清理回放器缓存
 * @details 清理并释放历史数据回放器的缓存数据
 *          当需要重新加载数据或减少内存占用时调用
 */
void WtBtRunner::clear_cache()
{
	_replayer.clear_cache();
}

/**
 * @brief 获取原始合约代码
 * @details 从标准化合约代码中获取原始合约代码
 *          在某些场景下需要使用原始合约代码而非标准化代码
 * 
 * @param stdCode 标准化合约代码
 * @return const char* 原始合约代码
 */
const char* WtBtRunner::get_raw_stdcode(const char* stdCode)
{
	static thread_local std::string s;
	s = _replayer.get_rawcode(stdCode);
	return s.c_str();
}

/**
 * @brief 日志级别标签数组
 * @details 定义了系统支持的日志级别标签
 *          从全部输出(all)到不输出(none)的不同级别
 *          依次为级别从低到高排列
 */
const char* LOG_TAGS[] = {
	"all",
	"debug",
	"info",
	"warn",
	"error",
	"fatal",
	"none",
};

/**
 * @brief 初始化事件通知器
 * @details 根据配置初始化事件通知器组件
 *          事件通知器用于将回测过程中的事件发送到外部监听器
 * 
 * @param cfg 事件通知器配置对象
 * @return bool 初始化是否成功
 */
bool WtBtRunner::initEvtNotifier(WTSVariant* cfg)
{
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}