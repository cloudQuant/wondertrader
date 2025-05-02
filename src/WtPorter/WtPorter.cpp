/*!
 * \file WtPorter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtPorter.h"
#include "WtRtRunner.h"

#include "../WtCore/WtHelper.h"
#include "../WTSTools/WTSLogger.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSVersion.h"

#ifdef _WIN32
#   ifdef _WIN64
    char PLATFORM_NAME[] = "X64";
#   else
    char PLATFORM_NAME[] = "X86";
#endif
#else
    char PLATFORM_NAME[] = "UNIX";
#endif

/**
 * @brief 获取WtRtRunner全局单例
 * @return 返回WtRtRunner的静态单例对象引用
 * @details 采用单例模式实现，确保全局只有一个WtRtRunner实例
 *          WtRtRunner是实时交易引擎的入口类，管理各种策略和交易的运行
 */
WtRtRunner& getRunner()
{
	static WtRtRunner runner;
	return runner;
}


/**
 * @brief 注册事件回调函数
 * @param cbEvt 事件回调函数指针
 * @details 注册系统事件的回调函数，用于接收交易引擎中的事件通知
 *          事件包括系统启动、终止、行情接收等重要状态变化
 *          实现了WtPorter.h中声明的register_evt_callback函数
 */
void register_evt_callback(FuncEventCallback cbEvt)
{
	getRunner().registerEvtCallback(cbEvt);
}

/**
 * @brief 注册CTA策略回调函数集
 * @param cbInit 策略初始化回调函数
 * @param cbTick tick数据回调函数
 * @param cbCalc 周期计算回调函数
 * @param cbBar K线数据回调函数
 * @param cbSessEvt 交易时段事件回调函数
 * @param cbCondTrigger 条件触发回调函数，默认为NULL
 * @details 注册CTA策略所需的各类回调函数
 *          CTA策略（Commodity Trading Advisor）是单合约策略，主要用于期货等标的交易
 *          实现了WtPorter.h中声明的register_cta_callbacks函数
 */
void register_cta_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCondTriggerCallback cbCondTrigger/* = NULL*/)
{
	getRunner().registerCtaCallbacks(cbInit, cbTick, cbCalc, cbBar, cbSessEvt, cbCondTrigger);
}

/**
 * @brief 注册SEL选股策略回调函数集
 * @param cbInit 策略初始化回调函数
 * @param cbTick tick数据回调函数
 * @param cbCalc 周期计算回调函数
 * @param cbBar K线数据回调函数
 * @param cbSessEvt 交易时段事件回调函数
 * @details 注册选股策略所需的各类回调函数
 *          SEL策略主要用于选股，可以管理多个股票标的组合
 *          实现了WtPorter.h中声明的register_sel_callbacks函数
 */
void register_sel_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt)
{
	getRunner().registerSelCallbacks(cbInit, cbTick, cbCalc, cbBar, cbSessEvt);
}

/**
 * @brief 注册HFT高频策略回调函数集
 * @param cbInit 策略初始化回调函数
 * @param cbTick tick数据回调函数
 * @param cbBar K线数据回调函数
 * @param cbChnl 通道事件回调函数
 * @param cbOrd 订单回调函数
 * @param cbTrd 成交回调函数
 * @param cbEntrust 委托回调函数
 * @param cbOrdDtl 逮单明细回调函数
 * @param cbOrdQue 委托队列回调函数
 * @param cbTrans 成交明细回调函数
 * @param cbSessEvt 交易时段事件回调函数
 * @param cbPosition 持仓变化回调函数
 * @details 注册高频交易策略所需的各类回调函数
 *          HFT策略（High Frequency Trading）是高频交易策略，需要更快速地响应市场变化
 *          包含了更多微观市场数据的回调，如委托队列、成交明细等
 *          实现了WtPorter.h中声明的register_hft_callbacks函数
 */
void register_hft_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar, 
	FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
	FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt, FuncHftPosCallback cbPosition)
{
	getRunner().registerHftCallbacks(cbInit, cbTick, cbBar, cbChnl, cbOrd, cbTrd, cbEntrust, cbOrdDtl, cbOrdQue, cbTrans, cbSessEvt, cbPosition);
}

/**
 * @brief 注册解析器回调函数集
 * @param cbEvt 解析器事件回调函数
 * @param cbSub 解析器订阅回调函数
 * @details 注册行情解析器的回调函数
 *          行情解析器负责从各种数据源解析行情数据，并转换为平台支持的格式
 *          事件回调处理解析器事件，如连接、断开等；订阅回调处理行情订阅请求
 *          实现了WtPorter.h中声明的register_parser_callbacks函数
 */
