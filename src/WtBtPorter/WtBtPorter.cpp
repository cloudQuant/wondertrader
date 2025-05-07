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

/**
 * @brief 写入日志
 * @details 将日志信息写入到日志系统中，可指定日志级别和分类
 * @param level 日志级别，对应WTSLogLevel枚举
 * @param message 日志消息内容
 * @param catName 日志分类名称，如果为空则使用默认分类
 */
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

/**
 * @brief 初始化CTA策略模拟器
 * @details 创建并初始化一个CTA策略的模拟器实例，用于回测过程
 * @param name 策略名称
 * @param slippage 滑点设置，默认为0
 * @param hook 是否启用钩子函数，默认为false
 * @param persistData 是否持久化数据，默认为true
 * @param bIncremental 是否为增量模式，默认为false
 * @param bRatioSlp 是否使用比例滑点，默认为false
 * @return CtxHandler 策略上下文句柄
 */
CtxHandler init_cta_mocker(const char* name, int slippage/* = 0*/, bool hook/* = false*/, bool persistData/* = true*/, bool bIncremental/* = false*/, bool bRatioSlp/* = false*/)
{
	return getRunner().initCtaMocker(name, slippage, hook, persistData, bIncremental, bRatioSlp);
}

/**
 * @brief 初始化高频策略模拟器
 * @details 创建并初始化一个高频策略的模拟器实例，用于回测过程
 * @param name 策略名称
 * @param hook 是否启用钩子函数，默认为false
 * @return CtxHandler 策略上下文句柄
 */
CtxHandler init_hft_mocker(const char* name, bool hook/* = false*/)
{
	return getRunner().initHftMocker(name, hook);
}

/**
 * @brief 初始化选股策略模拟器
 * @details 创建并初始化一个选股策略的模拟器实例，用于回测过程
 * @param name 策略名称
 * @param date 日期，格式为YYYYMMDD
 * @param time 时间，格式为HHMMSS或HHMMSS000
 * @param period 周期标识符，如"d1"表示日线
 * @param trdtpl 交易模板，默认为"CHINA"
 * @param session 交易时段，默认为"TRADING"
 * @param slippage 滑点设置，默认为0
 * @param bRatioSlp 是否使用比例滑点，默认为false
 * @return CtxHandler 策略上下文句柄
 */
CtxHandler init_sel_mocker(const char* name, WtUInt32 date, WtUInt32 time, const char* period, const char* trdtpl/* = "CHINA"*/, const char* session/* = "TRADING"*/, int slippage/* = 0*/, bool bRatioSlp/* = false*/)
{
	return getRunner().initSelMocker(name, date, time, period, trdtpl, session, slippage, bRatioSlp);
}

#pragma region "CTA策略接口"
/**
 * @brief CTA策略做多入场
 * @details 在CTA策略中执行做多入场操作，可以指定限价和止损价
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param qty 交易数量
 * @param userTag 用户自定义标签，用于识别交易
 * @param limitprice 限价，为0则为市价单
 * @param stopprice 止损价，为0则不设止损
 */
void cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_enter_long(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief CTA策略平多出场
 * @details 在CTA策略中执行平多出场操作，可以指定限价和止损价
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param qty 交易数量
 * @param userTag 用户自定义标签，用于识别交易
 * @param limitprice 限价，为0则为市价单
 * @param stopprice 止损价，为0则不设止损
 */
void cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_exit_long(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief CTA策略做空入场
 * @details 在CTA策略中执行做空入场操作，可以指定限价和止损价
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param qty 交易数量
 * @param userTag 用户自定义标签，用于识别交易
 * @param limitprice 限价，为0则为市价单
 * @param stopprice 止损价，为0则不设止损
 */
void cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_enter_short(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief CTA策略平空出场
 * @details 在CTA策略中执行平空出场操作，可以指定限价和止损价
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param qty 交易数量
 * @param userTag 用户自定义标签，用于识别交易
 * @param limitprice 限价，为0则为市价单
 * @param stopprice 止损价，为0则不设止损
 */
void cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_exit_short(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief 获取K线数据
 * @details 在CTA策略中获取指定合约的K线数据，并通过回调函数返回
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param period 周期标识符，如"d1"表示日线
 * @param barCnt 请求的K线数量
 * @param isMain 是否为主图指标
 * @param cb 获取K线数据的回调函数
 * @return WtUInt32 实际返回的K线数量
 */
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

/**
 * @brief 获取Tick数据
 * @details 在CTA策略中获取指定合约的Tick数据，并通过回调函数返回
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param tickCnt 请求的Tick数量
 * @param cb 获取Tick数据的回调函数
 * @return WtUInt32 实际返回的Tick数量
 */
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

/**
 * @brief 获取持仓盈亏
 * @details 在CTA策略中获取指定合约的持仓盈亏
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 持仓盈亏金额
 */
double cta_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

/**
 * @brief 获取详细入场时间
 * @details 在CTA策略中获取指定合约和标签的入场时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param openTag 开仓标签
 * @return WtUInt64 入场时间，格式为YYYYMMDDHHmmss
 */
WtUInt64 cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

/**
 * @brief 获取详细开仓成本
 * @details 在CTA策略中获取指定合约和标签的开仓成本
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param openTag 开仓标签
 * @return double 开仓成本
 */
double cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

/**
 * @brief 获取详细盈亏
 * @details 在CTA策略中获取指定合约和标签的盈亏情况
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param openTag 开仓标签
 * @param flag 盈亏标志，0-浮动盈亏，1-平仓盈亏
 * @return double 盈亏金额
 */
double cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

/**
 * @brief 获取持仓均价
 * @details 在CTA策略中获取指定合约的持仓均价
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 持仓均价
 */
double cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

/**
 * @brief 获取所有持仓
 * @details 在CTA策略中获取所有合约的持仓情况，通过回调函数返回
 * @param cHandle 策略上下文句柄
 * @param cb 获取持仓的回调函数，会多次调用，每次返回一个合约的持仓，最后一次调用的标准代码为空字符串，表示枚举结束
 */
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

/**
 * @brief 获取持仓量
 * @details 在CTA策略中获取指定合约的持仓量
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只返回有效持仓
 * @param openTag 开仓标签，如果不为空，则只返回指定标签的持仓
 * @return double 持仓量，正数表示多头持仓，负数表示空头持仓
 */
double cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

/**
 * @brief 获取资金数据
 * @details 在CTA策略中获取资金相关数据，如动态权益、静态权益等
 * @param cHandle 策略上下文句柄
 * @param flag 资金数据标志，0-动态权益，1-静态权益，2-浮动盈亏
 * @return double 资金数据值
 */
double cta_get_fund_data(CtxHandler cHandle, int flag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}

/**
 * @brief 设置目标仓位
 * @details 在CTA策略中设置指定合约的目标仓位，系统会自动根据当前仓位进行调整
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param qty 目标仓位量，正数表示多头仓位，负数表示空头仓位
 * @param userTag 用户自定义标签
 * @param limitprice 限价，为0则为市价单
 * @param stopprice 止损价，为0则不设止损
 */
void cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_set_position(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief 获取首次入场时间
 * @details 在CTA策略中获取指定合约的首次入场时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtUInt64 首次入场时间，格式为YYYYMMDDHHmmss
 */
WtUInt64 cta_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

/**
 * @brief 获取最近入场时间
 * @details 在CTA策略中获取指定合约的最后一次入场时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtUInt64 最近入场时间，格式为YYYYMMDDHHmmss
 */
WtUInt64 cta_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

/**
 * @brief 获取最近出场时间
 * @details 在CTA策略中获取指定合约的最后一次出场时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtUInt64 最近出场时间，格式为YYYYMMDDHHmmss
 */
WtUInt64 cta_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

/**
 * @brief 获取最近入场价格
 * @details 在CTA策略中获取指定合约的最近一次入场价格
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 最近入场价格
 */
double cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

/**
 * @brief 获取最近入场标签
 * @details 在CTA策略中获取指定合约的最近一次入场标签
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtString 最近入场标签
 */
WtString cta_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}

/**
 * @brief 获取当前合约价格
 * @details 获取当前回测时点指定合约的最新价格
 * @param stdCode 标准合约代码
 * @return double 当前合约价格
 */
double cta_get_price(const char* stdCode)
{
	return getRunner().replayer().get_cur_price(stdCode);
}

/**
 * @brief 获取合约日线价格
 * @details 获取当前交易日的指定合约的开盘价/收盘价/最高价/最低价
 * @param stdCode 标准合约代码
 * @param flag 价格标志，0-开盘价，1-收盘价，2-最高价，3-最低价
 * @return double 对应的价格
 */
double cta_get_day_price(const char* stdCode, int flag)
{
	return getRunner().replayer().get_day_price(stdCode, flag);
}

/**
 * @brief 获取当前交易日
 * @details 获取当前回测时点的交易日期
 * @return WtUInt32 交易日期，格式为YYYYMMDD
 */
WtUInt32 cta_get_tdate()
{
	return getRunner().replayer().get_trading_date();
}

/**
 * @brief 获取当前自然日
 * @details 获取当前回测时点的自然日期（公历日）
 * @return WtUInt32 自然日期，格式为YYYYMMDD
 */
WtUInt32 cta_get_date()
{
	return getRunner().replayer().get_date();
}

/**
 * @brief 获取当前时间
 * @details 获取当前回测时点的时间
 * @return WtUInt32 时间，格式为HHMMSS或HHMMSS000
 */
WtUInt32 cta_get_time()
{
	return getRunner().replayer().get_min_time();
}

/**
 * @brief CTA策略日志输出
 * @details 在CTA策略中输出指定级别的日志
 * @param cHandle 策略上下文句柄
 * @param level 日志级别，LOG_LEVEL_DEBUG/LOG_LEVEL_INFO/LOG_LEVEL_WARN/LOG_LEVEL_ERROR
 * @param message 日志内容
 */
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

/**
 * @brief 保存用户数据
 * @details 在CTA策略中保存用户自定义数据，可用于存储策略运行状态
 * @param cHandle 策略上下文句柄
 * @param key 数据键名
 * @param val 数据值
 */
void cta_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

/**
 * @brief 加载用户数据
 * @details 在CTA策略中加载用户自定义数据，用于恢复策略运行状态
 * @param cHandle 策略上下文句柄
 * @param key 数据键名
 * @param defVal 默认值，当指定键名的数据不存在时返回此值
 * @return WtString 加载的数据值
 */
WtString cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

/**
 * @brief 订阅合约的tick数据
 * @details 在CTA策略中订阅指定合约的tick实时行情数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 */
void cta_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return ;

	ctx->stra_sub_ticks(stdCode);
}

/**
 * @brief 订阅K线事件
 * @details 在CTA策略中订阅指定合约和周期的K线事件，当新的K线形成时会收到通知
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param period K线周期，如m1/m5/d1等
 */
void cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_sub_bar_events(stdCode, period);
}

/**
 * @brief 执行一步策略计算
 * @details 在异步模式下执行策略的单步计算，用于手动控制回测步伐
 * @param cHandle 策略上下文句柄
 * @return bool 返回计算是否成功
 */
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

/**
 * @brief 设置图表K线
 * @details 在CTA策略中设置图表显示的K线，用于回测结果可视化
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param period K线周期，如m1/m5/d1等
 */
void cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->set_chart_kline(stdCode, period);
}

/**
 * @brief 添加图表标记
 * @details 在CTA策略图表上添加标记，用于标记重要价格位置或交易信号
 * @param cHandle 策略上下文句柄
 * @param price 标记所在的价格位置
 * @param icon 标记图标，如"buy"/"sell"等
 * @param tag 标记标签，用于显示标记的说明文字
 */
void cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->add_chart_mark(price, icon, tag);
}

