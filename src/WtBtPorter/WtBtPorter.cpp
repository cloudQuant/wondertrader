/*!
 * \file WtBtPorter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测引擎接口封装实现
 * \details 本文件实现了WtBtPorter.h中声明的回测引擎接口，为上层应用提供了CTA、SEL和HFT策略的回测功能
 */
#include "WtBtPorter.h"
#include "WtBtRunner.h"

#include "../WtBtCore/WtHelper.h"
#include "../WtBtCore/CtaMocker.h"
#include "../WtBtCore/SelMocker.h"
#include "../WtBtCore/HftMocker.h"

#include "../WTSTools/WTSLogger.h"

#include "../Includes/WTSVersion.h"


#ifdef _WIN32
#ifdef _WIN64
char PLATFORM_NAME[] = "X64";
#else
char PLATFORM_NAME[] = "X86";
#endif
#else
char PLATFORM_NAME[] = "UNIX";
#endif


/**
 * @brief 获取回测引擎单例
 * @details 使用单例模式获取WtBtRunner实例，确保整个程序中只有一个回测引擎实例
 * @return WtBtRunner& 返回回测引擎单例的引用
 */
WtBtRunner& getRunner()
{
	static WtBtRunner runner;
	return runner;
}

/**
 * @brief 注册事件回调函数
 * @details 将事件回调函数注册到回测引擎中，用于接收回测过程中的事件通知
 * @param cbEvt 事件回调函数指针
 */
void register_evt_callback(FuncEventCallback cbEvt)
{
	getRunner().registerEvtCallback(cbEvt);
}

/**
 * @brief 注册CTA策略回调函数
 * @details 将CTA策略的各种回调函数注册到回测引擎中
 * @param cbInit 策略初始化回调函数
 * @param cbTick Tick数据到达回调函数
 * @param cbCalc 策略计算回调函数
 * @param cbBar K线数据到达回调函数
 * @param cbSessEvt 交易时段事件回调函数
 * @param cbCalcDone 计算完成回调函数，默认为NULL
 * @param cbCondTrigger 条件触发回调函数
 */
void register_cta_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
	FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone/* = NULL*/, FuncStraCondTriggerCallback cbCondTrigger)
{
	getRunner().registerCtaCallbacks(cbInit, cbTick, cbCalc, cbBar, cbSessEvt, cbCalcDone, cbCondTrigger);
}

/**
 * @brief 注册选股策略回调函数
 * @details 将选股策略的各种回调函数注册到回测引擎中
 * @param cbInit 策略初始化回调函数
 * @param cbTick Tick数据到达回调函数
 * @param cbCalc 策略计算回调函数
 * @param cbBar K线数据到达回调函数
 * @param cbSessEvt 交易时段事件回调函数
 * @param cbCalcDone 计算完成回调函数，默认为NULL
 */
void register_sel_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
	FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone/* = NULL*/)
{
	getRunner().registerSelCallbacks(cbInit, cbTick, cbCalc, cbBar, cbSessEvt, cbCalcDone);
}

/**
 * @brief 注册高频策略回调函数
 * @details 将高频策略的各种回调函数注册到回测引擎中
 * @param cbInit 策略初始化回调函数
 * @param cbTick Tick数据到达回调函数
 * @param cbBar K线数据到达回调函数
 * @param cbChnl 通道数据回调函数
 * @param cbOrd 订单回调函数
 * @param cbTrd 成交回调函数
 * @param cbEntrust 委托回调函数
 * @param cbOrdDtl 逆序委托明细回调函数
 * @param cbOrdQue 逆序委托队列回调函数
 * @param cbTrans 逆序逐笔成交回调函数
 * @param cbSessEvt 交易时段事件回调函数
 */
void register_hft_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar,
	FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
	FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt)
{
	getRunner().registerHftCallbacks(cbInit, cbTick, cbBar, cbChnl, cbOrd, cbTrd, cbEntrust, cbOrdDtl, cbOrdQue, cbTrans, cbSessEvt);
}

/**
 * @brief 注册外部数据加载器
 * @details 将外部数据加载器注册到回测引擎中，用于加载历史数据
 * @param fnlBarLoader 完整K线加载器函数
 * @param rawBarLoader 原始K线加载器函数
 * @param fctLoader 除权因子加载器函数
 * @param tickLoader Tick数据加载器函数
 * @param bAutoTrans 是否自动转换数据
 */