void register_parser_callbacks(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)
{
	getRunner().registerParserPorter(cbEvt, cbSub);
}

/**
 * @brief 注册执行器回调函数集
 * @param cbInit 执行器初始化回调函数
 * @param cbExec 执行器命令回调函数
 * @details 注册交易执行器的回调函数
 *          执行器负责处理具体的订单执行逻辑，控制交易执行的细节
 *          初始化回调处理执行器的初始化，命令回调用于处理执行命令
 *          实现了WtPorter.h中声明的register_exec_callbacks函数
 */
void register_exec_callbacks(FuncExecInitCallback cbInit, FuncExecCmdCallback cbExec)
{
	getRunner().registerExecuterPorter(cbInit, cbExec);
}

/**
 * @brief 创建外部行情解析器
 * @param id 解析器唯一标识
 * @return 创建成功返回true，失败返回false
 * @details 创建一个外部行情解析器实例
 *          外部解析器用于处理非内置数据源的行情数据
 *          通过id标识唯一确定该解析器
 *          实现了WtPorter.h中声明的create_ext_parser函数
 */
bool create_ext_parser(const char* id)
{
	return getRunner().createExtParser(id);
}

/**
 * @brief 创建外部交易执行器
 * @param id 执行器唯一标识
 * @return 创建成功返回true，失败返回false
 * @details 创建一个外部交易执行器实例
 *          外部执行器用于处理非内置执行器的订单执行逻辑
 *          通过id标识唯一确定执行器
 *          实现了WtPorter.h中声明的create_ext_executer函数
 */
bool create_ext_executer(const char* id)
{
	return getRunner().createExtExecuter(id);
}

/**
 * @brief 注册外部数据加载器
 * @param fnlBarLoader 最终K线加载器回调函数
 * @param rawBarLoader 原始K线加载器回调函数
 * @param fctLoader 除权因子加载器回调函数
 * @param tickLoader tick数据加载器回调函数
 * @details 注册外部数据加载器的回调函数集
 *          外部数据加载器用于从自定义数据源加载历史行情数据
 *          支持最终K线、原始K线、除权因子和tick数据的加载
 *          实现了WtPorter.h中声明的register_ext_data_loader函数
 */
void register_ext_data_loader(FuncLoadFnlBars fnlBarLoader, FuncLoadRawBars rawBarLoader, FuncLoadAdjFactors fctLoader, FuncLoadRawTicks tickLoader)
{
	getRunner().registerExtDataLoader(fnlBarLoader, rawBarLoader, fctLoader, tickLoader);
}

/**
 * @brief 输入原始K线数据
 * @param bars K线数据结构指针
 * @param count K线数量
 * @details 将原始K线数据输入到交易引擎中
 *          原始K线数据通常是由外部数据源或自定义数据加载器提供的
 *          引擎会自动处理这些数据并分发给相应的策略
 *          实现了WtPorter.h中声明的feed_raw_bars函数
 */
void feed_raw_bars(WTSBarStruct* bars, WtUInt32 count)
{
	getRunner().feedRawBars(bars, count);
}

/**
 * @brief 输入除权因子数据
 * @param stdCode 标准合约代码
 * @param dates 除权日期数组指针，格式YYYYMMDD
 * @param factors 除权因子数组指针
 * @param count 除权因子数量
 * @details 将合约的除权因子数据输入到交易引擎中
 *          除权因子主要用于股票等需要除权的金融工具，用于修正价格
 *          引擎会自动处理这些因子并应用于相关的价格数据计算
 *          实现了WtPorter.h中声明的feed_adj_factors函数
 */
void feed_adj_factors(WtString stdCode, WtUInt32* dates, double* factors, WtUInt32 count)
{
	getRunner().feedAdjFactors(stdCode, (uint32_t*)dates, factors, count);
}

void feed_raw_ticks(WTSTickStruct* ticks, WtUInt32 count)
{
	WTSLogger::error("API not implemented");
}

void init_porter(const char* logProfile, bool isFile, const char* genDir)
{
	static bool inited = false;

	if (inited)
		return;

	getRunner().init(logProfile, isFile, genDir);

	inited = true;
}

void config_porter(const char* cfgfile, bool isFile)
{
	if (strlen(cfgfile) == 0)
		getRunner().config("config.json", true);
	else
		getRunner().config(cfgfile, isFile);
}

void run_porter(bool bAsync)
{
	getRunner().run(bAsync);
}

void release_porter()
{
	getRunner().release();
}

