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

/**
 * @brief 输入原始tick数据
 * @param ticks tick数据结构指针
 * @param count tick数量
 * @details 将原始tick数据输入到交易引擎中
 *          tick数据通常是由外部数据源或自定义数据加载器提供的
 *          引擎会自动处理这些数据并分发给相应的策略
 *          实现了WtPorter.h中声明的feed_raw_ticks函数
 */
void feed_raw_ticks(WTSTickStruct* ticks, WtUInt32 count)
{
	WTSLogger::error("API not implemented");
}

/**
 * @brief 初始化交易引擎
 * @param logProfile 日志配置文件路径
 * @param isFile 是否将日志写入文件
 * @param genDir 生成文件的目录
 * @details 初始化交易引擎，设置日志配置并加载配置文件
 *          如果logProfile为空，则使用默认配置
 *          isFile为true时，日志将写入文件，否则写入控制台
 *          genDir指定生成文件的目录，如配置文件、日志等
 *          实现了WtPorter.h中声明的init_porter函数
 */
void init_porter(const char* logProfile, bool isFile, const char* genDir)
{
	static bool inited = false;

	if (inited)
		return;

	getRunner().init(logProfile, isFile, genDir);

	inited = true;
}

/**
 * @brief 配置交易引擎
 * @param cfgfile 配置文件路径
 * @param isFile 是否将配置文件写入文件
 * @details 加载配置文件，配置文件可以是JSON格式的配置文件
 *          如果cfgfile为空，则使用默认配置
 *          isFile为true时，配置文件将写入文件，否则写入控制台
 *          实现了WtPorter.h中声明的config_porter函数
 */
void config_porter(const char* cfgfile, bool isFile)
{
	if (strlen(cfgfile) == 0)
		getRunner().config("config.json", true);
	else
		getRunner().config(cfgfile, isFile);
}

/**
 * @brief 运行交易引擎
 * @param bAsync 是否异步运行
 * @details 启动交易引擎，开始处理数据和策略
 *          bAsync为true时，引擎将以异步方式运行
 *          bAsync为false时，引擎将以同步方式运行
 *          实现了WtPorter.h中声明的run_porter函数
 */
void run_porter(bool bAsync)
{
	getRunner().run(bAsync);
}

/**
 * @brief 释放交易引擎
 * @details 释放交易引擎，清理资源
 *          实现了WtPorter.h中声明的release_porter函数
 */
void release_porter()
{
	getRunner().release();
}

/**
 * @brief 获取WonderTrader版本信息
 * @return 返回WonderTrader的版本字符串
 * @details 返回WonderTrader的版本信息，包括平台名称、版本号、构建日期和时间
 *          实现了WtPorter.h中声明的get_version函数
 */
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

/**
 * @brief 获取原始合约代码
 * @param stdCode 标准合约代码
 * @return 返回原始合约代码
 * @details 获取原始合约代码，用于处理非标准合约的行情数据
 *          实现了WtPorter.h中声明的get_raw_stdcode函数
 */
const char* get_raw_stdcode(const char* stdCode)
{
	return getRunner().get_raw_stdcode(stdCode);
}

/**
 * @brief 写入日志
 * @param level 日志级别
 * @param message 日志消息
 * @param catName 日志类别名称
 * @details 将日志写入到日志文件中
 *          实现了WtPorter.h中声明的write_log函数
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
 * @brief 注册CTA策略工厂
 * @param factFolder 策略工厂文件夹路径
 * @return 返回注册成功或失败
 * @details 注册CTA策略工厂，用于加载CTA策略的工厂
 *          实现了WtPorter.h中声明的reg_cta_factories函数
 */
bool reg_cta_factories(const char* factFolder)
{
	return getRunner().addCtaFactories(factFolder);
}

/**
 * @brief 注册选股策略工厂
 * @param factFolder 策略工厂文件夹路径
 * @return 返回注册成功或失败
 * @details 注册选股策略工厂，用于加载选股策略的工厂
 *          实现了WtPorter.h中声明的reg_sel_factories函数
 */
bool reg_sel_factories(const char* factFolder)
{
	return getRunner().addSelFactories(factFolder);
}

/**
 * @brief 注册高频策略工厂
 * @param factFolder 策略工厂文件夹路径
 * @return 返回注册成功或失败
 * @details 注册高频策略工厂，用于加载高频策略的工厂
 *          实现了WtPorter.h中声明的reg_hft_factories函数
 */
bool reg_hft_factories(const char* factFolder)
{
	return getRunner().addHftFactories(factFolder);
}

/**
 * @brief 注册交易执行器工厂
 * @param factFolder 策略工厂文件夹路径
 * @return 返回注册成功或失败
 * @details 注册交易执行器工厂，用于加载交易执行器的工厂
 *          实现了WtPorter.h中声明的reg_exe_factories函数
 */
bool reg_exe_factories(const char* factFolder)
{
	return getRunner().addExeFactories(factFolder);
}


#pragma region "CTA策略接口"

/**
 * @brief 创建CTA策略上下文
 * @param name 策略名称
 * @param slippage 滑点
 * @return 返回CTA策略上下文句柄
 * @details 创建CTA策略上下文，用于管理CTA策略的执行
 *          实现了WtPorter.h中声明的create_cta_context函数
 */
CtxHandler create_cta_context(const char* name, int slippage)
{
	return getRunner().createCtaContext(name, slippage);
}