void register_ext_data_loader(FuncLoadFnlBars fnlBarLoader, FuncLoadRawBars rawBarLoader, FuncLoadAdjFactors fctLoader, FuncLoadRawTicks tickLoader, bool bAutoTrans)
{
	getRunner().registerExtDataLoader(fnlBarLoader, rawBarLoader, fctLoader, tickLoader, bAutoTrans);
}

/**
 * @brief 向回测引擎输入原始K线数据
 * @details 将外部加载的原始K线数据输入到回测引擎中进行处理
 * @param bars K线数据结构指针
 * @param count K线数据数量
 */
void feed_raw_bars(WTSBarStruct* bars, WtUInt32 count)
{
	getRunner().feedRawBars(bars, count);
}

/**
 * @brief 向回测引擎输入原始Tick数据
 * @details 将外部加载的原始Tick数据输入到回测引擎中进行处理
 * @param ticks Tick数据结构指针
 * @param count Tick数据数量
 */
void feed_raw_ticks(WTSTickStruct* ticks, WtUInt32 count)
{
	getRunner().feedRawTicks(ticks, count);
}

/**
 * @brief 初始化回测引擎
 * @details 初始化回测引擎的环境，包括日志配置和输出目录等设置
 * @param logProfile 日志配置文件或内容
 * @param isFile 是否为文件路径，如果为true则logProfile为文件路径，否则为配置内容
 * @param outDir 输出目录
 * @note 该函数只会执行一次，重复调用会被忽略
 */
void init_backtest(const char* logProfile, bool isFile, const char* outDir)
{
	static bool inited = false;

	if (inited)
		return;

	getRunner().init(logProfile, isFile, outDir);

	inited = true;
}

/**
 * @brief 配置回测引擎
 * @details 设置回测引擎的配置参数，包括交易日历、品种信息等
 * @param cfgfile 配置文件路径或配置内容字符串
 * @param isFile 是否为文件路径，如果为true则cfgfile为文件路径，否则为配置内容
 * @note 该函数只会执行一次，重复调用会被忽略。如果cfgfile为空，则使用默认配置文件"configbt.json"
 */
void config_backtest(const char* cfgfile, bool isFile)
{
	static bool inited = false;

	if (inited)
		return;

	if (strlen(cfgfile) == 0)
		getRunner().config("configbt.json", true);
	else
		getRunner().config(cfgfile, isFile);
}

/**
 * @brief 设置回测的时间范围
 * @details 设置回测起止时间，用于控制回测的时间范围
 * @param stime 开始时间，格式为YYYYMMDDHHmmss
 * @param etime 结束时间，格式YYYYMMDDHHmmss
 */
void set_time_range(WtUInt64 stime, WtUInt64 etime)
{
	getRunner().set_time_range(stime, etime);
}

/**
 * @brief 启用Tick数据回测
 * @details 设置是否启用Tick数据进行回测，如果启用则使用Tick级别回测，否则使用K线回测
 * @param bEnabled 是否启用Tick数据回测，默认为true
 */
void enable_tick(bool bEnabled /* = true */)
{
	getRunner().enable_tick(bEnabled);
}

/**
 * @brief 运行回测
 * @details 启动回测引擎并运行回测过程
 * @param bNeedDump 是否需要输出回测结果
 * @param bAsync 是否使用异步模式运行回测
 */
void run_backtest(bool bNeedDump, bool bAsync)
{
	getRunner().run(bNeedDump, bAsync);
}

/**
 * @brief 停止回测
 * @details 停止正在运行的回测过程，主要用于异步模式下的回测控制
 */
void stop_backtest()
{
	getRunner().stop();
}

/**
 * @brief 释放回测资源
 * @details 释放回测引擎占用的内存和资源，在回测结束后调用
 */
void release_backtest()
{
	getRunner().release();
}

/**
 * @brief 获取原始标准代码
 * @details 将组合商品代码转换为原始标准代码
 * @param stdCode 标准代码
 * @return WtString 原始标准代码
 */
WtString get_raw_stdcode(const char* stdCode)
{
	return getRunner().get_raw_stdcode(stdCode);
}

/**
 * @brief 获取版本信息
 * @details 获取WonderTrader的版本信息，包括平台、版本号和构建时间
 * @return const char* 版本信息字符串
 */