const char* get_version()
{
	static std::string _ver;
	if (_ver.empty())
	{
		_ver = PLATFORM_NAME;
		_ver += " ";
		_ver += WT_VERSION;
		_ver += " Build@";
		_ver += __DATE__;
		_ver += " ";
		_ver += __TIME__;
	}
	return _ver.c_str();
}

const char* get_raw_stdcode(const char* stdCode)
{
	return getRunner().get_raw_stdcode(stdCode);
}

void write_log(WtUInt32 level, const char* message, const char* catName)
{
	if (strlen(catName) > 0)
	{
		WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message);
	}
	else
	{
		WTSLogger::log_raw((WTSLogLevel)level, message);
	}
}

bool reg_cta_factories(const char* factFolder)
{
	return getRunner().addCtaFactories(factFolder);
}

bool reg_sel_factories(const char* factFolder)
{
	return getRunner().addSelFactories(factFolder);
}

bool reg_hft_factories(const char* factFolder)
{
	return getRunner().addHftFactories(factFolder);
}

bool reg_exe_factories(const char* factFolder)
{
	return getRunner().addExeFactories(factFolder);
}


#pragma region "CTA策略接口"

CtxHandler create_cta_context(const char* name, int slippage)
{
	return getRunner().createCtaContext(name, slippage);
}

void cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_enter_long(stdCode, qty, userTag, limitprice, stopprice);
}

void cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_exit_long(stdCode, qty, userTag, limitprice, stopprice);
}

void cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_enter_short(stdCode, qty, userTag, limitprice, stopprice);
}

void cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_exit_short(stdCode, qty, userTag, limitprice, stopprice);
}

WtUInt32 cta_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, bool isMain, FuncGetBarsCallback cb)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSKlineSlice* kData = ctx->stra_get_bars(stdCode, period, barCnt, isMain);
		if (kData)
		{
			WtUInt32 reaCnt = (WtUInt32)kData->size();

			uint32_t blkCnt = kData->get_block_counts();
			for (uint32_t i = 0; i < blkCnt; i++)
			{
				if(kData->get_block_addr(i) != NULL)
					cb(cHandle, stdCode, period, kData->get_block_addr(i), kData->get_block_size(i), i == blkCnt - 1);
			}

			kData->release();
			return reaCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

WtUInt32	cta_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSTickSlice* tData = ctx->stra_get_ticks(stdCode, tickCnt);
		if (tData)
		{
			uint32_t thisCnt = min(tickCnt, (WtUInt32)tData->size());
			cb(cHandle, stdCode, (WTSTickStruct*)tData->at(0), thisCnt, true);
			tData->release();
			return thisCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

double cta_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

WtUInt64 cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

double cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

double cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

double cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

void cta_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
	{
		cb(cHandle, "", 0, true);
		return;
	}

	ctx->enum_position([cb, cHandle](const char* stdCode, double qty) {
		cb(cHandle, stdCode, qty, false);
	});

	cb(cHandle, "", 0, true);
}

double cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

double cta_get_fund_data(CtxHandler cHandle, int flag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}


void cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_set_position(stdCode, qty, userTag, limitprice, stopprice);
}


WtUInt64 cta_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

WtUInt64 cta_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

WtUInt64 cta_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

double cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

WtString cta_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}

double cta_get_price(const char* stdCode)
{
	return getRunner().getEngine()->get_cur_price(stdCode);
}

double cta_get_day_price(const char* stdCode, int flag)
{
	return getRunner().getEngine()->get_day_price(stdCode, flag);
}

WtUInt32 cta_get_tdate()
{
	return getRunner().getEngine()->get_trading_date();
}

WtUInt32 cta_get_date()
{
	return getRunner().getEngine()->get_date();
}

WtUInt32 cta_get_time()
{
	return getRunner().getEngine()->get_min_time();
}

void cta_log_text(CtxHandler cHandle, WtUInt32 level, const char* message)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	switch (level)
	{
	case LOG_LEVEL_DEBUG:
		ctx->stra_log_debug(message);
		break;
	case LOG_LEVEL_INFO:
		ctx->stra_log_info(message);
		break;
	case LOG_LEVEL_WARN:
		ctx->stra_log_warn(message);
		break;
	case LOG_LEVEL_ERROR:
		ctx->stra_log_error(message);
		break;
	default:
		break;
		}
}

void cta_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

WtString cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

void cta_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

void cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_bar_events(stdCode, period);
}

void cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->set_chart_kline(stdCode, period);
}

void cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->add_chart_mark(price, icon, tag);
}

void cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->register_index(idxName, indexType);
}