/**
 * @brief 注册指标
 * @details 在CTA策略中注册一个自定义指标，用于图表展示或分析
 * @param cHandle 策略上下文句柄
 * @param idxName 指标名称
 * @param indexType 指标类型
 */
void cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return;

	ctx->register_index(idxName, indexType);
}

/**
 * @brief 注册指标线
 * @details 在CTA策略中为已注册的指标添加一条线型数据
 * @param cHandle 策略上下文句柄
 * @param idxName 指标名称
 * @param lineName 线型名称
 * @param lineType 线型类型
 * @return bool 返回注册是否成功
 */
bool cta_register_index_line(CtxHandler cHandle, const char* idxName, const char* lineName, WtUInt32 lineType)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->register_index_line(idxName, lineName, lineType);
}
/**
 * @brief 添加指标基准线
 * @details 在CTA策略指标中添加一条基准线，用于参考或对比
 * @param cHandle 策略上下文句柄
 * @param idxName 指标名称
 * @param lineName 基准线名称
 * @param val 基准线值
 * @return bool 返回添加是否成功
 */
bool cta_add_index_baseline(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->add_index_baseline(idxName, lineName, val);
}

/**
 * @brief 设置指标线数据值
 * @details 在CTA策略中设置指定指标线的当前数据值
 * @param cHandle 策略上下文句柄
 * @param idxName 指标名称
 * @param lineName 线型名称
 * @param val 数据值
 * @return bool 返回设置是否成功
 */