/**
 * @brief CTA策略买入
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param qty 买入数量
 * @param userTag 用户标签
 * @param limitprice 限价
 * @param stopprice 止损价格
 * @details CTA策略买入操作，用于执行买入操作
 *          实现了WtPorter.h中声明的cta_enter_long函数
 */
void cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_enter_long(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief CTA策略卖出
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param qty 卖出数量
 * @param userTag 用户标签
 * @param limitprice 限价
 * @param stopprice 止损价格
 * @details CTA策略卖出操作，用于执行卖出操作
 *          实现了WtPorter.h中声明的cta_exit_long函数
 */
void cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_exit_long(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief CTA策略空头
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param qty 空头数量
 * @param userTag 用户标签
 * @param limitprice 限价
 * @param stopprice 止损价格
 * @details CTA策略空头操作，用于执行空头操作
 *          实现了WtPorter.h中声明的cta_enter_short函数
 */
void cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_enter_short(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief CTA策略空头
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param qty 空头数量
 * @param userTag 用户标签
 * @param limitprice 限价
 * @param stopprice 止损价格
 * @details CTA策略空头操作，用于执行空头操作
 *          实现了WtPorter.h中声明的cta_exit_short函数
 */
void cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_exit_short(stdCode, qty, userTag, limitprice, stopprice);
}

/**
 * @brief 获取CTA策略K线数据
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param period K线周期
 * @param barCnt K线数量
 * @param isMain 是否为主K线
 * @param cb K线数据回调函数
 * @return 返回实际获取的K线数量
 * @details 获取CTA策略的K线数据，用于获取历史或实时K线数据
 *          实现了WtPorter.h中声明的cta_get_bars函数
 */
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

/**
 * @brief 获取CTA策略Tick数据
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param tickCnt Tick数量
 * @param cb Tick数据回调函数
 * @return 返回实际获取的Tick数量
 * @details 获取CTA策略的Tick数据，用于获取历史或实时Tick数据
 *          实现了WtPorter.h中声明的cta_get_ticks函数
 */
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

/**
 * @brief 获取CTA策略持仓收益
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回持仓收益
 * @details 获取CTA策略的持仓收益，用于获取持仓收益
 *          实现了WtPorter.h中声明的cta_get_position_profit函数
 */
double cta_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

/**
 * @brief 获取CTA策略持仓收益
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回持仓收益
 * @details 获取CTA策略的持仓收益，用于获取持仓收益
 *          实现了WtPorter.h中声明的cta_get_position_profit函数
 */
WtUInt64 cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}


/**
 * @brief 获取CTA策略具体成本
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param openTag 开仓标签
 * @return 返回特定标签的成本
 * @details 获取CTA策略特定标签的开仓成本，用于精确计算收益
 *          每个标签代表一笔特定的交易，通过标签可以区分不同的交易
 *          实现了WtPorter.h中声明的cta_get_detail_cost函数
 */
double cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}


/**
 * @brief 获取CTA策略具体收益
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param openTag 开仓标签
 * @param flag 收益标志，0-浮动盈亏，1-平仓盈亏
 * @return 返回特定标签的收益
 * @details 获取CTA策略特定标签的开仓收益，用于精确计算收益
 *          每个标签代表一笔特定的交易，通过标签可以区分不同的交易
 *          flag为0时计算浮动盈亏，为1时计算平仓盈亏
 *          实现了WtPorter.h中声明的cta_get_detail_profit函数
 */
double cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

/**
 * @brief 获取CTA策略持仓平均价
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回持仓平均价
 * @details 获取CTA策略的持仓平均价，用于计算成本和盈亏
 *          平均价是根据多次开仓和补仓加权平均后的结果
 *          实现了WtPorter.h中声明的cta_get_position_avgpx函数
 */
double cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

/**
 * @brief 获取CTA策略所有持仓
 * @param cHandle CTA策略上下文句柄
 * @param cb 持仓回调函数
 * @details 获取CTA策略的所有持仓信息，并通过回调函数返回
 *          回调函数将对每个持仓合约调用一次
 *          最后会使用空字符串和数量为0，结束标志为true表示枚举结束
 *          实现了WtPorter.h中声明的cta_get_all_position函数
 */
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

/**
 * @brief 获取CTA策略持仓数量
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param bOnlyValid 是否只返回有效持仓
 * @param openTag 开仓标签，如果指定则只返回指定标签的持仓
 * @return 返回持仓数量
 * @details 获取CTA策略的指定合约的持仓数量
 *          当bOnlyValid为true时，只返回有效持仓（已完成开仓的持仓）
 *          当openTag不为空时，只返回指定标签的持仓
 *          实现了WtPorter.h中声明的cta_get_position函数
 */
double cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

/**
 * @brief 获取CTA策略资金数据
 * @param cHandle CTA策略上下文句柄
 * @param flag 资金数据标志，0-动态权益，1-静态权益，2-均价资产
 * @return 返回指定类型的资金数据
 * @details 获取CTA策略的资金数据，包括动态权益、静态权益和均价资产
 *          动态权益包含浮动盈亏，静态权益不包含浮动盈亏
 *          均价资产是按照当前价格计算的总资产
 *          实现了WtPorter.h中声明的cta_get_fund_data函数
 */
double cta_get_fund_data(CtxHandler cHandle, int flag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}


/**
 * @brief 设置CTA策略目标仓位
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param qty 目标仓位数量
 * @param userTag 用户标签
 * @param limitprice 限价
 * @param stopprice 止损价格
 * @details 设置CTA策略的目标仓位，系统会自动根据当前仓位计算需要交易的数量
 *          当目标仓位大于当前仓位时，系统会自动发起买入操作
 *          当目标仓位小于当前仓位时，系统会自动发起卖出操作
 *          实现了WtPorter.h中声明的cta_set_position函数
 */
void cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_set_position(stdCode, qty, userTag, limitprice, stopprice);
}


/**
 * @brief 获取CTA策略第一次开仓时间
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回第一次开仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 获取CTA策略指定合约的第一次开仓时间
 *          这个时间是最早进入该合约的时间，用于计算持仓时间
 *          返回格式为YYYYMMDDHHNNSS，如果没有持仓则返回0
 *          实现了WtPorter.h中声明的cta_get_first_entertime函数
 */
WtUInt64 cta_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

/**
 * @brief 获取CTA策略最后一次开仓时间
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次开仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 获取CTA策略指定合约的最后一次开仓时间
 *          这个时间是最近进入该合约的时间，用于计算持仓时间
 *          返回格式为YYYYMMDDHHNNSS，如果没有持仓则返回0
 *          实现了WtPorter.h中声明的cta_get_last_entertime函数
 */
WtUInt64 cta_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

/**
 * @brief 获取CTA策略最后一次平仓时间
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次平仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 获取CTA策略指定合约的最后一次平仓时间
 *          这个时间是最近退出该合约的时间，用于计算交易间隔
 *          返回格式为YYYYMMDDHHNNSS，如果没有平仓记录则返回0
 *          实现了WtPorter.h中声明的cta_get_last_exittime函数
 */
WtUInt64 cta_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

/**
 * @brief 获取CTA策略最后一次开仓价格
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次开仓价格
 * @details 获取CTA策略指定合约的最后一次开仓价格
 *          这个价格是最近进入该合约时的开仓价格，用于计算持仓成本
 *          如果没有持仓记录则返回0
 *          实现了WtPorter.h中声明的cta_get_last_enterprice函数
 */
double cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

/**
 * @brief 获取CTA策略最后一次开仓标签
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次开仓标签
 * @details 获取CTA策略指定合约的最后一次开仓标签
 *          这个标签是最近进入该合约时的开仓标签，用于区分不同的开仓记录
 *          如果没有持仓记录则返回空字符串
 *          实现了WtPorter.h中声明的cta_get_last_entertag函数
 */
WtString cta_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}

/**
 * @brief 获取CTA策略当前价格
 * @param stdCode 合约代码
 * @return 返回当前价格
 * @details 获取CTA策略指定合约的当前价格
 *          这个价格是最近进入该合约时的开仓价格，用于计算持仓成本
 *          如果没有持仓记录则返回0
 *          实现了WtPorter.h中声明的cta_get_price函数
 */
double cta_get_price(const char* stdCode)
{
	return getRunner().getEngine()->get_cur_price(stdCode);
}

/**
 * @brief 获取CTA策略指定合约的指定类型日线价格
 * @param stdCode 合约代码
 * @param flag 日线价格类型，0-收盘价，1-最高价，2-最低价，3-开盘价
 * @return 返回指定类型日线价格
 * @details 获取CTA策略指定合约的指定类型日线价格
 *          这个价格是最近进入该合约时的开仓价格，用于计算持仓成本
 *          如果没有持仓记录则返回0
 *          实现了WtPorter.h中声明的cta_get_day_price函数
 */
double cta_get_day_price(const char* stdCode, int flag)
{
	return getRunner().getEngine()->get_day_price(stdCode, flag);
}

/**
 * @brief 获取CTA策略当前交易日
 * @return 返回当前交易日
 * @details 获取CTA策略当前交易日，用于获取当前交易日
 *          实现了WtPorter.h中声明的cta_get_tdate函数
 */
WtUInt32 cta_get_tdate()
{
	return getRunner().getEngine()->get_trading_date();
}

/**
 * @brief 获取CTA策略当前日期
 * @return 返回当前日期
 * @details 获取CTA策略当前日期，用于获取当前日期
 *          实现了WtPorter.h中声明的cta_get_date函数
 */
WtUInt32 cta_get_date()
{
	return getRunner().getEngine()->get_date();
}

/**
 * @brief 获取CTA策略当前时间
 * @return 返回当前时间
 * @details 获取CTA策略当前时间，用于获取当前时间
 *          实现了WtPorter.h中声明的cta_get_time函数
 */
WtUInt32 cta_get_time()
{
	return getRunner().getEngine()->get_min_time();
}

/**
 * @brief CTA策略日志输出
 * @param cHandle CTA策略上下文句柄
 * @param level 日志级别
 * @param message 日志消息
 * @details CTA策略日志输出，用于输出日志信息
 *          实现了WtPorter.h中声明的cta_log_text函数
 */
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

/**
 * @brief CTA策略保存用户数据
 * @param cHandle CTA策略上下文句柄
 * @param key 用户数据键
 * @param val 用户数据值
 * @details CTA策略保存用户数据，用于保存用户数据
 *          实现了WtPorter.h中声明的cta_save_userdata函数
 */
void cta_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

/**
 * @brief CTA策略加载用户数据
 * @param cHandle CTA策略上下文句柄
 * @param key 用户数据键
 * @param defVal 默认值
 * @return 返回用户数据值
 * @details CTA策略加载用户数据，用于加载用户数据
 *          实现了WtPorter.h中声明的cta_load_userdata函数
 */
WtString cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

/**
 * @brief CTA策略订阅行情
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @details CTA策略订阅行情，用于订阅行情
 *          实现了WtPorter.h中声明的cta_sub_ticks函数
 */