bool cta_register_index_line(CtxHandler cHandle, const char* idxName, const char* lineName, WtUInt32 lineType)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->register_index_line(idxName, lineName, lineType);
}
bool cta_add_index_baseline(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->add_index_baseline(idxName, lineName, val);
}

bool cta_set_index_value(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->set_index_value(idxName, lineName, val);
}


#pragma endregion

#pragma region "多因子策略接口"
CtxHandler create_sel_context(const char* name, uint32_t date, uint32_t time, const char* period, const char* trdtpl/* = "CHINA"*/, const char* session/* = "TRADING"*/, int32_t slippage/* = 0*/)
{
	return getRunner().createSelContext(name, date, time, period, slippage, trdtpl, session);
}


void sel_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

WtString sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

void sel_log_text(CtxHandler cHandle, WtUInt32 level, const char* message)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	switch (level)
	{
	case LOG_LEVEL_DEBUG:
		ctx->stra_log_debug(message);
		break;
	case LOG_LEVEL_INFO:
		ctx->stra_log_info(message);
		break;
	case LOG_LEVEL_WARN:
		ctx->stra_log_warn(message);
		break;
	case LOG_LEVEL_ERROR:
		ctx->stra_log_error(message);
		break;
	default:
		break;
	}
}

double sel_get_price(const char* stdCode)
{
	return getRunner().getEngine()->get_cur_price(stdCode);
}

WtUInt32 sel_get_date()
{
	return getRunner().getEngine()->get_date();
}

WtUInt32 sel_get_time()
{
	return getRunner().getEngine()->get_min_time();
}

void sel_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
	{
		cb(cHandle, "", 0, true);
		return;
	}

	ctx->enum_position([cb, cHandle](const char* stdCode, double qty) {
		cb(cHandle, stdCode, qty, false);
	});

	cb(cHandle, "", 0, true);
}

double sel_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

WtUInt32 sel_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSKlineSlice* kData = ctx->stra_get_bars(stdCode, period, barCnt);
		if (kData)
		{
			WtUInt32 reaCnt = (WtUInt32)kData->size();

			for (uint32_t i = 0; i < kData->get_block_counts(); i++)
				cb(cHandle, stdCode, period, kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts() - 1);

			kData->release();
			return reaCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

void sel_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	//多因子引擎,限价和止价都无效
	ctx->stra_set_position(stdCode, qty, userTag);
}

WtUInt32	sel_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSTickSlice* tData = ctx->stra_get_ticks(stdCode, tickCnt);
		if (tData)
		{
			uint32_t thisCnt = min(tickCnt, (WtUInt32)tData->size());
			if (thisCnt != 0)
				cb(cHandle, stdCode, (WTSTickStruct*)tData->at(0), thisCnt, true);
			else
				cb(cHandle, stdCode, NULL, 0, true);
			tData->release();
			return thisCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

void sel_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

double sel_get_day_price(const char* stdCode, int flag)
{
	return getRunner().getEngine()->get_day_price(stdCode, flag);
}

WtUInt32 sel_get_tdate()
{
	return getRunner().getEngine()->get_trading_date();
}

double sel_get_fund_data(CtxHandler cHandle, int flag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}

double sel_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

WtUInt64 sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

double sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

double sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

double sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

WtUInt64 sel_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

WtUInt64 sel_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

WtUInt64 sel_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

double sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

WtString sel_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}
#pragma endregion

#pragma region "HFT策略接口"
CtxHandler create_hft_context(const char* name, const char* trader, bool agent, int32_t slippage/* = 0*/)
{
	return getRunner().createHftContext(name, trader, agent, slippage);
}

double hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid);
}

double hft_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

double hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

double hft_get_undone(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_undone(stdCode);
}

double hft_get_price(const char* stdCode)
{
	return getRunner().getEngine()->get_cur_price(stdCode);
}

WtUInt32 hft_get_date()
{
	return getRunner().getEngine()->get_date();
}

WtUInt32 hft_get_time()
{
	return getRunner().getEngine()->get_raw_time();
}

WtUInt32 hft_get_secs()
{
	return getRunner().getEngine()->get_secs();
}

