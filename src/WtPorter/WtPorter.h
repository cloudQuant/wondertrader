/*!
 * \file WtPorter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader的对外接口头文件
 * \details 定义了WonderTrader框架的对外C语言接口，包含了与策略交互的各种函数接口
 *          主要包括系统管理接口、CTA策略接口、选股(SEL)策略接口和高频(HFT)策略接口
 *          这些接口可以被其他语言如Python等调用
 */
#pragma once
#include "PorterDefs.h"


#ifdef __cplusplus
extern "C"
{
#endif
	/**
	 * @brief 注册事件回调函数
	 * @param cbEvt 事件回调函数指针
	 * @details 用于注册事件回调函数，当系统中发生重要事件时，会通过该回调函数通知用户
	 *          事件包括系统启动、停止和其他重要状态变化
	 */
	EXPORT_FLAG	void		register_evt_callback(FuncEventCallback cbEvt);

	/**
	 * @brief 注册CTA策略回调函数
	 * @param cbInit 策略初始化回调函数
	 * @param cbTick 行情Tick数据回调函数
	 * @param cbCalc 策略计算回调函数
	 * @param cbBar K线数据回调函数
	 * @param cbSessEvt 交易时段事件回调函数
	 * @param cbCondTrigger 条件触发回调函数，可选参数
	 * @details 用于注册CTA策略的各种回调函数，当框架需要与策略交互时，会调用这些回调函数
	 *          CTA策略是量化交易的主要策略类型，支持多品种投资
	 */
	EXPORT_FLAG	void		register_cta_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCondTriggerCallback cbCondTrigger = NULL);

	/**
	 * @brief 注册选股(SEL)策略回调函数
	 * @param cbInit 策略初始化回调函数
	 * @param cbTick 行情Tick数据回调函数
	 * @param cbCalc 策略计算回调函数
	 * @param cbBar K线数据回调函数
	 * @param cbSessEvt 交易时段事件回调函数
	 * @details 用于注册选股策略的各种回调函数，当框架需要与选股策略交互时，会调用这些回调函数
	 *          选股策略专注于股票池的选取和管理，旨在实现自动化的股票投资策略
	 */
	EXPORT_FLAG	void		register_sel_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt);

	/**
	 * @brief 注册高频(HFT)策略回调函数
	 * @param cbInit 策略初始化回调函数
	 * @param cbTick 行情Tick数据回调函数
	 * @param cbBar K线数据回调函数
	 * @param cbChnl 交易通道状态回调函数
	 * @param cbOrd 委托回报回调函数
	 * @param cbTrd 成交回报回调函数
	 * @param cbEntrust 委托状态回调函数
	 * @param cbOrdDtl 委托明细回调函数
	 * @param cbOrdQue 委托队列回调函数
	 * @param cbTrans 成交明细回调函数
	 * @param cbSessEvt 交易时段事件回调函数
	 * @param cbPosition 持仓变化回调函数
	 * @details 用于注册高频策略的各种回调函数，当框架需要与高频策略交互时，会调用这些回调函数
	 *          高频策略专注于短时间内的快速交易，需要处理详细的委托、成交和市场微观结构信息
	 */
	EXPORT_FLAG	void		register_hft_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar,
							FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
							FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt, FuncHftPosCallback cbPosition);

	/**
	 * @brief 注册行情解析器回调函数
	 * @param cbEvt 行情解析器事件回调函数
	 * @param cbSub 行情订阅回调函数
	 * @details 用于注册行情解析器相关的回调函数，已实现自定义行情数据源的接入
	 *          当需要处理行情数据相关的事件或订阅行为时，会调用这些回调函数
	 */
	EXPORT_FLAG void		register_parser_callbacks(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub);

	/**
	 * @brief 注册执行器回调函数
	 * @param cbInit 执行器初始化回调函数
	 * @param cbExec 执行器命令处理回调函数
	 * @details 用于注册执行器相关的回调函数，实现自定义执行器的接入
	 *          执行器是负责将策略生成的交易信号转化为实际交易指令的组件
	 */
	EXPORT_FLAG void		register_exec_callbacks(FuncExecInitCallback cbInit, FuncExecCmdCallback cbExec);

	/**
	 * @brief 注册外部数据加载器
	 * @param fnlBarLoader 最终K线数据加载器
	 * @param rawBarLoader 原始K线数据加载器
	 * @param fctLoader 复权因子加载器
	 * @param tickLoader Tick数据加载器
	 * @details 用于注册外部数据加载器，实现自定义数据源的接入
	 *          当系统需要加载历史数据时，会调用这些加载器函数
	 *          支持加载K线数据、复权因子和Tick数据
	 */
	EXPORT_FLAG void		register_ext_data_loader(FuncLoadFnlBars fnlBarLoader, FuncLoadRawBars rawBarLoader, FuncLoadAdjFactors fctLoader, FuncLoadRawTicks tickLoader);

	/**
	 * @brief 输入原始K线数据
	 * @param bars K线数据指针，指向K线数据数组
	 * @param count K线数据数量
	 * @details 用于直接向框架输入原始K线数据，主要在使用外部数据源时使用
	 *          这些数据会被框架内部处理并提供给策略使用
	 */
	EXPORT_FLAG void		feed_raw_bars(WTSBarStruct* bars, WtUInt32 count);

	/**
	 * @brief 输入原始Tick数据
	 * @param ticks Tick数据指针，指向Tick数据数组
	 * @param count Tick数据数量
	 * @details 用于直接向框架输入原始Tick数据，主要在使用外部数据源时使用
	 *          这些数据会被框架内部处理并提供给策略使用
	 *          Tick数据是最小的行情单元，包含最近一笔成交的价格和数量等信息
	 */
	EXPORT_FLAG void		feed_raw_ticks(WTSTickStruct* ticks, WtUInt32 count);

	/**
	 * @brief 输入复权因子数据
	 * @param stdCode 标准合约代码
	 * @param dates 日期数组指针
	 * @param factors 复权因子数组指针
	 * @param count 数据数量
	 * @details 用于直接向框架输入复权因子数据，主要用于股票类品种的数据处理
	 *          复权因子用于调整股票价格数据，使其在除权、除息等事件后保持连续性
	 */
	EXPORT_FLAG void		feed_adj_factors(WtString stdCode, WtUInt32* dates, double* factors, WtUInt32 count);

	/**
	 * @brief 初始化Porter模块
	 * @param logCfg 日志配置文件或内容
	 * @param isFile 是否为文件，如果为true，logCfg为文件路径；如果为false，logCfg为配置内容字符串
	 * @param genDir 数据生成目录
	 * @details 初始化Porter模块，设置日志配置和数据生成目录
	 *          该函数应在使用其他Porter接口前调用
	 */
	EXPORT_FLAG	void		init_porter(const char* logCfg, bool isFile, const char* genDir);

	/**
	 * @brief 配置Porter模块
	 * @param cfgfile 配置文件或配置内容
	 * @param isFile 是否为文件，如果为true，cfgfile为文件路径；如果为false，cfgfile为配置内容字符串
	 * @details 配置Porter模块，加载策略和数据源等相关配置
	 *          该函数应在init_porter之后、run_porter之前调用
	 */
	EXPORT_FLAG	void		config_porter(const char* cfgfile, bool isFile);

	/**
	 * @brief 运行Porter模块
	 * @param bAsync 是否异步运行，如果为true，函数会立即返回；如果为false，函数会阻塞直到系统退出
	 * @details 启动并运行Porter模块，开始接收行情和执行策略
	 *          该函数应在init_porter和config_porter之后调用
	 *          如果使用异步模式，需要在程序退出前调用release_porter函数释放资源
	 */
	EXPORT_FLAG	void		run_porter(bool bAsync);

	/**
	 * @brief 写入日志
	 * @param level 日志级别，数值越大级别越高
	 * @param message 日志消息内容
	 * @param catName 日志类别名称，用于分类日志
	 * @details 将日志消息写入到框架的日志系统中
	 *          可以根据不同的日志级别和类别对日志进行过滤和分类
	 *          在需要记录策略运行状态或调试信息时非常有用
	 */
	EXPORT_FLAG	void		write_log(WtUInt32 level, const char* message, const char* catName);

	/**
	 * @brief 获取版本信息
	 * @return 返回当前框架的版本信息字符串
	 * @details 获取WonderTrader框架当前的版本信息
	 *          可用于在日志中记录版本号或显示给用户
	 */
	EXPORT_FLAG	WtString	get_version();

	/**
	 * @brief 注册CTA策略工厂
	 * @param factFolder 工厂文件夹路径，包含策略动态库文件
	 * @return 注册成功返回true，失败返回false
	 * @details 注册CTA策略工厂，框架会自动加载指定文件夹中的所有CTA策略动态库
	 *          策略动态库需要实现特定的导出函数，让框架能够识别并使用策略类
	 */
	EXPORT_FLAG	bool		reg_cta_factories(const char* factFolder);

	/**
	 * @brief 注册高频(HFT)策略工厂
	 * @param factFolder 工厂文件夹路径，包含策略动态库文件
	 * @return 注册成功返回true，失败返回false
	 * @details 注册高频策略工厂，框架会自动加载指定文件夹中的所有高频策略动态库
	 *          高频策略适用于快速交易场景，能够处理更细节的市场微观结构数据
	 */
	EXPORT_FLAG	bool		reg_hft_factories(const char* factFolder);

	/**
	 * @brief 注册选股(SEL)策略工厂
	 * @param factFolder 工厂文件夹路径，包含策略动态库文件
	 * @return 注册成功返回true，失败返回false
	 * @details 注册选股策略工厂，框架会自动加载指定文件夹中的所有选股策略动态库
	 *          选股策略主要用于股票池的管理和自动化交易，能够处理大量股票的选择和筛选
	 */
	EXPORT_FLAG	bool		reg_sel_factories(const char* factFolder);

	EXPORT_FLAG	bool		reg_exe_factories(const char* factFolder);

	EXPORT_FLAG	void		release_porter();

	EXPORT_FLAG	bool		create_ext_parser(const char* id);

	EXPORT_FLAG	bool		create_ext_executer(const char* id);

	EXPORT_FLAG	WtString	get_raw_stdcode(const char* stdCode);

	//////////////////////////////////////////////////////////////////////////
	//CTA策略接口