void cta_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

/**
 * @brief CTA策略订阅K线事件
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param period K线周期
 * @details CTA策略订阅K线事件，用于订阅K线事件
 *          实现了WtPorter.h中声明的cta_sub_bar_events函数
 */
void cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_bar_events(stdCode, period);
}

/**
 * @brief CTA策略设置K线图
 * @param cHandle CTA策略上下文句柄
 * @param stdCode 合约代码
 * @param period K线周期
 * @details CTA策略设置K线图，用于设置K线图
 *          实现了WtPorter.h中声明的cta_set_chart_kline函数
 */
void cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->set_chart_kline(stdCode, period);
}

/**
 * @brief CTA策略添加图表标记
 * @param cHandle CTA策略上下文句柄
 * @param price 标记价格
 * @param icon 标记图标
 * @param tag 标记标签
 * @details CTA策略添加图表标记，用于添加图表标记
 *          实现了WtPorter.h中声明的cta_add_chart_mark函数
 */
void cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->add_chart_mark(price, icon, tag);
}

/**
 * @brief CTA策略注册指标
 * @param cHandle CTA策略上下文句柄
 * @param idxName 指标名称
 * @param indexType 指标类型
 * @details CTA策略注册指标，用于注册指标
 *          实现了WtPorter.h中声明的cta_register_index函数
 */
void cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->register_index(idxName, indexType);
}

/**
 * @brief CTA策略注册指标线
 * @param cHandle CTA策略上下文句柄
 * @param idxName 指标名称
 * @param lineName 指标线名称
 * @param lineType 指标线类型
 * @details CTA策略注册指标线，用于注册指标线
 *          实现了WtPorter.h中声明的cta_register_index_line函数
 */
bool cta_register_index_line(CtxHandler cHandle, const char* idxName, const char* lineName, WtUInt32 lineType)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->register_index_line(idxName, lineName, lineType);
}

/**
 * @brief CTA策略添加指标基线
 * @param cHandle CTA策略上下文句柄
 * @param idxName 指标名称
 * @param lineName 指标线名称
 * @param val 基线值
 * @details CTA策略添加指标基线，用于添加指标基线
 *          实现了WtPorter.h中声明的cta_add_index_baseline函数
 */
bool cta_add_index_baseline(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->add_index_baseline(idxName, lineName, val);
}

/**
 * @brief CTA策略设置指标值
 * @param cHandle CTA策略上下文句柄
 * @param idxName 指标名称
 * @param lineName 指标线名称
 * @param val 指标值
 * @details CTA策略设置指标值，用于设置指标值
 *          实现了WtPorter.h中声明的cta_set_index_value函数
 */
bool cta_set_index_value(CtxHandler cHandle, const char* idxName, const char* lineName, double val)
{
	CtaContextPtr ctx = getRunner().getCtaContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->set_index_value(idxName, lineName, val);
}


#pragma endregion

#pragma region "多因子策略接口"

/**
 * @brief 创建多因子策略上下文
 * @param name 策略名称
 * @param date 策略日期
 * @param time 策略时间
 * @param period 策略周期
 * @param trdtpl 交易模板
 * @param session 交易时段
 * @param slippage 滑点
 * @return 返回多因子策略上下文句柄
 * @details 创建多因子策略上下文，用于创建多因子策略上下文
 *          实现了WtPorter.h中声明的create_sel_context函数
 */
CtxHandler create_sel_context(const char* name, uint32_t date, uint32_t time, const char* period, const char* trdtpl/* = "CHINA"*/, const char* session/* = "TRADING"*/, int32_t slippage/* = 0*/)
{
	return getRunner().createSelContext(name, date, time, period, slippage, trdtpl, session);
}

/**
 * @brief 多因子策略保存用户数据
 * @param cHandle 多因子策略上下文句柄
 * @param key 用户数据键
 * @param val 用户数据值
 * @details 多因子策略保存用户数据，用于保存用户数据
 *          实现了WtPorter.h中声明的sel_save_userdata函数
 */
void sel_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

/**
 * @brief 多因子策略加载用户数据
 * @param cHandle 多因子策略上下文句柄
 * @param key 用户数据键
 * @param defVal 默认值
 * @return 返回用户数据值
 * @details 多因子策略加载用户数据，用于加载用户数据
 *          实现了WtPorter.h中声明的sel_load_userdata函数
 */
WtString sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}

/**
 * @brief 多因子策略日志输出
 * @param cHandle 多因子策略上下文句柄
 * @param level 日志级别
 * @param message 日志消息
 * @details 多因子策略日志输出，用于输出日志信息
 *          实现了WtPorter.h中声明的sel_log_text函数
 */
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

/**
 * @brief 多因子策略获取当前价格
 * @param stdCode 合约代码
 * @return 返回当前价格
 * @details 多因子策略获取当前价格，用于获取当前价格
 *          实现了WtPorter.h中声明的sel_get_price函数
 */
double sel_get_price(const char* stdCode)
{
	return getRunner().getEngine()->get_cur_price(stdCode);
}

/**
 * @brief 多因子策略获取当前日期
 * @return 返回当前日期
 * @details 多因子策略获取当前日期，用于获取当前日期
 *          实现了WtPorter.h中声明的sel_get_date函数
 */
WtUInt32 sel_get_date()
{
	return getRunner().getEngine()->get_date();
}

/**
 * @brief 多因子策略获取当前时间
 * @return 返回当前时间
 * @details 多因子策略获取当前时间，用于获取当前时间
 *          实现了WtPorter.h中声明的sel_get_time函数
 */