const char* get_version()
{
	static std::string _ver;
	if(_ver.empty())
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

/**
 * @brief 清理缓存
 * @details 清理回测引擎中的数据缓存，释放内存资源
 */
void clear_cache()
{
	getRunner().clear_cache();
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

CtxHandler init_cta_mocker(const char* name, int slippage/* = 0*/, bool hook/* = false*/, bool persistData/* = true*/, bool bIncremental/* = false*/, bool bRatioSlp/* = false*/)
{
	return getRunner().initCtaMocker(name, slippage, hook, persistData, bIncremental, bRatioSlp);
}

CtxHandler init_hft_mocker(const char* name, bool hook/* = false*/)
{
	return getRunner().initHftMocker(name, hook);
}

CtxHandler init_sel_mocker(const char* name, WtUInt32 date, WtUInt32 time, const char* period, const char* trdtpl/* = "CHINA"*/, const char* session/* = "TRADING"*/, int slippage/* = 0*/, bool bRatioSlp/* = false*/)
{
	return getRunner().initSelMocker(name, date, time, period, trdtpl, session, slippage, bRatioSlp);
}

#pragma region "CTA策略接口"
void cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_enter_long(stdCode, qty, userTag, limitprice, stopprice);
}

void cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_exit_long(stdCode, qty, userTag, limitprice, stopprice);
}

void cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_enter_short(stdCode, qty, userTag, limitprice, stopprice);
}

void cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_exit_short(stdCode, qty, userTag, limitprice, stopprice);
}

WtUInt32 cta_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, bool isMain, FuncGetBarsCallback cb)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;
	try
	{
		WTSKlineSlice* kData = ctx->stra_get_bars(stdCode, period, barCnt, isMain);
		if (kData)
		{
			WtUInt32 reaCnt = (WtUInt32)kData->size();

			for (uint32_t i = 0; i < kData->get_block_counts(); i++)
				cb(cHandle, stdCode, period, kData->get_block_addr(i), kData->get_block_size(i), i == kData->get_block_counts()-1);

			kData->release();
			return reaCnt;
		}
		else
		{
			return 0;
		}
	}
	catch(...)
	{
		return 0;
	}
}

WtUInt32	cta_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb)
{
	CtaMocker* ctx = getRunner().cta_mocker();
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

double cta_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

WtUInt64 cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

double cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

double cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

double cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

void cta_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
	{
		cb(cHandle, "", 0, true);
		return;
	}

	ctx->enum_position([cb, cHandle](const char* stdCode, double qty) {
		cb(cHandle, stdCode, qty, false);
	}, false);

	cb(cHandle, "", 0, true);
}

double cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

double cta_get_fund_data(CtxHandler cHandle, int flag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}

void cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_set_position(stdCode, qty, userTag, limitprice, stopprice);
}

WtUInt64 cta_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

WtUInt64 cta_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

WtUInt64 cta_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

double cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

WtString cta_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}

double cta_get_price(const char* stdCode)
{
	return getRunner().replayer().get_cur_price(stdCode);
}

double cta_get_day_price(const char* stdCode, int flag)
{
	return getRunner().replayer().get_day_price(stdCode, flag);
}

WtUInt32 cta_get_tdate()
{
	return getRunner().replayer().get_trading_date();
}

WtUInt32 cta_get_date()
{
	return getRunner().replayer().get_date();
}

WtUInt32 cta_get_time()
{
	return getRunner().replayer().get_min_time();
}

void cta_log_text(CtxHandler cHandle, WtUInt32 level, const char* message)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	switch(level)
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
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

WtString cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

void cta_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return ;

	ctx->stra_sub_ticks(stdCode);
}

void cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_sub_bar_events(stdCode, period);
}

bool cta_step(CtxHandler cHandle)
{
	//只有异步模式才有意义
	if (!getRunner().isAsync())
		return false;

	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->step_calc();
}

void cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->set_chart_kline(stdCode, period);
}

void cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->add_chart_mark(price, icon, tag);
}

void cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->register_index(idxName, indexType);
}

bool cta_register_index_line(CtxHandler cHandle, const char* idxName, const char* lineName, WtUInt32 lineType)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->register_index_line(idxName, lineName, lineType);
}
bool cta_add_index_baseline(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->add_index_baseline(idxName, lineName, val);
}

bool cta_set_index_value(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->set_index_value(idxName, lineName, val);
}

#pragma endregion "CTA策略接口"

#pragma region "SEL策略接口"
void sel_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

WtString sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

void sel_log_text(CtxHandler cHandle, WtUInt32 level, const char* message)
{
	SelMocker* ctx = getRunner().sel_mocker();
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
	return getRunner().replayer().get_cur_price(stdCode);
}