bool cta_set_index_value(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaMocker* ctx = getRunner().cta_mocker();
	if (ctx == NULL)
		return false;

	return ctx->set_index_value(idxName, lineName, val);
}

#pragma endregion "CTA策略接口"

#pragma region "SEL策略接口"
/**
 * @brief 保存SEL策略用户数据
 * @details 在选股策略中保存用户自定义数据，可用于存储策略运行状态
 * @param cHandle 策略上下文句柄
 * @param key 数据键名
 * @param val 数据值
 */
void sel_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

/**
 * @brief 加载SEL策略用户数据
 * @details 在选股策略中加载用户自定义数据，用于恢复策略运行状态
 * @param cHandle 策略上下文句柄
 * @param key 数据键名
 * @param defVal 默认值，当指定键名的数据不存在时返回此值
 * @return WtString 加载的数据值
 */
WtString sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

/**
 * @brief SEL策略日志输出
 * @details 在选股策略中输出指定级别的日志
 * @param cHandle 策略上下文句柄
 * @param level 日志级别，LOG_LEVEL_DEBUG/LOG_LEVEL_INFO/LOG_LEVEL_WARN/LOG_LEVEL_ERROR
 * @param message 日志内容
 */
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

/**
 * @brief 获取SEL策略合约当前价格
 * @details 在选股策略中获取当前回测时点指定合约的最新价格
 * @param stdCode 标准合约代码
 * @return double 当前合约价格
 */
double sel_get_price(const char* stdCode)
{
	return getRunner().replayer().get_cur_price(stdCode);
}

/**
 * @brief 获取SEL策略当前日期
 * @details 在选股策略中获取当前回测时点的自然日期（公历日）
 * @return WtUInt32 当前日期，格式为YYYYMMDD
 */
WtUInt32 sel_get_date()
{
	return getRunner().replayer().get_date();
}

/**
 * @brief 获取SEL策略当前时间
 * @details 在选股策略中获取当前回测时点的时间
 * @return WtUInt32 当前时间，格式为HHMMSS或HHMMSS000
 */
WtUInt32 sel_get_time()
{
	return getRunner().replayer().get_min_time();
}

/**
 * @brief 获取SEL策略所有持仓
 * @details 在选股策略中获取并枚举所有合约的当前持仓量
 * @param cHandle 策略上下文句柄
 * @param cb 持仓回调函数，用于接收持仓数据
 */
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

/**
 * @brief 获取SEL策略持仓
 * @details 在选股策略中获取指定合约和标签的当前持仓量
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只计算可用持仓（非冲销中）
 * @param openTag 开仓标签，用于区分不同的开仓来源
 * @return double 合约持仓量，正数为多头仓位，负数为空头仓位
 */