WtUInt32 hft_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	try
	{
		WTSKlineSlice* kData = ctx->stra_get_bars(stdCode, period, barCnt);
		if (kData)
		{
			WtUInt32 reaCnt = (WtUInt32)kData->size();

			for (uint32_t i = 0; i < kData->get_block_counts(); i++)
				cb(cHandle, stdCode, period, kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts() - 1);

			kData->release();
			return reaCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

WtUInt32 hft_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSTickSlice* tData = ctx->stra_get_ticks(stdCode, tickCnt);
		if (tData)
		{
			uint32_t thisCnt = min(tickCnt, (WtUInt32)tData->size());
			if (thisCnt != 0)
				cb(cHandle, stdCode, (WTSTickStruct*)tData->at(0), thisCnt, true);
			else
				cb(cHandle, stdCode, NULL, 0, true);
			tData->release();
			return thisCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

WtUInt32 hft_get_ordque(CtxHandler cHandle, const char* stdCode, WtUInt32 itemCnt, FuncGetOrdQueCallback cb)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSOrdQueSlice* dataSlice = ctx->stra_get_order_queue(stdCode, itemCnt);
		if (dataSlice)
		{
			uint32_t thisCnt = min(itemCnt, (WtUInt32)dataSlice->size());
			cb(cHandle, stdCode, (WTSOrdQueStruct*)dataSlice->at(0), thisCnt, true);
			dataSlice->release();
			return thisCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

WtUInt32 hft_get_orddtl(CtxHandler cHandle, const char* stdCode, WtUInt32 itemCnt, FuncGetOrdDtlCallback cb)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSOrdDtlSlice* dataSlice = ctx->stra_get_order_detail(stdCode, itemCnt);
		if (dataSlice)
		{
			uint32_t thisCnt = min(itemCnt, (WtUInt32)dataSlice->size());
			cb(cHandle, stdCode, (WTSOrdDtlStruct*)dataSlice->at(0), thisCnt, true);
			dataSlice->release();
			return thisCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

WtUInt32 hft_get_trans(CtxHandler cHandle, const char* stdCode, WtUInt32 itemCnt, FuncGetTransCallback cb)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;
	try
	{
		WTSTransSlice* dataSlice = ctx->stra_get_transaction(stdCode, itemCnt);
		if (dataSlice)
		{
			uint32_t thisCnt = min(itemCnt, (WtUInt32)dataSlice->size());
			cb(cHandle, stdCode, (WTSTransStruct*)dataSlice->at(0), thisCnt, true);
			dataSlice->release();
			return thisCnt;
		}
		else
		{
			return 0;
		}
	}
	catch (...)
	{
		return 0;
	}
}

void hft_log_text(CtxHandler cHandle, WtUInt32 level, const char* message)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	switch (level)
	{
	case LOG_LEVEL_DEBUG:
		ctx->stra_log_debug(message);
		break;
	case LOG_LEVEL_INFO:
		ctx->stra_log_info(message);
		break;
	case LOG_LEVEL_WARN:
		ctx->stra_log_warn(message);
		break;
	case LOG_LEVEL_ERROR:
		ctx->stra_log_error(message);
		break;
	default:
		break;
	}
}

void hft_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

void hft_sub_order_detail(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_order_details(stdCode);
}

void hft_sub_order_queue(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_order_queues(stdCode);
}

void hft_sub_transaction(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_transactions(stdCode);
}

bool hft_cancel(CtxHandler cHandle, WtUInt32 localid)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->stra_cancel(localid);
}

WtString hft_cancel_all(CtxHandler cHandle, const char* stdCode, bool isBuy)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return "";

	static thread_local std::string ret;

	std::stringstream ss;
	OrderIDs ids = ctx->stra_cancel(stdCode, isBuy, DBL_MAX);
	for(uint32_t localid : ids)
	{
		ss << localid << ",";
	}

	ret = ss.str();
	if (ret.size() > 0)
		ret = ret.substr(0, ret.size() - 1);
	return ret.c_str();
}

WtString hft_buy(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return "";

	static std::string ret;

	std::stringstream ss;
	OrderIDs ids = ctx->stra_buy(stdCode, price, qty, userTag, flag);
	for (uint32_t localid : ids)
	{
		ss << localid << ",";
	}

	ret = ss.str();
	if(ret.size() > 0)
		ret = ret.substr(0, ret.size() - 1);
	return ret.c_str();
}

WtString hft_sell(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return "";

	static std::string ret;

	std::stringstream ss;
	OrderIDs ids = ctx->stra_sell(stdCode, price, qty, userTag, flag);
	for (uint32_t localid : ids)
	{
		ss << localid << ",";
	}

	ret = ss.str();
	if (ret.size() > 0)
		ret = ret.substr(0, ret.size() - 1);
	return ret.c_str();
}

void hft_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

WtString hft_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}
#pragma endregion "HFT策略接口"

#pragma region "扩展Parser接口"
void parser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag)
{
	getRunner().on_ext_parser_quote(id, curTick, uProcFlag);
}
#pragma endregion "扩展Parser接口"