#pragma region "CTA接口"
	EXPORT_FLAG	CtxHandler	create_cta_context(const char* name, int slippage = 0);

	EXPORT_FLAG	void		cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	EXPORT_FLAG	void		cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	EXPORT_FLAG	void		cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	EXPORT_FLAG	void		cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	EXPORT_FLAG	double		cta_get_position_profit(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtUInt64	cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag);

	EXPORT_FLAG	double		cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag);

	EXPORT_FLAG	double		cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag);

	EXPORT_FLAG	double		cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double		cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag);

	EXPORT_FLAG	void		cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* uesrTag, double limitprice, double stopprice);

	EXPORT_FLAG	double 		cta_get_price(const char* stdCode);

	EXPORT_FLAG	double 		cta_get_day_price(const char* stdCode, int flag);

	EXPORT_FLAG	double		cta_get_fund_data(CtxHandler cHandle, int flag);

	EXPORT_FLAG	WtUInt32 	cta_get_tdate();

	EXPORT_FLAG	WtUInt32 	cta_get_date();

	EXPORT_FLAG	WtUInt32 	cta_get_time();

	EXPORT_FLAG	WtUInt32	cta_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, bool isMain, FuncGetBarsCallback cb);

	EXPORT_FLAG	WtUInt32	cta_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	EXPORT_FLAG	void		cta_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb);

	EXPORT_FLAG	WtUInt64	cta_get_first_entertime(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtUInt64	cta_get_last_entertime(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtUInt64	cta_get_last_exittime(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double		cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtString	cta_get_last_entertag(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	void		cta_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	EXPORT_FLAG	void		cta_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	EXPORT_FLAG	WtString	cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	EXPORT_FLAG	void		cta_sub_ticks(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	void		cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period);

	/*
	 *	设置图表K线
	 */
	EXPORT_FLAG void		cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period);

	/*
	 *	添加信号
	 */
	EXPORT_FLAG void		cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag);

	/*
	 *	添加指标
	 *	@idxName	指标名称
	 *	@indexType	指标类型：0-主图指标，1-副图指标
	 */
	EXPORT_FLAG void		cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType);

	/*
	 *	添加指标线
	 *	@idxName	指标名称
	 *	@lineName	线条名称
	 *	@lineType	线性，0-曲线
	 */
	EXPORT_FLAG bool		cta_register_index_line(CtxHandler cHandle, const char* idxName, const char* lineName, WtUInt32 lineType);

	/*
	 *	添加基准线
	 *	@idxName	指标名称
	 *	@lineName	线条名称
	 *	@val		数值
	 */
	EXPORT_FLAG bool		cta_add_index_baseline(CtxHandler cHandle, const char* idxName, const char* lineName, double val);

	/*
	 *	设置指标值
	 *	@idxName	指标名称
	 *	@lineName	线条名称
	 *	@val		指标值
	 */
	EXPORT_FLAG bool		cta_set_index_value(CtxHandler cHandle, const char* idxName, const char* lineName, double val);
#pragma endregion "CTA接口"

	//////////////////////////////////////////////////////////////////////////
	//选股策略接口
#pragma  region "SEL接口"
	EXPORT_FLAG	CtxHandler	create_sel_context(const char* name, uint32_t date, uint32_t time, const char* period, const char* trdtpl = "CHINA", const char* session = "TRADING", int32_t slippage = 0);

	EXPORT_FLAG	double		sel_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag);

	EXPORT_FLAG	void		sel_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* uesrTag);

	EXPORT_FLAG	double 		sel_get_price(const char* stdCode);

	EXPORT_FLAG	WtUInt32 	sel_get_date();

	EXPORT_FLAG	WtUInt32 	sel_get_time();

	EXPORT_FLAG	WtUInt32	sel_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb);

	EXPORT_FLAG	WtUInt32	sel_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	EXPORT_FLAG	void		sel_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb);

	EXPORT_FLAG	void		sel_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	EXPORT_FLAG	void		sel_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	EXPORT_FLAG	WtString	sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	EXPORT_FLAG	void		sel_sub_ticks(CtxHandler cHandle, const char* stdCode);

	//By Wesley @ 2023.05.17
	//扩展SEL的接口，主要是和CTA接口做一个同步
	EXPORT_FLAG	double		sel_get_position_profit(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtUInt64	sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag);

	EXPORT_FLAG	double		sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag);

	EXPORT_FLAG	double		sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag);

	EXPORT_FLAG	double		sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double 		sel_get_day_price(const char* stdCode, int flag);

	EXPORT_FLAG	double		sel_get_fund_data(CtxHandler cHandle, int flag);

	EXPORT_FLAG	WtUInt32 	sel_get_tdate();

	EXPORT_FLAG	WtUInt64	sel_get_first_entertime(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtUInt64	sel_get_last_entertime(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtUInt64	sel_get_last_exittime(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double		sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	WtString	sel_get_last_entertag(CtxHandler cHandle, const char* stdCode);
#pragma endregion "SEL接口"

	//////////////////////////////////////////////////////////////////////////
	//HFT策略接口
#pragma  region "HFT接口"
	EXPORT_FLAG	CtxHandler	create_hft_context(const char* name, const char* trader, bool agent, int32_t slippage = 0);

	EXPORT_FLAG	double		hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid);

	EXPORT_FLAG	double		hft_get_position_profit(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double		hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double		hft_get_undone(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	double 		hft_get_price(const char* stdCode);

	EXPORT_FLAG	WtUInt32 	hft_get_date();

	EXPORT_FLAG	WtUInt32 	hft_get_time();

	EXPORT_FLAG	WtUInt32 	hft_get_secs();

	EXPORT_FLAG	WtUInt32	hft_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb);

	EXPORT_FLAG	WtUInt32	hft_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	EXPORT_FLAG	WtUInt32	hft_get_ordque(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetOrdQueCallback cb);

	EXPORT_FLAG	WtUInt32	hft_get_orddtl(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetOrdDtlCallback cb);

	EXPORT_FLAG	WtUInt32	hft_get_trans(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTransCallback cb);

	EXPORT_FLAG	void		hft_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	EXPORT_FLAG	void		hft_sub_ticks(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	void		hft_sub_order_queue(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	void		hft_sub_order_detail(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	void		hft_sub_transaction(CtxHandler cHandle, const char* stdCode);

	EXPORT_FLAG	bool		hft_cancel(CtxHandler cHandle, WtUInt32 localid);

	EXPORT_FLAG	WtString	hft_cancel_all(CtxHandler cHandle, const char* stdCode, bool isBuy);

	EXPORT_FLAG	WtString	hft_buy(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag);

	EXPORT_FLAG	WtString	hft_sell(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag);

	EXPORT_FLAG	void		hft_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	EXPORT_FLAG	WtString	hft_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);
#pragma endregion "HFT接口"

#pragma region "扩展Parser接口"
	EXPORT_FLAG	void		parser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag);
#pragma endregion "扩展Parser接口"

#ifdef __cplusplus
}
#endif