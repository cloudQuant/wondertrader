/*!
 * \file WtRtRunner.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 实时交易运行器实现文件
 * \details 该文件实现了WtRtRunner类，用于管理实时交易环境，包括CTA策略、HFT高频策略和SEL选股策略的初始化、
 *          运行和事件处理，以及与外部模块的交互接口。
 */
#include "WtRtRunner.h"
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
 * @details 初始化WtRtRunner的所有成员变量为默认值，包括各种回调函数指针、引擎标记和数据加载器等
 */
WtRtRunner::WtRtRunner()
	: _data_store(NULL)
	, _cb_cta_init(NULL)
	, _cb_cta_tick(NULL)
	, _cb_cta_calc(NULL)
	, _cb_cta_bar(NULL)
	, _cb_cta_cond_trigger(NULL)
	, _cb_cta_sessevt(NULL)

	, _cb_sel_init(NULL)
	, _cb_sel_tick(NULL)
	, _cb_sel_calc(NULL)
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
	, _cb_hft_position(NULL)
	, _cb_hft_sessevt(NULL)

	, _cb_exec_cmd(NULL)
	, _cb_exec_init(NULL)

	, _cb_parser_evt(NULL)
	, _cb_parser_sub(NULL)

	, _cb_evt(NULL)
	, _is_hft(false)
	, _is_sel(false)

	, _ext_fnl_bar_loader(NULL)
	, _ext_raw_bar_loader(NULL)
	, _ext_adj_fct_loader(NULL)

	, _to_exit(false)
{
}


/**
 * @brief 析构函数
 * @details 清理WtRtRunner对象占用的资源
 */
WtRtRunner::~WtRtRunner()
{
}

/**
 * @brief 初始化运行器
 * @details 初始化日志系统和目录路径，在Windows平台上还会初始化崩溃转储机制
 * @param logCfg 日志配置文件或内容，默认为"logcfg.prop"
 * @param isFile 是否为文件路径，true表示文件路径，false表示配置内容，默认为true
 * @param genDir 数据生成目录
 * @return 是否初始化成功
 */
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

/**
 * @brief 注册事件回调函数
 * @details 注册事件回调函数，并将当前对象注册为各个引擎的事件监听器
 * @param cbEvt 事件回调函数
 */
void WtRtRunner::registerEvtCallback(FuncEventCallback cbEvt)
{
	_cb_evt = cbEvt;

	_cta_engine.regEventListener(this);
	_hft_engine.regEventListener(this);
	_sel_engine.regEventListener(this);
}

/**
 * @brief 注册Parser解析器回调函数
 * @details 注册外部Parser解析器的事件和订阅回调函数
 * @param cbEvt 事件回调函数，用于处理Parser解析器事件
 * @param cbSub 订阅回调函数，用于处理订阅和取消订阅请求
 */
void WtRtRunner::registerParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)
{
	_cb_parser_evt = cbEvt;
	_cb_parser_sub = cbSub;

	WTSLogger::info("Callbacks of Extented Parser registration done");
}

/**
 * @brief 注册Executer执行器回调函数
 * @details 注册外部Executer执行器的初始化和命令回调函数
 * @param cbInit 初始化回调函数，用于初始化执行器
 * @param cbExec 命令回调函数，用于处理执行器命令
 */
void WtRtRunner::registerExecuterPorter(FuncExecInitCallback cbInit, FuncExecCmdCallback cbExec)
{
	_cb_exec_init = cbInit;
	_cb_exec_cmd = cbExec;

	WTSLogger::info("Callbacks of Extented Executer registration done");
}

/**
 * @brief 注册CTA策略回调函数
 * @details 注册CTA策略的各种事件回调函数，包括初始化、Tick数据、计算、K线、交易日事件和条件触发等
 * @param cbInit 初始化回调函数
 * @param cbTick Tick数据回调函数
 * @param cbCalc 计算回调函数
 * @param cbBar K线数据回调函数
 * @param cbSessEvt 交易日事件回调函数
 * @param cbCondTrigger 条件触发回调函数，可选参数，默认为NULL
 */
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