WtUInt32 sel_get_time()
{
	return getRunner().getEngine()->get_min_time();
}

/**
 * @brief 多因子策略获取所有持仓
 * @param cHandle 多因子策略上下文句柄
 * @param cb 获取持仓回调函数
 * @details 多因子策略获取所有持仓，用于获取所有持仓
 *          实现了WtPorter.h中声明的sel_get_all_position函数
 */
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

/**
 * @brief 多因子策略获取持仓
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param bOnlyValid 是否只获取有效持仓
 * @param openTag 开仓标签
 * @return 返回持仓数量
 * @details 多因子策略获取持仓，用于获取持仓数量
 *          实现了WtPorter.h中声明的sel_get_position函数
 */
double sel_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid, openTag);
}

/**
 * @brief 多因子策略获取K线数据
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param period K线周期
 * @param barCnt K线数量
 * @param cb 获取K线数据回调函数
 * @return 返回K线数据
 * @details 多因子策略获取K线数据，用于获取K线数据
 *          实现了WtPorter.h中声明的sel_get_bars函数
 */
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

/**
 * @brief 多因子策略设置持仓
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param qty 持仓数量
 * @param userTag 用户标签
 * @details 多因子策略设置持仓，用于设置持仓数量
 *          实现了WtPorter.h中声明的sel_set_position函数
 */
void sel_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	//多因子引擎,限价和止价都无效
	ctx->stra_set_position(stdCode, qty, userTag);
}


/**
 * @brief 多因子策略获取Tick数据
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param tickCnt 请求的tick数量
 * @param cb 获取Tick数据回调函数
 * @return 返回实际获取的tick数量
 * @details 多因子策略获取指定合约的Tick数据，并通过回调函数返回
 *          实现了WtPorter.h中声明的sel_get_ticks函数
 */
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

/**
 * @brief 多因子策略订阅Tick数据
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @details 多因子策略订阅指定合约的实时Tick数据
 *          订阅后，系统会将该合约的Tick数据推送给策略
 *          实现了WtPorter.h中声明的sel_sub_ticks函数
 */
void sel_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

/**
 * @brief 多因子策略获取日线价格数据
 * @param stdCode 合约代码
 * @param flag 价格标志，0-收盘价，1-最高价，2-最低价，3-开盘价
 * @return 返回指定类型的日线价格
 * @details 多因子策略获取指定合约的日线价格数据
 *          根据flag参数返回不同类型的价格（收盘、最高、最低、开盘）
 *          实现了WtPorter.h中声明的sel_get_day_price函数
 */
double sel_get_day_price(const char* stdCode, int flag)
{
	return getRunner().getEngine()->get_day_price(stdCode, flag);
}

/**
 * @brief 多因子策略获取当前交易日
 * @return 返回当前交易日，格式为YYYYMMDD
 * @details 多因子策略获取当前交易日期
 *          返回格式为YYYYMMDD，如20250502
 *          实现了WtPorter.h中声明的sel_get_tdate函数
 */
WtUInt32 sel_get_tdate()
{
	return getRunner().getEngine()->get_trading_date();
}

/**
 * @brief 多因子策略获取资金数据
 * @param cHandle 多因子策略上下文句柄
 * @param flag 资金数据标志，0-动态权益，1-静态权益，2-均价资产
 * @return 返回指定类型的资金数据
 * @details 多因子策略获取资金数据，包括动态权益、静态权益和均价资产
 *          动态权益包含浮动盈亏，静态权益不包含浮动盈亏
 *          均价资产是按照当前价格计算的总资产
 *          实现了WtPorter.h中声明的sel_get_fund_data函数
 */
double sel_get_fund_data(CtxHandler cHandle, int flag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_fund_data(flag);
}

/**
 * @brief 多因子策略获取持仓收益
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回指定合约的持仓收益
 * @details 多因子策略获取指定合约的持仓收益
 *          收益包括已实现和未实现的收益
 *          实现了WtPorter.h中声明的sel_get_position_profit函数
 */
double sel_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

/**
 * @brief 多因子策略获取开仓时间
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param openTag 开仓标签
 * @return 返回开仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 多因子策略获取指定合约和标签的开仓时间
 *          这个时间是指定标签的交易开始时间
 *          如果没有找到指定标签的持仓，则返回0
 *          实现了WtPorter.h中声明的sel_get_detail_entertime函数
 */
WtUInt64 sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_entertime(stdCode, openTag);
}

/**
 * @brief 多因子策略获取开仓成本
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param openTag 开仓标签
 * @return 返回开仓成本
 * @details 多因子策略获取指定合约和标签的开仓成本
 *          开仓成本用于计算盈亏
 *          如果没有找到指定标签的持仓，则返回0
 *          实现了WtPorter.h中声明的sel_get_detail_cost函数
 */
double sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_cost(stdCode, openTag);
}

/**
 * @brief 多因子策略获取开仓收益
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @param openTag 开仓标签
 * @param flag 收益标志，0-浮动盈亏，1-平仓盈亏
 * @return 返回开仓收益
 * @details 多因子策略获取指定合约和标签的开仓收益
 *          当flag为0时返回浮动盈亏，即未实现盈亏；当flag为1时返回平仓盈亏，即已实现盈亏
 *          如果没有找到指定标签的持仓，则返回0
 *          实现了WtPorter.h中声明的sel_get_detail_profit函数
 */
double sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_detail_profit(stdCode, openTag, flag);
}