double sel_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

/**
 * @brief 获取SEL策略K线数据
 * @details 在选股策略中获取指定合约和周期的历史K线数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param period K线周期，如m1/m5/d1等
 * @param barCnt 请求的K线条数
 * @param cb K线数据回调函数，用于接收K线数据
 * @return WtUInt32 实际返回的K线条数
 */
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

/**
 * @brief 设置SEL策略持仓
 * @details 在选股策略中直接设置指定合约的目标仓位
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param qty 目标仓位，正数表示多头仓位，负数表示空头仓位
 * @param userTag 用户自定义标签，用于标记仓位的来源
 */
void sel_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return;

	//多因子引擎,限价和止价都无效
	ctx->stra_set_position(stdCode, qty, userTag);
}

/**
 * @brief 获取SEL策略Tick数据
 * @details 在选股策略中获取指定合约的历史Tick数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param tickCnt 请求的Tick数据条数
 * @param cb Tick数据回调函数，用于接收Tick数据
 * @return WtUInt32 实际返回的Tick数据条数
 */
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

/**
 * @brief 订阅SEL策略Tick数据
 * @details 在选股策略中订阅指定合约的tick实时行情数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 */
void sel_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

/**
 * @brief 获取SEL策略日线价格
 * @details 在选股策略中获取当前交易日的指定合约的开盘价/收盘价/最高价/最低价
 * @param stdCode 标准合约代码
 * @param flag 价格标志，0-开盘价，1-收盘价，2-最高价，3-最低价
 * @return double 对应的价格
 */
double sel_get_day_price(const char* stdCode, int flag)
{
	return getRunner().replayer().get_day_price(stdCode, flag);
}

/**
 * @brief 获取SEL策略当前交易日
 * @details 在选股策略中获取当前回测时点的交易日期
 * @return WtUInt32 交易日期，格式为YYYYMMDD
 */
WtUInt32 sel_get_tdate()
{
	return getRunner().replayer().get_trading_date();
}

/**
 * @brief 获取SEL策略资金数据
 * @details 在选股策略中获取资金相关数据，如动态权益、各类资金指标等
 * @param cHandle 策略上下文句柄
 * @param flag 资金数据标志，0-动态权益，1-静态权益，2-各类资金指标
 * @return double 对应的资金数据值
 */
double sel_get_fund_data(CtxHandler cHandle, int flag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}

/**
 * @brief 获取SEL策略持仓盈亏
 * @details 在选股策略中获取指定合约的持仓盈亏
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 持仓盈亏数值
 */
double sel_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

/**
 * @brief 获取SEL策略持仓明细入场时间
 * @details 在选股策略中获取指定合约和开仓标签的入场时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param openTag 开仓标签
 * @return WtUInt64 入场时间，格式为时间戳
 */
WtUInt64 sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

/**
 * @brief 获取SEL策略持仓明细成本
 * @details 在选股策略中获取指定合约和开仓标签的持仓成本
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param openTag 开仓标签
 * @return double 持仓成本
 */
double sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

/**
 * @brief 获取SEL策略持仓明细盈亏
 * @details 在选股策略中获取指定合约和开仓标签的持仓盈亏
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param openTag 开仓标签
 * @param flag 盈亏标志，0-浮动盈亏，1-平仓盈亏
 * @return double 持仓盈亏
 */
double sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

/**
 * @brief 获取SEL策略持仓平均价
 * @details 在选股策略中获取指定合约的当前持仓平均价
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 持仓平均价
 */
double sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

/**
 * @brief 获取SEL策略首次入场时间
 * @details 在选股策略中获取指定合约的首次入场（开仓）交易的时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtUInt64 入场时间，格式为时间戳
 */
WtUInt64 sel_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

/**
 * @brief 获取SEL策略最近一次入场时间
 * @details 在选股策略中获取指定合约的最近一次入场（开仓）交易的时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtUInt64 入场时间，格式为时间戳
 */
WtUInt64 sel_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