/**
 * @brief 注册SEL选股策略回调函数
 * @details 注册SEL选股策略的各种事件回调函数，包括初始化、Tick数据、计算、K线和交易日事件等
 * @param cbInit 初始化回调函数
 * @param cbTick Tick数据回调函数
 * @param cbCalc 计算回调函数
 * @param cbBar K线数据回调函数
 * @param cbSessEvt 交易日事件回调函数
 */
void WtRtRunner::registerSelCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt)
{
	_cb_sel_init = cbInit;
	_cb_sel_tick = cbTick;
	_cb_sel_calc = cbCalc;
	_cb_sel_bar = cbBar;

	_cb_sel_sessevt = cbSessEvt;

	WTSLogger::info("Callbacks of SEL engine registration done");
}

/**
 * @brief 注册HFT高频策略回调函数
 * @details 注册HFT高频策略的各种事件回调函数，包括初始化、Tick数据、K线、通道事件、订单、成交、委托等
 * @param cbInit 初始化回调函数
 * @param cbTick Tick数据回调函数
 * @param cbBar K线数据回调函数
 * @param cbChnl 通道事件回调函数
 * @param cbOrd 订单回调函数
 * @param cbTrd 成交回调函数
 * @param cbEntrust 委托回调函数
 * @param cbOrdDtl 订单明细回调函数
 * @param cbOrdQue 订单队列回调函数
 * @param cbTrans 成交明细回调函数
 * @param cbSessEvt 交易日事件回调函数
 * @param cbPosition 持仓回调函数
 */
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

/**
 * @brief 加载最终历史K线数据
 * @details 实现IHisDataLoader接口方法，加载指定合约的最终历史K线数据，已经过滤和调整
 * @param obj 传入的上下文指针
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param cb 数据读取回调函数
 * @return 是否成功加载数据
 */
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

/**
 * @brief 加载原始历史K线数据
 * @details 实现IHisDataLoader接口方法，加载指定合约的原始历史K线数据，未经过滤和调整
 * @param obj 传入的上下文指针
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param cb 数据读取回调函数
 * @return 是否成功加载数据
 */
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

/**
 * @brief 加载所有复权因子
 * @details 实现IHisDataLoader接口方法，加载系统中所有合约的复权因子
 * @param obj 传入的上下文指针
 * @param cb 数据读取回调函数
 * @return 是否成功加载数据
 */
bool WtRtRunner::loadAllAdjFactors(void* obj, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader("");
}

/**
 * @brief 加载指定合约的复权因子
 * @details 实现IHisDataLoader接口方法，加载指定合约的复权因子
 * @param obj 传入的上下文指针
 * @param stdCode 标准化合约代码
 * @param cb 数据读取回调函数
 * @return 是否成功加载数据
 */
bool WtRtRunner::loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb)
{
	StdUniqueLock lock(_feed_mtx);
	if (_ext_adj_fct_loader == NULL)
		return false;

	_feed_obj = obj;
	_feeder_fcts = cb;

	return _ext_adj_fct_loader(stdCode);
}

/**
 * @brief 输入复权因子数据
 * @details 用于外部模块向运行器提供复权因子数据
 * @param stdCode 标准化合约代码
 * @param dates 日期数组
 * @param factors 因子数组
 * @param count 数据数量
 */
void WtRtRunner::feedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count)
{
	_feeder_fcts(_feed_obj, stdCode, dates, factors, count);
}


/**
 * @brief 输入原始K线数据
 * @details 用于外部模块向运行器提供原始K线数据
 * @param bars K线数据数组
 * @param count 数据数量
 */
void WtRtRunner::feedRawBars(WTSBarStruct* bars, uint32_t count)
{
	if (_ext_fnl_bar_loader == NULL)
	{
		WTSLogger::error("Cannot feed bars because of no extented bar loader registered.");
		return;
	}

	_feeder_bars(_feed_obj, bars, count);
}


/**
 * @brief 创建外部数据解析器
 * @details 创建一个外部数据解析器，用于处理外部数据源
 * @param id 解析器ID
 * @return 是否创建成功
 */
bool WtRtRunner::createExtParser(const char* id)
{
	ParserAdapterPtr adapter(new ParserAdapter);
	ExpParser* parser = new ExpParser(id);
	adapter->initExt(id, parser, _engine, _engine->get_basedata_mgr(), _engine->get_hot_mgr());
	_parsers.addAdapter(id, adapter);
	WTSLogger::info("Extended parser created");
	return true;
}