/**
 * @brief 多因子策略获取持仓平均价
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回指定合约的持仓平均价
 * @details 多因子策略获取指定合约的持仓平均价
 *          持仓平均价是多次开仓后的加权平均价格，用于计算成本和盈亏
 *          如果没有持仓，则返回0
 *          实现了WtPorter.h中声明的sel_get_position_avgpx函数
 */
double sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

/**
 * @brief 多因子策略获取第一次开仓时间
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回第一次开仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 多因子策略获取指定合约的第一次开仓时间
 *          这个时间是最早进入该合约的时间，用于计算持仓时间
 *          返回格式为YYYYMMDDHHNNSS，如果没有持仓则返回0
 *          实现了WtPorter.h中声明的sel_get_first_entertime函数
 */
WtUInt64 sel_get_first_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_first_entertime(stdCode);
}

/**
 * @brief 多因子策略获取最后一次开仓时间
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次开仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 多因子策略获取指定合约的最后一次开仓时间
 *          这个时间是最近进入该合约的时间，用于计算持仓时间
 *          返回格式为YYYYMMDDHHNNSS，如果没有持仓则返回0
 *          实现了WtPorter.h中声明的sel_get_last_entertime函数
 */
WtUInt64 sel_get_last_entertime(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertime(stdCode);
}

/**
 * @brief 多因子策略获取最后一次平仓时间
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次平仓时间，以YYYYMMDDHHNNSS格式返回
 * @details 多因子策略获取指定合约的最后一次平仓时间
 *          这个时间是最近退出该合约的时间，用于计算交易间隔
 *          返回格式为YYYYMMDDHHNNSS，如果没有平仓记录则返回0
 *          实现了WtPorter.h中声明的sel_get_last_exittime函数
 */
WtUInt64 sel_get_last_exittime(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_exittime(stdCode);
}

/**
 * @brief 多因子策略获取最后一次开仓价格
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次开仓价格
 * @details 多因子策略获取指定合约的最后一次开仓价格
 *          这个价格是最近进入该合约的交易价格，用于计算成本和盈亏
 *          如果没有开仓记录，则返回0
 *          实现了WtPorter.h中声明的sel_get_last_enterprice函数
 */
double sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_enterprice(stdCode);
}

/**
 * @brief 多因子策略获取最后一次开仓标签
 * @param cHandle 多因子策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回最后一次开仓标签
 * @details 多因子策略获取指定合约的最后一次开仓标签
 *          这个标签是用于标识特定交易的唯一标识，用于跟踪和管理交易
 *          如果没有开仓记录，则返回空字符串
 *          实现了WtPorter.h中声明的sel_get_last_entertag函数
 */
WtString sel_get_last_entertag(CtxHandler cHandle, const char* stdCode)
{
	SelContextPtr ctx = getRunner().getSelContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_last_entertag(stdCode);
}
#pragma endregion

#pragma region "HFT策略接口"
/**
 * @brief 创建高频策略上下文
 * @param name 策略名称
 * @param trader 交易器名称
 * @param agent 是否代理模式
 * @param slippage 滑点，默认为0
 * @return 返回高频策略上下文句柄
 * @details 创建高频策略上下文，用于管理高频策略的执行
 *          trader参数指定使用的交易器，agent参数指定是否是代理模式
 *          滑点参数用于设置交易滑点，影响实际成交价格
 *          实现了WtPorter.h中声明的create_hft_context函数
 */
CtxHandler create_hft_context(const char* name, const char* trader, bool agent, int32_t slippage/* = 0*/)
{
	return getRunner().createHftContext(name, trader, agent, slippage);
}

/**
 * @brief 获取高频策略持仓数量
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param bOnlyValid 是否只返回有效持仓
 * @return 返回指定合约的持仓数量
 * @details 获取高频策略指定合约的持仓数量
 *          当bOnlyValid为true时，只返回有效持仓（已完成开仓的持仓）
 *          当bOnlyValid为false时，返回所有持仓，包括未完成开仓的持仓
 *          实现了WtPorter.h中声明的hft_get_position函数
 */
double hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position(stdCode, bOnlyValid);
}

/**
 * @brief 获取高频策略持仓收益
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回指定合约的持仓收益
 * @details 获取高频策略指定合约的持仓收益
 *          这个收益包括浮动盈亏，即未实现的收益
 *          如果没有持仓，则返回0
 *          实现了WtPorter.h中声明的hft_get_position_profit函数
 */
double hft_get_position_profit(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_profit(stdCode);
}

/**
 * @brief 获取高频策略持仓平均价
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回指定合约的持仓平均价
 * @details 获取高频策略指定合约的持仓平均价
 *          平均价是多次开仓后的加权平均价格，用于计算成本和盈亏
 *          如果没有持仓，则返回0
 *          实现了WtPorter.h中声明的hft_get_position_avgpx函数
 */
double hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_position_avgpx(stdCode);
}

/**
 * @brief 获取高频策略未完成数量
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @return 返回指定合约的未完成委托数量
 * @details 获取高频策略指定合约的未完成委托数量
 *          未完成委托是指已提交但尚未完全成交的委托
 *          如果没有未完成委托，则返回0
 *          实现了WtPorter.h中声明的hft_get_undone函数
 */
double hft_get_undone(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return 0;

	return ctx->stra_get_undone(stdCode);
}

/**
 * @brief 获取高频策略合约当前价格
 * @param stdCode 合约代码
 * @return 返回指定合约的当前价格
 * @details 获取高频策略指定合约的当前市场价格
 *          这个价格通常是最新成交价或最新行情价格
 *          如果没有行情数据，则返回0
 *          实现了WtPorter.h中声明的hft_get_price函数
 */