WtUInt32 sel_get_date()
{
	return getRunner().replayer().get_date();
}

WtUInt32 sel_get_time()
{
	return getRunner().replayer().get_min_time();
}

void sel_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb)
{
	SelMocker* ctx = getRunner().sel_mocker();
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
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

WtUInt32 sel_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb)
{
	SelMocker* ctx = getRunner().sel_mocker();
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
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return;

	//多因子引擎,限价和止价都无效
	ctx->stra_set_position(stdCode, qty, userTag);
}

WtUInt32	sel_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb)
{
	SelMocker* ctx = getRunner().sel_mocker();
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
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

double sel_get_day_price(const char* stdCode, int flag)
{
	return getRunner().replayer().get_day_price(stdCode, flag);
}

WtUInt32 sel_get_tdate()
{
	return getRunner().replayer().get_trading_date();
}

double sel_get_fund_data(CtxHandler cHandle, int flag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}

double sel_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

WtUInt64 sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

double sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

double sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

double sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

WtUInt64 sel_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

WtUInt64 sel_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

WtUInt64 sel_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

double sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

WtString sel_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}

#pragma endregion "SEL策略接口"

#pragma region "HFT策略接口"
double hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_position(stdCode, bOnlyValid);
}

double hft_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_position_profit(stdCode);
}

double hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_position_avgpx(stdCode);
}


double hft_get_undone(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_undone(stdCode);
}

double hft_get_price(const char* stdCode)
{
	return getRunner().replayer().get_cur_price(stdCode);
}

WtUInt32 hft_get_date()
{
	return getRunner().replayer().get_date();
}

WtUInt32 hft_get_time()
{
	return getRunner().replayer().get_raw_time();
}

WtUInt32 hft_get_secs()
{
	return getRunner().replayer().get_secs();
}

WtUInt32 hft_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	try
	{
		WTSKlineSlice* kData = mocker->stra_get_bars(stdCode, period, barCnt);
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
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;
	try
	{
		WTSTickSlice* tData = mocker->stra_get_ticks(stdCode, tickCnt);
		if (tData)
		{
			uint32_t thisCnt = min(tickCnt, (WtUInt32)tData->size());
			if(thisCnt != 0)
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
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;
	try
	{
		WTSOrdQueSlice* dataSlice = mocker->stra_get_order_queue(stdCode, itemCnt);
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
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;
	try
	{
		WTSOrdDtlSlice* dataSlice = mocker->stra_get_order_detail(stdCode, itemCnt);
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
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;
	try
	{
		WTSTransSlice* dataSlice = mocker->stra_get_transaction(stdCode, itemCnt);
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
	HftMocker* ctx = getRunner().hft_mocker();
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
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_ticks(stdCode);
}

void hft_sub_order_detail(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_order_details(stdCode);
}

void hft_sub_order_queue(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_order_queues(stdCode);
}

void hft_sub_transaction(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_transactions(stdCode);
}

bool hft_cancel(CtxHandler cHandle, WtUInt32 localid)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return false;

	return mocker->stra_cancel(localid);
}

WtString hft_cancel_all(CtxHandler cHandle, const char* stdCode, bool isBuy)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return "";

	static thread_local std::string ret;

	std::stringstream ss;
	OrderIDs ids = mocker->stra_cancel(stdCode, isBuy, DBL_MAX);
	for (WtUInt32 localid : ids)
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
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return "";

	static std::string ret;

	std::stringstream ss;
	OrderIDs ids = mocker->stra_buy(stdCode, price, qty, userTag, flag);
	for (WtUInt32 localid : ids)
	{
		ss << localid << ",";
	}

	ret = ss.str();
	ret = ret.substr(0, ret.size() - 1);
	return ret.c_str();
}

WtString hft_sell(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return "";

	static std::string ret;

	std::stringstream ss;
	OrderIDs ids = mocker->stra_sell(stdCode, price, qty, userTag, flag);
	for (WtUInt32 localid : ids)
	{
		ss << localid << ",";
	}

	ret = ss.str();
	ret = ret.substr(0, ret.size() - 1);
	return ret.c_str();
}

void hft_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_save_user_data(key, val);
}

WtString hft_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return defVal;

	return mocker->stra_load_user_data(key, defVal);
}

void hft_step(CtxHandler cHandle)
{
	//只有异步模式才有意义
	if (!getRunner().isAsync())
		return;

	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->step_tick();
}
#pragma endregion "HFT策略接口"