/**
 * @brief 创建外部执行器
 * @details 创建一个外部执行器，用于处理外部交易指令
 * @param id 执行器ID
 * @return 是否创建成功
 */
bool WtRtRunner::createExtExecuter(const char* id)
{
	ExpExecuter* executer = new ExpExecuter(id);
	executer->init();
	_cta_engine.addExecuter(ExecCmdPtr(executer));
	WTSLogger::info("Extended Executer created");
	return true;
}

/**
 * @brief 创建CTA策略上下文
 * @details 创建一个CTA策略的运行上下文，并添加到CTA引擎中
 * @param name 策略名称
 * @param slippage 滑点设置，默认为0
 * @return 上下文ID
 */
uint32_t WtRtRunner::createCtaContext(const char* name, int32_t slippage /* = 0 */)
{
	ExpCtaContext* ctx = new ExpCtaContext(&_cta_engine, name, slippage);
	_cta_engine.addContext(CtaContextPtr(ctx));
	return ctx->id();
}

/**
 * @brief 创建HFT高频策略上下文
 * @details 创建一个HFT高频策略的运行上下文，并绑定交易适配器
 * @param name 策略名称
 * @param trader 交易适配器ID
 * @param bAgent 是否为代理模式
 * @param slippage 滑点设置，默认为0
 * @return 上下文ID
 */
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

/**
 * @brief 创建SEL选股策略上下文
 * @details 创建一个SEL选股策略的运行上下文，并设置执行周期
 * @param name 策略名称
 * @param date 日期，格式为YYYYMMDD
 * @param time 时间，格式为HHMMSS
 * @param period 周期设置，可以为"d"(日)、"w"(周)、"m"(月)、"y"(年)、"min"(分钟)
 * @param slippage 滑点设置
 * @param trdtpl 交易模板，默认为"CHINA"
 * @param session 交易时段，默认为"TRADING"
 * @return 上下文ID
 */
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


/**
 * @brief 获取原始标准化合约代码
 * @details 将标准化合约代码转换为原始格式
 * @param stdCode 标准化合约代码
 * @return 原始格式的合约代码
 */
const char* WtRtRunner::get_raw_stdcode(const char* stdCode)
{
	static thread_local std::string s;
	s = _engine->get_rawcode(stdCode);
	return s.c_str();
}


/**
 * @brief 获取CTA策略上下文
 * @details 根据ID获取CTA策略上下文对象
 * @param id 上下文ID
 * @return CTA策略上下文指针
 */
CtaContextPtr WtRtRunner::getCtaContext(uint32_t id)
{
	return _cta_engine.getContext(id);
}

/**
 * @brief 获取HFT高频策略上下文
 * @details 根据ID获取HFT高频策略上下文对象
 * @param id 上下文ID
 * @return HFT高频策略上下文指针
 */
HftContextPtr WtRtRunner::getHftContext(uint32_t id)
{
	return _hft_engine.getContext(id);
}

/**
 * @brief 获取SEL选股策略上下文
 * @details 根据ID获取SEL选股策略上下文对象
 * @param id 上下文ID
 * @return SEL选股策略上下文指针
 */
SelContextPtr WtRtRunner::getSelContext(uint32_t id)
{
	return _sel_engine.getContext(id);
}

/**
 * @brief K线周期数据事件回调
 * @details 处理不同策略引擎的K线周期数据事件
 * @param id 上下文ID
 * @param stdCode 标准化合约代码
 * @param period 周期标识
 * @param newBar 新K线数据
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtRtRunner::ctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType /* = ET_CTA */)
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
 * @brief 策略计算事件回调
 * @details 处理CTA和SEL策略引擎的计算事件
 * @param id 上下文ID
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtRtRunner::ctx_on_calc(uint32_t id, uint32_t curDate, uint32_t curTime, EngineType eType /* = ET_CTA */)
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
 * @brief 条件触发事件回调
 * @details 处理CTA策略引擎的条件触发事件
 * @param id 上下文ID
 * @param stdCode 标准化合约代码
 * @param target 目标价格
 * @param price 当前价格
 * @param usertag 用户标签
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtRtRunner::ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType /* = ET_CTA */)
{
	switch (eType)
	{
	case ET_CTA: if (_cb_cta_cond_trigger) _cb_cta_cond_trigger(id, stdCode, target, price, usertag); break;
	default:
		break;
	}
}