double hft_get_price(const char* stdCode)
{
	return getRunner().getEngine()->get_cur_price(stdCode);
}

/**
 * @brief 获取高频策略当前交易日期
 * @return 返回当前日期，格式为YYYYMMDD
 * @details 获取高频策略当前交易日期
 *          返回格式为YYYYMMDD，如果当前是2025年5月2日，则返回20250502
 *          实现了WtPorter.h中声明的hft_get_date函数
 */
WtUInt32 hft_get_date()
{
	return getRunner().getEngine()->get_date();
}

/**
 * @brief 获取高频策略当前交易时间
 * @return 返回当前时间，格式为HHMMSS
 * @details 获取高频策略当前交易时间
 *          返回格式为HHMMSS，如果当前是10点35分30秒，则返回103530
 *          实现了WtPorter.h中声明的hft_get_time函数
 */
WtUInt32 hft_get_time()
{
	return getRunner().getEngine()->get_raw_time();
}

/**
 * @brief 获取高频策略当前秒数
 * @return 返回当前秒数，代表当天经过的秒数
 * @details 获取高频策略当前交易日内的秒数
 *          返回值是从当天零点开始的秒数，最大为86400（24*60*60）
 *          实现了WtPorter.h中声明的hft_get_secs函数
 */
WtUInt32 hft_get_secs()
{
	return getRunner().getEngine()->get_secs();
}

/**
 * @brief 获取高频策略K线数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param period K线周期，如m1、m5、d1等
 * @param barCnt 请求的K线数量
 * @param cb K线数据回调函数
 * @return 返回实际获取的K线数量
 * @details 获取高频策略指定合约和周期的K线数据
 *          数据通过回调函数cb返回，可能会分多次调用回调函数
 *          如果没有数据或发生错误，则返回0
 *          实现了WtPorter.h中声明的hft_get_bars函数
 */
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

/**
 * @brief 获取高频策略Tick数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param tickCnt 请求的tick数量
 * @param cb Tick数据回调函数
 * @return 返回实际获取的tick数量
 * @details 获取高频策略指定合约的Tick数据
 *          数据通过回调函数cb返回，包含最新的市场行情数据
 *          如果没有数据或发生错误，则返回0
 *          实现了WtPorter.h中声明的hft_get_ticks函数
 */
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

/**
 * @brief 获取高频策略委托队列数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param itemCnt 请求的委托队列数量
 * @param cb 委托队列数据回调函数
 * @return 返回实际获取的委托队列数量
 * @details 获取高频策略指定合约的委托队列数据
 *          委托队列数据包含市场上的委托情况，运行于非连续交易的市场
 *          数据通过回调函数cb返回
 *          如果没有数据或发生错误，则返回0
 *          实现了WtPorter.h中声明的hft_get_ordque函数
 */
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

/**
 * @brief 获取高频策略委托明细数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param itemCnt 请求的委托明细数量
 * @param cb 委托明细数据回调函数
 * @return 返回实际获取的委托明细数量
 * @details 获取高频策略指定合约的委托明细数据
 *          委托明细数据包含市场上的委托详情，比委托队列数据更详细
 *          数据通过回调函数cb返回
 *          如果没有数据或发生错误，则返回0
 *          实现了WtPorter.h中声明的hft_get_orddtl函数
 */
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

/**
 * @brief 获取高频策略逆回成交数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param itemCnt 请求的成交数量
 * @param cb 成交数据回调函数
 * @return 返回实际获取的成交数量
 * @details 获取高频策略指定合约的逆回成交数据
 *          逆回成交数据包含市场上的成交详情，是高频交易的重要信息
 *          数据通过回调函数cb返回
 *          如果没有数据或发生错误，则返回0
 *          实现了WtPorter.h中声明的hft_get_trans函数
 */
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

/**
 * @brief 高频策略日志输出函数
 * @param cHandle 高频策略上下文句柄
 * @param level 日志级别，可以是INFO、WARNING、ERROR等
 * @param message 日志消息内容
 * @details 将高频策略中需要记录的信息输出到系统日志中
 *          根据指定的日志级别使用不同的输出方式
 *          实现了WtPorter.h中声明的hft_log_text函数
 */
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

/**
 * @brief 高频策略订阅Tick数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @details 订阅指定合约的Tick数据，高频策略需要Tick数据来分析短期市场流动性
 *          订阅后，系统会实时收集并处理该合约的Tick数据
 *          实现了WtPorter.h中声明的hft_sub_ticks函数
 */
void hft_sub_ticks(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_ticks(stdCode);
}

/**
 * @brief 高频策略订阅委托明细数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @details 订阅指定合约的委托明细数据，高频策略需要这些数据来分析市场深度和流动性
 *          委托明细数据包含市场上委托的详细情况，订阅后系统会实时收集这些数据
 *          实现了WtPorter.h中声明的hft_sub_order_detail函数
 */
void hft_sub_order_detail(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_order_details(stdCode);
}

/**
 * @brief 高频策略订阅委托队列数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @details 订阅指定合约的委托队列数据，高频策略可以通过这些数据分析市场深度
 *          委托队列数据包含市场上委托排队的情况，相比委托明细数据更聚焦于队列结构
 *          订阅后系统会实时收集这些数据
 *          实现了WtPorter.h中声明的hft_sub_order_queue函数
 */
void hft_sub_order_queue(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_order_queues(stdCode);
}