/**
 * @brief 获取SEL策略最近一次离场时间
 * @details 在选股策略中获取指定合约的最近一次离场（平仓）交易的时间
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtUInt64 离场时间，格式为时间戳
 */
WtUInt64 sel_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

/**
 * @brief 获取SEL策略最近一次入场价格
 * @details 在选股策略中获取指定合约的最近一次入场交易的价格
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 入场价格
 */
double sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

/**
 * @brief 获取SEL策略最近一次入场标签
 * @details 在选股策略中获取指定合约的最近一次入场交易的标签
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return WtString 入场标签字符串
 */
WtString sel_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	SelMocker* ctx = getRunner().sel_mocker();
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}

#pragma endregion "SEL策略接口"

#pragma region "HFT策略接口"
/**
 * @brief 获取HFT策略持仓
 * @details 在高频策略中获取指定合约的当前持仓量
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只计算可用持仓（非冲销中）
 * @return double 合约持仓量，正数为多头仓位，负数为空头仓位
 */
double hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_position(stdCode, bOnlyValid);
}

/**
 * @brief 获取HFT策略持仓盈亏
 * @details 在高频策略中获取指定合约当前持仓的浮动盈亏
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 当前持仓浮动盈亏
 */
double hft_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_position_profit(stdCode);
}

/**
 * @brief 获取HFT策略持仓均价
 * @details 在高频策略中获取指定合约当前持仓的平均价格
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 当前持仓平均价格
 */
double hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_position_avgpx(stdCode);
}


/**
 * @brief 获取HFT策略未完成委托数量
 * @details 在高频策略中获取指定合约的未完成委托数量
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @return double 未完成委托数量
 */
double hft_get_undone(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return 0;

	return mocker->stra_get_undone(stdCode);
}

/**
 * @brief 获取HFT策略合约当前价格
 * @details 在高频策略中获取指定合约的当前实时最新价格
 * @param stdCode 标准合约代码
 * @return double 合约当前价格
 */
double hft_get_price(const char* stdCode)
{
	return getRunner().replayer().get_cur_price(stdCode);
}

/**
 * @brief 获取HFT策略当前日期
 * @details 在高频策略中获取当前回测时点的交易日期
 * @return WtUInt32 当前日期，格式为YYYYMMDD
 */
WtUInt32 hft_get_date()
{
	return getRunner().replayer().get_date();
}

/**
 * @brief 获取HFT策略当前时间
 * @details 在高频策略中获取当前回测时点的时间
 * @return WtUInt32 当前时间，格式为HHMMSS或HHMMSS000
 */
WtUInt32 hft_get_time()
{
	return getRunner().replayer().get_raw_time();
}

/**
 * @brief 获取HFT策略当前秒数
 * @details 在高频策略中获取当前回测时点的秒数，距离今日开盘的秒数
 * @return WtUInt32 当前秒数
 */
WtUInt32 hft_get_secs()
{
	return getRunner().replayer().get_secs();
}

/**
 * @brief 获取HFT策略K线数据
 * @details 在高频策略中获取指定合约和周期的历史K线数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param period K线周期，如m1/m5/d1等
 * @param barCnt 请求的K线条数
 * @param cb K线数据回调函数，用于接收K线数据
 * @return WtUInt32 实际返回的K线条数
 */
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

/**
 * @brief 获取HFT策略Tick数据
 * @details 在高频策略中获取指定合约的历史Tick数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param tickCnt 请求的Tick数据条数
 * @param cb Tick数据回调函数，用于接收Tick数据
 * @return WtUInt32 实际返回的Tick数据条数
 */
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

/**
 * @brief 获取HFT策略委托队列数据
 * @details 在高频策略中获取指定合约的委托队列数据，用于分析市场深度
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param itemCnt 请求的委托队列数据条数
 * @param cb 委托队列数据回调函数，用于接收委托队列数据
 * @return WtUInt32 实际返回的委托队列数据条数
 */
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

/**
 * @brief 获取HFT策略委托明细数据
 * @details 在高频策略中获取指定合约的委托明细数据，用于分析市场成交细节
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param itemCnt 请求的委托明细数据条数
 * @param cb 委托明细数据回调函数，用于接收委托明细数据
 * @return WtUInt32 实际返回的委托明细数据条数
 */
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