/**
 * @brief 策略初始化事件回调
 * @details 处理各类策略引擎的初始化事件
 * @param id 上下文ID
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtRtRunner::ctx_on_init(uint32_t id, EngineType eType/* = ET_CTA*/)
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
 * @brief 交易时段事件回调
 * @details 处理各类策略引擎的交易时段开始或结束事件
 * @param id 上下文ID
 * @param curTDate 当前交易日期，格式为YYYYMMDD
 * @param isBegin 是否为交易时段开始事件，默认为true
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtRtRunner::ctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin /* = true */, EngineType eType /* = ET_CTA */)
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
 * @brief 实时行情数据回调
 * @details 处理各类策略引擎的行情数据更新事件
 * @param id 上下文ID
 * @param stdCode 标准化合约代码
 * @param newTick 新行情数据
 * @param eType 引擎类型，默认为CTA引擎
 */
void WtRtRunner::ctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType /* = ET_CTA */)
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
 * @brief 通道丢失事件回调
 * @details 处理HFT策略引擎的通道丢失事件
 * @param cHandle 通道句柄
 * @param trader 交易适配器ID
 */
void WtRtRunner::hft_on_channel_lost(uint32_t cHandle, const char* trader)
{
	if (_cb_hft_chnl)
		_cb_hft_chnl(cHandle, trader, CHNL_EVENT_LOST);
}

/**
 * @brief HFT交易通道就绪回调
 * @details 处理HFT高频交易的通道就绪事件
 * @param cHandle 上下文句柄
 * @param trader 交易器ID
 */
void WtRtRunner::hft_on_channel_ready(uint32_t cHandle, const char* trader)
{
	if (_cb_hft_chnl)
		_cb_hft_chnl(cHandle, trader, CHNL_EVENT_READY);
}

/**
 * @brief HFT委托回报回调
 * @details 处理HFT高频交易的委托回报事件
 * @param cHandle 上下文句柄
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 是否委托成功
 * @param message 错误消息
 * @param userTag 用户标签
 */
void WtRtRunner::hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)
{
	if (_cb_hft_entrust)
		_cb_hft_entrust(cHandle, localid, stdCode, bSuccess, message, userTag);
}

/**
 * @brief HFT委托状态回调
 * @details 处理HFT高频交易的委托状态变化事件
 * @param cHandle 上下文句柄
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入委托
 * @param totalQty 总委托量
 * @param leftQty 剩余委托量
 * @param price 委托价格
 * @param isCanceled 是否已撤单
 * @param userTag 用户标签
 */
void WtRtRunner::hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)
{
	if (_cb_hft_ord)
		_cb_hft_ord(cHandle, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);
}

/**
 * @brief HFT成交回调
 * @details 处理HFT高频交易的成交事件
 * @param cHandle 上下文句柄
 * @param localid 本地委托ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交量
 * @param price 成交价格
 * @param userTag 用户标签
 */
void WtRtRunner::hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)
{
	if (_cb_hft_trd)
		_cb_hft_trd(cHandle, localid, stdCode, isBuy, vol, price, userTag);
}

/**
 * @brief HFT持仓回调
 * @details 处理HFT高频交易的持仓变化事件
 * @param cHandle 上下文句柄
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头持仓
 * @param prevol 之前的总仓位
 * @param preavail 之前的可用仓位
 * @param newvol 新的总仓位
 * @param newavail 新的可用仓位
 */
void WtRtRunner::hft_on_position(uint32_t cHandle, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)
{
	if (_cb_hft_position)
		_cb_hft_position(cHandle, stdCode, isLong, prevol, preavail, newvol, newavail);
}

/**
 * @brief 配置运行器
 * @details 根据配置文件或内容初始化运行器，加载基础数据、交易时段、品种、合约等信息
 * @param cfgFile 配置文件路径或内容
 * @param isFile 是否为文件路径，true表示文件路径，false表示配置内容
 * @return 是否配置成功
 */