/**
 * @brief 高频策略订阅逆回成交数据
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @details 订阅指定合约的逆回成交数据，高频策略可以通过这些数据分析市场的成交情况
 *          逆回成交数据包含市场上实际发生的成交记录，是高频交易中非常重要的信息
 *          订阅后系统会实时收集这些数据
 *          实现了WtPorter.h中声明的hft_sub_transaction函数
 */
void hft_sub_transaction(CtxHandler cHandle, const char* stdCode)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_sub_transactions(stdCode);
}

/**
 * @brief 高频策略撤单函数
 * @param cHandle 高频策略上下文句柄
 * @param localid 要撤销的委托本地ID
 * @return 撤单是否成功，true表示撤单请求成功发送，false表示撤单请求失败
 * @details 尝试撤销指定本地ID的委托单
 *          高频交易中需要快速响应市场变化，有时需要撤销已发出的委托
 *          注意这里的撤单只表示撤单请求发送成功，不代表委托一定被成功撤销
 *          实际撤单结果需要通过回调获知
 *          实现了WtPorter.h中声明的hft_cancel函数
 */
bool hft_cancel(CtxHandler cHandle, WtUInt32 localid)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return false;

	return ctx->stra_cancel(localid);
}

/**
 * @brief 高频策略批量撤单函数
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码，要撤销的合约
 * @param isBuy 是否为买单，true表示买单，false表示卖单
 * @return 被撤销的委托的本地ID列表，以逗号分隔的字符串
 * @details 批量撤销指定合约的进行中的所有委托
 *          可以根据isBuy参数选择只撤销买单或卖单
 *          函数返回被撤销的委托的本地ID列表，如果没有委托被撤销则返回空字符串
 *          高频交易中有时需要快速撤销所有进行中的委托以响应市场变化
 *          实现了WtPorter.h中声明的hft_cancel_all函数
 */
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

/**
 * @brief 高频策略买入函数
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param price 买入价格
 * @param qty 买入数量
 * @param userTag 用户自定义标签，用于标识特定交易
 * @param flag 下单标记，可以指定下单类型或特殊属性
 * @return 返回下单的本地ID列表，以逗号分隔的字符串形式
 * @details 在高频交易策略中发出买入委托
 *          可以通过flag参数指定不同的下单类型，如限价单、市价单等
 *          可以通过userTag参数为该笔交易添加自定义标记，便于后续识别和管理
 *          返回值是下单的本地ID列表，可用于跟踪和取消该委托
 *          实现了WtPorter.h中声明的hft_buy函数
 */
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

/**
 * @brief 高频策略卖出函数
 * @param cHandle 高频策略上下文句柄
 * @param stdCode 合约代码
 * @param price 卖出价格
 * @param qty 卖出数量
 * @param userTag 用户自定义标签，用于标识特定交易
 * @param flag 下单标记，可以指定下单类型或特殊属性
 * @return 返回下单的本地ID列表，以逗号分隔的字符串形式
 * @details 在高频交易策略中发出卖出委托
 *          可以通过flag参数指定不同的下单类型，如限价单、市价单等
 *          可以通过userTag参数为该笔交易添加自定义标记，便于后续识别和管理
 *          返回值是下单的本地ID列表，可用于跟踪和取消该委托
 *          实现了WtPorter.h中声明的hft_sell函数
 */
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

/**
 * @brief 高频策略保存用户数据
 * @param cHandle 高频策略上下文句柄
 * @param key 数据键名
 * @param val 数据值
 * @details 将高频策略的用户数据按照键值对的形式保存
 *          该函数可以用于保存策略参数、中间计算结果或其他需要在不同运行周期间保存的数据
 *          保存的数据可以通过hft_load_userdata函数加载
 *          实现了WtPorter.h中声明的hft_save_userdata函数
 */
void hft_save_userdata(CtxHandler cHandle, const char* key, const char* val)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return;

	ctx->stra_save_user_data(key, val);
}

/**
 * @brief 高频策略加载用户数据
 * @param cHandle 高频策略上下文句柄
 * @param key 要加载的数据键名
 * @param defVal 如果键不存在时的默认值
 * @return 返回加载的用户数据值，如果键不存在则返回defVal
 * @details 从高频策略存储中加载指定键名的用户数据
 *          该函数是hft_save_userdata的配对函数，用于读取之前保存的数据
 *          可以用于读取策略参数、中间计算结果或其他跨运行周期的数据
 *          实现了WtPorter.h中声明的hft_load_userdata函数
 */
WtString hft_load_userdata(CtxHandler cHandle, const char* key, const char* defVal)
{
	HftContextPtr ctx = getRunner().getHftContext(cHandle);
	if (ctx == NULL)
		return defVal;

	return ctx->stra_load_user_data(key, defVal);
}
#pragma endregion "HFT策略接口"

#pragma region "扩展Parser接口"
/**
 * @brief 外部行情解析器推送行情数据
 * @param id 解析器ID标识
 * @param curTick 当前行情数据结构指针
 * @param uProcFlag 处理标志，控制数据处理方式
 * @details 用于外部行情解析器推送行情数据到交易框架内
 *          外部解析器获取到市场行情数据后，通过该函数将数据推送给WonderTrader处理
 *          该函数会调用getRunner().on_ext_parser_quote来处理外部行情数据
 *          实现了WtPorter.h中声明的parser_push_quote函数
 */
void parser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag)
{
	getRunner().on_ext_parser_quote(id, curTick, uProcFlag);
}
#pragma endregion "扩展Parser接口"