/**
 * @brief 获取HFT策略逐笔成交数据
 * @details 在高频策略中获取指定合约的逐笔成交数据，用于分析市场成交情况
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param itemCnt 请求的逐笔成交数据条数
 * @param cb 逐笔成交数据回调函数，用于接收逐笔成交数据
 * @return WtUInt32 实际返回的逐笔成交数据条数
 */
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

/**
 * @brief HFT策略日志输出
 * @details 在高频策略中输出指定级别的日志
 * @param cHandle 策略上下文句柄
 * @param level 日志级别，LOG_LEVEL_DEBUG/LOG_LEVEL_INFO/LOG_LEVEL_WARN/LOG_LEVEL_ERROR
 * @param message 日志内容
 */
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

/**
 * @brief 订阅HFT策略Tick数据
 * @details 在高频策略中订阅指定合约的tick实时行情数据
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 */
void hft_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_ticks(stdCode);
}

/**
 * @brief 订阅HFT策略委托明细数据
 * @details 在高频策略中订阅指定合约的委托明细数据，用于分析市场成交细节
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 */
void hft_sub_order_detail(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_order_details(stdCode);
}

/**
 * @brief 订阅HFT策略委托队列数据
 * @details 在高频策略中订阅指定合约的委托队列数据，用于分析市场深度
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 */
void hft_sub_order_queue(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_order_queues(stdCode);
}

/**
 * @brief 订阅HFT策略逐笔成交数据
 * @details 在高频策略中订阅指定合约的逐笔成交数据，用于分析市场成交情况
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 */
void hft_sub_transaction(CtxHandler cHandle, const char* stdCode)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_sub_transactions(stdCode);
}

/**
 * @brief 取消HFT策略特定委托
 * @details 在高频策略中根据委托本地ID取消特定委托
 * @param cHandle 策略上下文句柄
 * @param localid 要取消的委托的本地ID
 * @return bool 取消操作是否成功
 */
bool hft_cancel(CtxHandler cHandle, WtUInt32 localid)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return false;

	return mocker->stra_cancel(localid);
}

/**
 * @brief 取消HFT策略指定合约的所有委托
 * @details 在高频策略中取消指定合约的全部委托，可指定购买/卖出方向
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码，为空则取消所有合约的委托
 * @param isBuy 是否为买入委托，true为买入，false为卖出
 * @return WtString 返回被取消的委托ID组成的字符串，以逗号分隔
 */
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

/**
 * @brief HFT策略买入操作
 * @details 在高频策略中发出买入委托
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param price 买入价格
 * @param qty 买入数量
 * @param userTag 用户自定义标签
 * @param flag 下单标志
 * @return WtString 返回多个委托ID组成的字符串，以逗号分隔
 */
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

/**
 * @brief HFT策略卖出操作
 * @details 在高频策略中发出卖出委托
 * @param cHandle 策略上下文句柄
 * @param stdCode 标准合约代码
 * @param price 卖出价格
 * @param qty 卖出数量
 * @param userTag 用户自定义标签
 * @param flag 下单标志
 * @return WtString 返回多个委托ID组成的字符串，以逗号分隔
 */
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

/**
 * @brief 保存HFT策略用户数据
 * @details 在高频策略中保存用户自定义数据，可用于存储策略运行状态
 * @param cHandle 策略上下文句柄
 * @param key 数据键名
 * @param val 数据值
 */
void hft_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	HftMocker* mocker = getRunner().hft_mocker();
	if (mocker == NULL)
		return;

	mocker->stra_save_user_data(key, val);
}

/**
 * @brief 加载HFT策略用户数据
 * @details 在高频策略中读取用户自定义数据，用于恢复策略运行状态
 * @param cHandle 策略上下文句柄
 * @param key 数据键名
 * @param defVal 默认值，当键不存在时返回此值
 * @return WtString 返回数据值，如果键不存在则返回默认值
 */
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