bool WtRtRunner::config(const char* cfgFile, bool isFile /* = true */)
{
	_config = isFile ? WTSCfgLoader::load_from_file(cfgFile) : WTSCfgLoader::load_from_content(cfgFile, false);

	//基础数据文件
	WTSVariant* cfgBF = _config->get("basefiles");
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

	WTSArray* ayContracts = _bd_mgr.getContracts();
	for (auto it = ayContracts->begin(); it != ayContracts->end(); it++)
	{
		WTSContractInfo* cInfo = (WTSContractInfo*)(*it);
		bool isHot = _hot_mgr.isHot(cInfo->getExchg(), cInfo->getCode());
		bool isSecond = _hot_mgr.isSecond(cInfo->getExchg(), cInfo->getCode());
		
		std::string hotCode = cInfo->getFullPid();
		if (isHot)
			hotCode += ".HOT";
		else if (isSecond)
			hotCode += ".2ND";
		else
			hotCode = "";

		cInfo->setHotFlag(isHot ? 1 : (isSecond ? 2 : 0), hotCode.c_str());
	}
	ayContracts->release();

	if(cfgBF->has("rules"))
	{
		auto cfgRules = cfgBF->get("rules");
		auto tags = cfgRules->memberNames();
		for(const std::string& ruleTag : tags)
		{
			_hot_mgr.loadCustomRules(ruleTag.c_str(), cfgRules->getCString(ruleTag.c_str()));
			WTSLogger::info("{} rules loaded from {}", ruleTag, cfgRules->getCString(ruleTag.c_str()));
		}
	}

	//初始化运行环境
	initEngine();

	//初始化数据管理
	initDataMgr();

	//初始化开平策略
	if (!initActionPolicy())
		return false;

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
						WTSLogger::error("Loading parsers failed");
					var->release();
				}
				else
				{
					WTSLogger::error("Loading parser config {} failed", filename);
				}
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

	//初始化交易通道
	WTSVariant* cfgTraders = _config->get("traders");
	if(cfgTraders)
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
						WTSLogger::error("Loading traders failed");
					var->release();
				}
				else
				{
					WTSLogger::error("Loading trader config {} failed", filename);
				}
			}
			else
			{
				WTSLogger::error("Trader configuration {} not exists", filename);
			}
		}
		else if (cfgTraders->type() == WTSVariant::VT_Array)
		{
			initTraders(cfgTraders);
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

	if (!_is_hft)
		initCtaStrategies();
	else
		initHftStrategies();
	
	return true;
}

/**
 * @brief 初始化CTA策略
 * @details 从配置中加载CTA策略并创建相应的策略上下文
 * @return 是否初始化成功
 */
bool WtRtRunner::initCtaStrategies()
{
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	cfg = cfg->get("cta");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		int32_t slippage = cfgItem->getInt32("slippage");
		CtaStrategyPtr stra = _cta_mgr.createStrategy(name, id);
		stra->self()->init(cfgItem->get("params"));
		CtaStraContext* ctx = new CtaStraContext(&_cta_engine, id, slippage);
		ctx->set_strategy(stra->self());
		_cta_engine.addContext(CtaContextPtr(ctx));
	}

	return true;
}

/**
 * @brief 初始化SEL选股策略
 * @details 从配置中加载SEL选股策略并创建相应的策略上下文
 * @return 是否初始化成功
 */
bool WtRtRunner::initSelStrategies()
{
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	cfg = cfg->get("cta");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
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
		stra->self()->init(cfgItem->get("params"));
		SelStraContext* ctx = new SelStraContext(&_sel_engine, id, slippage);
		ctx->set_strategy(stra->self());
		_sel_engine.addContext(SelContextPtr(ctx), date, time, ptype);
	}

	return true;
}

/**
 * @brief 初始化HFT高频策略
 * @details 从配置中加载HFT高频策略并创建相应的策略上下文
 * @return 是否初始化成功
 */
bool WtRtRunner::initHftStrategies()
{
	WTSVariant* cfg = _config->get("strategies");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	cfg = cfg->get("hft");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Array)
		return false;

	for (uint32_t idx = 0; idx < cfg->size(); idx++)
	{
		WTSVariant* cfgItem = cfg->get(idx);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");
		bool bAgent = cfgItem->getBoolean("agent");
		int32_t slippage = cfgItem->getInt32("slippage");

		HftStrategyPtr stra = _hft_mgr.createStrategy(name, id);
		if (stra == NULL)
			continue;

		stra->self()->init(cfgItem->get("params"));
		HftStraContext* ctx = new HftStraContext(&_hft_engine, id, bAgent, slippage);
		ctx->set_strategy(stra->self());

		const char* traderid = cfgItem->getCString("trader");
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

		_hft_engine.addContext(HftContextPtr(ctx));
	}

	return true;
}

/**
 * @brief 初始化交易引擎
 * @details 根据环境配置初始化交易引擎，设置相应的引擎类型（CTA、SEL或HFT）
 * @return 是否初始化成功
 */
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

	_engine->set_adapter_mgr(&_traders);
	
	return true;
}

/**
 * @brief 初始化数据管理器
 * @details 根据配置初始化数据管理器，设置数据存储路径
 * @return 是否初始化成功
 */
bool WtRtRunner::initDataMgr()
{
	WTSVariant* cfg = _config->get("data");
	if (cfg == NULL)
		return false;

	_data_mgr.regsiter_loader(this);

	_data_mgr.init(cfg, _engine, true);
	WTSLogger::log_raw(LL_INFO, "Data manager initialized");
	return true;
}

/**
 * @brief 初始化行情解析器
 * @details 根据配置初始化各类行情解析器，包括内置解析器和外部解析器
 * @param cfgParsers 解析器配置
 * @return 是否初始化成功
 */
bool WtRtRunner::initParsers(WTSVariant* cfgParsers)
{
	if (cfgParsers == NULL || cfgParsers->type() != WTSVariant::VT_Array)
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

		ParserAdapterPtr adapter(new ParserAdapter);
		adapter->init(realid.c_str(), cfgItem, _engine, _engine->get_basedata_mgr(), _engine->get_hot_mgr());
		_parsers.addAdapter(realid.c_str(), adapter);

		count++;
	}

	WTSLogger::info("{} parsers loaded", count);

	return true;
}

/**
 * @brief 初始化执行器
 * @details 根据配置初始化交易指令执行器，包括内置执行器和外部执行器
 * @param cfgExecuter 执行器配置
 * @return 是否初始化成功
 */
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
					WTSLogger::error("Trader {} not exists, cannot configured for executer %s", tid, id);
				}
			}

			_cta_engine.addExecuter(ExecCmdPtr(executer));
		}
		else if (name == "diff")
		{
			WtDiffExecuter* executer = new WtDiffExecuter(&_exe_factory, id, &_data_mgr, &_bd_mgr);
			if (!executer->init(cfgItem))
				return false;

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
					WTSLogger::error("Trader {} not exists, cannot configured for executer %s", tid, id);
				}
			}

			_cta_engine.addExecuter(ExecCmdPtr(executer));
		}
		else if (name == "arbi")
		{
			WtArbiExecuter* executer = new WtArbiExecuter(&_exe_factory, id, &_data_mgr);
			if (!executer->init(cfgItem))
				return false;

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
					WTSLogger::error("Trader {} not exists, cannot configured for executer %s", tid, id);
				}
			}

			_cta_engine.addExecuter(ExecCmdPtr(executer));
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

/**
 * @brief 初始化事件通知器
 * @details 根据配置初始化事件通知器，用于处理系统事件的通知
 * @return 是否初始化成功
 */
bool WtRtRunner::initEvtNotifier()
{
	WTSVariant* cfg = _config->get("notifier");
	if (cfg == NULL || cfg->type() != WTSVariant::VT_Object)
		return false;

	_notifier.init(cfg);

	return true;
}

/**
 * @brief 初始化交易适配器
 * @details 根据配置初始化各类交易适配器，用于实际交易指令的执行
 * @param cfgTraders 交易适配器配置
 * @return 是否初始化成功
 */
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
		adapter->init(id, cfgItem, &_bd_mgr, &_act_policy);

		_traders.addAdapter(id, adapter);
		count++;
	}

	WTSLogger::info("{} traders loaded", count);

	return true;
}

/**
 * @brief 运行交易引擎
 * @details 启动交易引擎，可以选择同步或异步运行模式
 * @param bAsync 是否异步运行，默认为false
 */
void WtRtRunner::run(bool bAsync /* = false */)
{
	try
	{
		_parsers.run();
		_traders.run();

		_engine->run();

		if (!bAsync)
		{
			install_signal_hooks([this](const char* message) {
				if (!_to_exit)
					WTSLogger::error(message);
			}, [this](bool toExit) {
				if (_to_exit)
					return;
				_to_exit = toExit;
				WTSLogger::info("Exit flag is {}", _to_exit);
			});

			while (!_to_exit)
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
	"none",
};

/**
 * @brief 处理日志追加
 * @details 处理日志追加事件，将日志消息通过通知器发送
 * @param ll 日志级别
 * @param msg 日志消息
 */
void WtRtRunner::handleLogAppend(WTSLogLevel ll, const char* msg)
{
	_notifier.notify_log(LOG_TAGS[ll-100], msg);
}

/**
 * @brief 释放资源
 * @details 清理并释放交易引擎相关的所有资源
 */
void WtRtRunner::release()
{
	WTSLogger::stop();
}

/**
 * @brief 初始化交易行为策略
 * @details 根据配置初始化交易行为策略，加载交易限制和风控策略
 * @return 是否初始化成功
 */
bool WtRtRunner::initActionPolicy()
{
	const char* action_file = _config->getCString("bspolicy");
	if (strlen(action_file) <= 0)
		return false;

	bool ret = _act_policy.init(action_file);
	WTSLogger::info("Action policies initialized");
	return ret;
}

/**
 * @brief 添加SEL选股策略工厂
 * @details 从指定文件夹加载SEL选股策略工厂
 * @param folder 工厂文件夹路径
 * @return 是否加载成功
 */
bool WtRtRunner::addSelFactories(const char* folder)
{
	return _sel_mgr.loadFactories(folder);
}

bool WtRtRunner::addExeFactories(const char* folder)
{
	return _exe_factory.loadFactories(folder);
}

/**
 * @brief 添加HFT高频策略工厂
 * @details 从指定文件夹加载HFT高频策略工厂
 * @param folder 工厂文件夹路径
 * @return 是否加载成功
 */
bool WtRtRunner::addHftFactories(const char* folder)
{
	return _hft_mgr.loadFactories(folder);
}

void WtRtRunner::hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue)
{
	if (_cb_hft_ordque)
		_cb_hft_ordque(id, stdCode, &newOrdQue->getOrdQueStruct());
}

void WtRtRunner::hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	if (_cb_hft_orddtl)
		_cb_hft_orddtl(id, stdCode, &newOrdDtl->getOrdDtlStruct());
}

void WtRtRunner::hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTrans)
{
	if (_cb_hft_trans)
		_cb_hft_trans(id, stdCode, &newTrans->getTransStruct());
}

#pragma region "Extended Parser"
void WtRtRunner::parser_init(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_INIT, id);
}

void WtRtRunner::parser_connect(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_CONNECT, id);
}

void WtRtRunner::parser_disconnect(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_DISCONNECT, id);
}

void WtRtRunner::parser_release(const char* id)
{
	if (_cb_parser_evt)
		_cb_parser_evt(EVENT_PARSER_RELEASE, id);
}

void WtRtRunner::parser_subscribe(const char* id, const char* code)
{
	if (_cb_parser_sub)
		_cb_parser_sub(id, code, true);
}

void WtRtRunner::parser_unsubscribe(const char* id, const char* code)
{
	if (_cb_parser_sub)
		_cb_parser_sub(id, code, false);
}

void WtRtRunner::on_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag)
{
	ParserAdapterPtr adapter = _parsers.getAdapter(id);
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
	if (_cb_exec_init)
		_cb_exec_init(id);
}

void WtRtRunner::executer_set_position(const char* id, const char* stdCode, double target)
{
	if (_cb_exec_cmd)
		_cb_exec_cmd(id, stdCode, target);
}
#pragma endregion