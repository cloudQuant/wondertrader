/*!
 * \file WtBtPorter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader回测引擎接口定义
 * \details 定义了WonderTrader回测引擎的外部调用接口，包括通用接口、CTA策略接口、选股策略接口和HFT策略接口
 */
#pragma once
#include "PorterDefs.h"


#ifdef __cplusplus
extern "C"
{
#endif
	/**
	 * @brief 注册事件回调函数
	 * @details 注册用于接收回测引擎事件通知的回调函数
	 * @param cbEvt 事件回调函数指针
	 */
	EXPORT_FLAG	void		register_evt_callback(FuncEventCallback cbEvt);

	/**
	 * @brief 注册CTA策略回调函数
	 * @details 注册用于接收CTA策略各类事件的回调函数，包括初始化、行情数据、计算等
	 * @param cbInit 策略初始化回调函数
	 * @param cbTick Tick数据回调函数
	 * @param cbCalc 策略计算回调函数
	 * @param cbBar K线数据回调函数
	 * @param cbSessEvt 交易时段事件回调函数
	 * @param cbCalcDone 计算完成回调函数
	 * @param cbCondTrigger 条件触发回调函数，可选参数
	 */
	EXPORT_FLAG	void		register_cta_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
		FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone, FuncStraCondTriggerCallback cbCondTrigger = NULL);

	/**
	 * @brief 注册选股策略回调函数
	 * @details 注册用于接收选股策略各类事件的回调函数，包括初始化、行情数据、计算等
	 * @param cbInit 策略初始化回调函数
	 * @param cbTick Tick数据回调函数
	 * @param cbCalc 策略计算回调函数
	 * @param cbBar K线数据回调函数
	 * @param cbSessEvt 交易时段事件回调函数
	 * @param cbCalcDone 计算完成回调函数
	 */
	EXPORT_FLAG	void		register_sel_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
		FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone);

	/**
	 * @brief 注册HFT策略回调函数
	 * @details 注册用于接收高频交易策略各类事件的回调函数，包括初始化、行情数据、订单和成交等
	 * @param cbInit 策略初始化回调函数
	 * @param cbTick Tick数据回调函数
	 * @param cbBar K线数据回调函数
	 * @param cbChnl 通道数据回调函数
	 * @param cbOrd 订单回调函数
	 * @param cbTrd 成交回调函数
	 * @param cbEntrust 委托回调函数
	 * @param cbOrdDtl 订单明细回调函数
	 * @param cbOrdQue 订单队列回调函数
	 * @param cbTrans 成交明细回调函数
	 * @param cbSessEvt 交易时段事件回调函数
	 */
	EXPORT_FLAG	void		register_hft_callbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar,
		FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
		FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt);

	/**
	 * @brief 注册外部数据加载器
	 * @details 注册用于加载各类外部数据的函数，包括K线数据、复权因子和Tick数据
	 * @param fnlBarLoader 最终K线数据加载器
	 * @param rawBarLoader 原始K线数据加载器
	 * @param fctLoader 复权因子加载器
	 * @param tickLoader Tick数据加载器
	 * @param bAutoTrans 是否自动转换数据
	 */
	EXPORT_FLAG void		register_ext_data_loader(FuncLoadFnlBars fnlBarLoader, FuncLoadRawBars rawBarLoader, FuncLoadAdjFactors fctLoader, FuncLoadRawTicks tickLoader, bool bAutoTrans);

	/**
	 * @brief 输入原始K线数据
	 * @details 将原始K线数据输入到回测引擎中
	 * @param bars K线数据数组
	 * @param count K线数据数量
	 */
	EXPORT_FLAG void		feed_raw_bars(WTSBarStruct* bars, WtUInt32 count);

	/**
	 * @brief 输入原始Tick数据
	 * @details 将原始Tick数据输入到回测引擎中
	 * @param ticks Tick数据数组
	 * @param count Tick数据数量
	 */
	EXPORT_FLAG void		feed_raw_ticks(WTSTickStruct* ticks, WtUInt32 count);

	/**
	 * @brief 输入复权因子数据
	 * @details 将复权因子数据输入到回测引擎中，用于计算复权行情
	 * @param stdCode 标准合约代码
	 * @param dates 复权因子对应的日期数组
	 * @param factors 复权因子数组
	 * @param count 复权因子数量
	 */
	EXPORT_FLAG void		feed_adj_factors(WtString stdCode, WtUInt32* dates, double* factors, WtUInt32 count);

	/**
	 * @brief 初始化回测环境
	 * @details 初始化回测引擎的环境，包括日志配置和输出目录
	 * @param logProfile 日志配置文件或内容
	 * @param isFile 是否为文件，true表示logProfile是文件路径，false表示是配置内容
	 * @param outDir 输出目录
	 */
	EXPORT_FLAG	void		init_backtest(const char* logProfile, bool isFile, const char* outDir);

	/**
	 * @brief 配置回测环境
	 * @details 设置回测引擎的配置参数
	 * @param cfgfile 配置文件或内容
	 * @param isFile 是否为文件，true表示cfgfile是文件路径，false表示是配置内容
	 */
	EXPORT_FLAG	void		config_backtest(const char* cfgfile, bool isFile);

	/**
	 * @brief 设置回测时间范围
	 * @details 设置回测的起始时间和结束时间
	 * @param stime 起始时间，格式为YYYYMMDDHHmmss
	 * @param etime 结束时间，格式为YYYYMMDDHHmmss
	 */
	EXPORT_FLAG	void		set_time_range(WtUInt64 stime, WtUInt64 etime);

	/**
	 * @brief 启用Tick数据回测
	 * @details 设置是否启用Tick级别的回测，默认为启用
	 * @param bEnabled 是否启用Tick回测，默认为true
	 */
	EXPORT_FLAG	void		enable_tick(bool bEnabled = true);

	/**
	 * @brief 初始化CTA策略模拟器
	 * @details 创建并初始化一个CTA策略的模拟器实例
	 * @param name 策略名称
	 * @param slippage 滑点设置，默认为0
	 * @param hook 是否启用钩子函数，默认为false
	 * @param persistData 是否持久化数据，默认为true
	 * @param bIncremental 是否增量计算，默认为false
	 * @param bRatioSlp 是否使用百分比滑点，默认为false
	 * @return CtxHandler 返回策略上下文句柄
	 */
	EXPORT_FLAG	CtxHandler	init_cta_mocker(const char* name, int slippage = 0, bool hook = false, bool persistData = true, bool bIncremental = false, bool bRatioSlp = false);

	/**
	 * @brief 初始化HFT策略模拟器
	 * @details 创建并初始化一个高频交易策略的模拟器实例
	 * @param name 策略名称
	 * @param hook 是否启用钩子函数，默认为false
	 * @return CtxHandler 返回策略上下文句柄
	 */
	EXPORT_FLAG	CtxHandler	init_hft_mocker(const char* name, bool hook = false);

	/**
	 * @brief 初始化选股策略模拟器
	 * @details 创建并初始化一个选股策略的模拟器实例
	 * @param name 策略名称
	 * @param date 交易日期，格式YYYYMMDD
	 * @param time 交易时间，格式HHMMSS
	 * @param period 周期类型，如"d1"表示日线
	 * @param trdtpl 交易模板，默认为"CHINA"
	 * @param session 交易时段，默认为"TRADING"
	 * @param slippage 滑点设置，默认为0
	 * @param bRatioSlp 是否使用百分比滑点，默认为false
	 * @return CtxHandler 返回策略上下文句柄
	 */
	EXPORT_FLAG	CtxHandler	init_sel_mocker(const char* name, WtUInt32 date, WtUInt32 time, const char* period, const char* trdtpl = "CHINA", const char* session = "TRADING", int slippage = 0, bool bRatioSlp = false);

	/**
	 * @brief 运行回测
	 * @details 启动回测引擎并执行回测过程
	 * @param bNeedDump 是否需要输出回测结果
	 * @param bAsync 是否异步运行，true表示异步运行，false表示同步运行
	 */
	EXPORT_FLAG	void		run_backtest(bool bNeedDump, bool bAsync);

	/**
	 * @brief 写入日志
	 * @details 将日志信息写入到回测引擎的日志系统中
	 * @param level 日志级别
	 * @param message 日志消息
	 * @param catName 日志类别名称
	 */
	EXPORT_FLAG	void		write_log(WtUInt32 level, const char* message, const char* catName);

	/**
	 * @brief 获取版本信息
	 * @details 获取回测引擎的版本号
	 * @return WtString 返回版本号字符串
	 */
	EXPORT_FLAG	WtString	get_version();

	/**
	 * @brief 释放回测环境
	 * @details 释放回测引擎的资源，包括内存和其他系统资源
	 */
	EXPORT_FLAG	void		release_backtest();

	/**
	 * @brief 清理缓存
	 * @details 清理回测引擎中的数据缓存，释放内存
	 */
	EXPORT_FLAG	void		clear_cache();

	/**
	 * @brief 停止回测
	 * @details 停止正在运行的回测过程，通常用于异步回测模式
	 */
	EXPORT_FLAG	void		stop_backtest();

	/**
	 * @brief 获取原始标准合约代码
	 * @details 从标准化的合约代码获取原始的合约代码
	 * @param stdCode 标准化的合约代码
	 * @return WtString 返回原始合约代码字符串
	 */
	EXPORT_FLAG	WtString	get_raw_stdcode(const char* stdCode);


	//////////////////////////////////////////////////////////////////////////
	//CTA策略接口
#pragma region "CTA接口"
	/**
	 * @brief CTA策略建立多头仓位
	 * @details 在CTA策略中建立多头仓位（买入开仓）
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于识别交易
	 * @param limitprice 限价，为0表示不限价
	 * @param stopprice 止损价，为0表示不设置止损
	 */
	EXPORT_FLAG	void		cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief CTA策略平仓多头仓位
	 * @details 在CTA策略中平仓多头仓位（卖出平仓）
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于识别交易
	 * @param limitprice 限价，为0表示不限价
	 * @param stopprice 止损价，为0表示不设置止损
	 */
	EXPORT_FLAG	void		cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief CTA策略建立空头仓位
	 * @details 在CTA策略中建立空头仓位（卖出开仓）
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于识别交易
	 * @param limitprice 限价，为0表示不限价
	 * @param stopprice 止损价，为0表示不设置止损
	 */
	EXPORT_FLAG	void		cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief CTA策略平仓空头仓位
	 * @details 在CTA策略中平仓空头仓位（买入平仓）
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于识别交易
	 * @param limitprice 限价，为0表示不限价
	 * @param stopprice 止损价，为0表示不设置止损
	 */
	EXPORT_FLAG	void		cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief 获取CTA策略持仓盈亏
	 * @details 获取指定合约当前持仓的浮动盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回持仓盈亏，正值表示盈利，负值表示亏损
	 */
	EXPORT_FLAG	double		cta_get_position_profit(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取CTA策略开仓时间
	 * @details 获取指定合约和标签的开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param openTag 开仓标签，用于识别特定的开仓
	 * @return WtUInt64 返回开仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取CTA策略开仓成本
	 * @details 获取指定合约和标签的开仓成本
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param openTag 开仓标签，用于识别特定的开仓
	 * @return double 返回开仓成本
	 */
	EXPORT_FLAG	double		cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取CTA策略仓位盈亏
	 * @details 获取指定合约和标签的仓位盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param openTag 开仓标签，用于识别特定的开仓
	 * @param flag 盈亏标志，0-浮动盈亏，1-平仓盈亏
	 * @return double 返回仓位盈亏，正值表示盈利，负值表示亏损
	 */
	EXPORT_FLAG	double		cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag);

	/**
	 * @brief 获取CTA策略持仓均价
	 * @details 获取指定合约的持仓均价
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回持仓均价
	 */
	EXPORT_FLAG	double		cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取CTA策略持仓量
	 * @details 获取指定合约的持仓数量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只返回有效仓位，true表示只返回有效仓位，false表示返回所有仓位
	 * @param openTag 开仓标签，用于识别特定的开仓，如果为空则返回所有标签的仓位之和
	 * @return double 返回持仓量，正值表示多头仓位，负值表示空头仓位
	 */
	EXPORT_FLAG	double		cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag);

	/**
	 * @brief 设置CTA策略目标仓位
	 * @details 直接设置指定合约的目标仓位，系统会自动计算需要交易的数量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param qty 目标仓位数量，正值表示多头仓位，负值表示空头仓位
	 * @param uesrTag 用户自定义标签，用于识别交易
	 * @param limitprice 限价，为0表示不限价
	 * @param stopprice 止损价，为0表示不设置止损
	 */
	EXPORT_FLAG	void		cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* uesrTag, double limitprice, double stopprice);

	/**
	 * @brief 获取合约当前价格
	 * @details 获取指定合约的当前最新价格
	 * @param stdCode 标准化合约代码
	 * @return double 返回当前价格
	 */
	EXPORT_FLAG	double 		cta_get_price(const char* stdCode);

	/**
	 * @brief 获取合约日线价格
	 * @details 获取指定合约的日线价格数据
	 * @param stdCode 标准化合约代码
	 * @param flag 价格标志，0-开盘价，1-最高价，2-最低价，3-收盘价
	 * @return double 返回日线价格
	 */
	EXPORT_FLAG	double 		cta_get_day_price(const char* stdCode, int flag);

	/**
	 * @brief 获取CTA策略资金数据
	 * @details 获取策略账户的资金相关数据
	 * @param cHandle 策略上下文句柄
	 * @param flag 资金数据标志，0-动态权益，1-静态权益，2-可用资金
	 * @return double 返回资金数据
	 */
	EXPORT_FLAG	double		cta_get_fund_data(CtxHandler cHandle, int flag);

	/**
	 * @brief 获取当前交易日期
	 * @details 获取回测引擎中当前的交易日期
	 * @return WtUInt32 返回交易日期，格式YYYYMMDD
	 */
	EXPORT_FLAG	WtUInt32 	cta_get_tdate();

	/**
	 * @brief 获取当前自然日期
	 * @details 获取回测引擎中当前的自然日期，与交易日期不同
	 * @return WtUInt32 返回自然日期，格式YYYYMMDD
	 */
	EXPORT_FLAG	WtUInt32 	cta_get_date();

	/**
	 * @brief 获取当前交易时间
	 * @details 获取回测引擎中当前的交易时间
	 * @return WtUInt32 返回交易时间，格式HHMMSS
	 */
	EXPORT_FLAG	WtUInt32 	cta_get_time();

	/**
	 * @brief 获取K线数据
	 * @details 获取指定合约的历史K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param period 周期类型，如"m1"表示1分钟，"d1"表示日线
	 * @param barCnt 请求的K线数量
	 * @param isMain 是否为主力合约
	 * @param cb 回调函数，用于接收K线数据
	 * @return WtUInt32 返回实际获取的K线数量
	 */
	EXPORT_FLAG	WtUInt32	cta_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, bool isMain, FuncGetBarsCallback cb);

	/**
	 * @brief 获取Tick数据
	 * @details 获取指定合约的历史Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param tickCnt 请求的Tick数量
	 * @param cb 回调函数，用于接收Tick数据
	 * @return WtUInt32 返回实际获取的Tick数量
	 */
	EXPORT_FLAG	WtUInt32	cta_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	/**
	 * @brief 获取所有持仓
	 * @details 获取策略的所有合约持仓信息
	 * @param cHandle 策略上下文句柄
	 * @param cb 回调函数，用于接收持仓信息
	 */
	EXPORT_FLAG void		cta_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb);

	/**
	 * @brief 获取首次开仓时间
	 * @details 获取指定合约的首次开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtUInt64 返回首次开仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	cta_get_first_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次开仓时间
	 * @details 获取指定合约的最后一次开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtUInt64 返回最后一次开仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	cta_get_last_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次平仓时间
	 * @details 获取指定合约的最后一次平仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtUInt64 返回最后一次平仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	cta_get_last_exittime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次开仓价格
	 * @details 获取指定合约的最后一次开仓价格
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回最后一次开仓价格
	 */
	EXPORT_FLAG	double		cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次开仓标记
	 * @details 获取指定合约的最后一次开仓标记
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtString 返回最后一次开仓标记
	 */
	EXPORT_FLAG	WtString	cta_get_last_entertag(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 输出日志信息
	 * @details 在CTA策略中输出日志信息
	 * @param cHandle 策略上下文句柄
	 * @param level 日志级别
	 * @param message 日志消息内容
	 */
	EXPORT_FLAG	void		cta_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	/**
	 * @brief 保存用户数据
	 * @details 将用户自定义数据保存到策略上下文中
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param val 数据值
	 */
	EXPORT_FLAG	void		cta_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	/**
	 * @brief 加载用户数据
	 * @details 从策略上下文中加载用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param defVal 默认值，当键名不存在时返回此值
	 * @return WtString 返回加载的数据值
	 */
	EXPORT_FLAG	WtString	cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	/**
	 * @brief 订阅Tick数据
	 * @details 在CTA策略中订阅指定合约的Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 */
	EXPORT_FLAG	void		cta_sub_ticks(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 订阅K线事件
	 * @details 在CTA策略中订阅指定合约的K线周期事件
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param period 周期类型，如"m1"表示1分钟，"d1"表示日线
	 */
	EXPORT_FLAG	void		cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period);

	/**
	 * @brief 执行策略单步
	 * @details 手动执行CTA策略的单步运行，用于手动控制策略运行节奏
	 * @param cHandle 策略上下文句柄
	 * @return bool 返回是否执行成功
	 */
	EXPORT_FLAG	bool		cta_step(CtxHandler cHandle);

		/**
	 * @brief 设置图表K线
	 * @details 设置回测图表中显示的K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param period 周期类型，如"m1"表示1分钟，"d1"表示日线
	 */
	EXPORT_FLAG void		cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period);

		/**
	 * @brief 添加图表标记
	 * @details 在回测图表中添加交易信号或标记点
	 * @param cHandle 策略上下文句柄
	 * @param price 标记位置的价格
	 * @param icon 图标类型，用于图表显示
	 * @param tag 标记的文字描述
	 */
	EXPORT_FLAG void		cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag);

		/**
	 * @brief 注册图表指标
	 * @details 在回测图表中添加自定义技术指标
	 * @param cHandle 策略上下文句柄
	 * @param idxName 指标名称
	 * @param indexType 指标类型：0-主图指标，1-副图指标
	 */
	EXPORT_FLAG void		cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType);

		/**
	 * @brief 注册指标线
	 * @details 为已注册的指标添加线条
	 * @param cHandle 策略上下文句柄
	 * @param idxName 指标名称，必须是已经注册过的指标
	 * @param lineName 线条名称
	 * @param lineType 线条类型，0-曲线
	 * @return bool 返回是否注册成功
	 */
	EXPORT_FLAG bool		cta_register_index_line(CtxHandler cHandle, const char* idxName, const char* lineName, WtUInt32 lineType);

		/**
	 * @brief 添加指标基准线
	 * @details 为指标添加水平基准线，如超买超卖线
	 * @param cHandle 策略上下文句柄
	 * @param idxName 指标名称，必须是已经注册过的指标
	 * @param lineName 基准线名称
	 * @param val 基准线数值
	 * @return bool 返回是否添加成功
	 */
	EXPORT_FLAG bool		cta_add_index_baseline(CtxHandler cHandle, const char* idxName, const char* lineName, double val);

		/**
	 * @brief 设置指标线数值
	 * @details 设置指定指标线的当前值，用于更新图表显示
	 * @param cHandle 策略上下文句柄
	 * @param idxName 指标名称，必须是已经注册过的指标
	 * @param lineName 线条名称，必须是已经注册过的线条
	 * @param val 指标线的当前值
	 * @return bool 返回是否设置成功
	 */
	EXPORT_FLAG bool		cta_set_index_value(CtxHandler cHandle, const char* idxName, const char* lineName, double val);

#pragma endregion "CTA接口"

	//////////////////////////////////////////////////////////////////////////
	//选股策略接口
#pragma  region "SEL接口"
	/**
	 * @brief 获取选股策略持仓量
	 * @details 获取选股策略中指定合约的持仓量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只计算有效仓位
	 * @param openTag 开仓标记，用于区分不同来源的仓位
	 * @return double 返回持仓量
	 */
	EXPORT_FLAG	double		sel_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag);

	/**
	 * @brief 设置选股策略持仓量
	 * @details 设置选股策略中指定合约的目标持仓量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param qty 目标持仓量
	 * @param uesrTag 用户自定义标记，用于识别交易来源
	 */
	EXPORT_FLAG	void		sel_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* uesrTag);

	/**
	 * @brief 获取合约最新价格
	 * @details 获取选股策略中指定合约的最新价格
	 * @param stdCode 标准化合约代码
	 * @return double 返回合约最新价格
	 */
	EXPORT_FLAG	double 		sel_get_price(const char* stdCode);

	/**
	 * @brief 获取当前日期
	 * @details 获取选股策略回测引擎中当前的自然日期
	 * @return WtUInt32 返回自然日期，格式YYYYMMDD
	 */
	EXPORT_FLAG	WtUInt32 	sel_get_date();

	/**
	 * @brief 获取当前时间
	 * @details 获取选股策略回测引擎中当前的交易时间
	 * @return WtUInt32 返回交易时间，格式HHMMSS
	 */
	EXPORT_FLAG	WtUInt32 	sel_get_time();

	/**
	 * @brief 获取K线数据
	 * @details 获取选股策略中指定合约的历史K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param period 周期类型，如"m1"表示1分钟，"d1"表示日线
	 * @param barCnt 请求的K线数量
	 * @param cb 回调函数，用于接收K线数据
	 * @return WtUInt32 返回实际获取的K线数量
	 */
	EXPORT_FLAG	WtUInt32	sel_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt,FuncGetBarsCallback cb);

	/**
	 * @brief 获取Tick数据
	 * @details 获取选股策略中指定合约的历史Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param tickCnt 请求的Tick数量
	 * @param cb 回调函数，用于接收Tick数据
	 * @return WtUInt32 返回实际获取的Tick数量
	 */
	EXPORT_FLAG	WtUInt32	sel_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt,FuncGetTicksCallback cb);

	/**
	 * @brief 获取所有持仓
	 * @details 获取选股策略的所有合约持仓信息
	 * @param cHandle 策略上下文句柄
	 * @param cb 回调函数，用于接收持仓信息
	 */
	EXPORT_FLAG void		sel_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb);

	/**
	 * @brief 输出日志信息
	 * @details 在选股策略中输出日志信息
	 * @param cHandle 策略上下文句柄
	 * @param level 日志级别
	 * @param message 日志消息内容
	 */
	EXPORT_FLAG	void		sel_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	/**
	 * @brief 保存用户数据
	 * @details 将用户自定义数据保存到选股策略上下文中
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param val 数据值
	 */
	EXPORT_FLAG	void		sel_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	/**
	 * @brief 加载用户数据
	 * @details 从选股策略上下文中加载用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param defVal 默认值，当键名不存在时返回此值
	 * @return WtString 返回加载的数据值
	 */
	EXPORT_FLAG	WtString	sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	/**
	 * @brief 订阅Tick数据
	 * @details 在选股策略中订阅指定合约的Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 */
	EXPORT_FLAG	void		sel_sub_ticks(CtxHandler cHandle, const char* stdCode);

	//By Wesley @ 2023.05.17
	//扩展SEL的接口，主要是和CTA接口做一个同步
	/**
	 * @brief 获取持仓盈亏
	 * @details 获取选股策略中指定合约的持仓盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回持仓盈亏金额
	 */
	EXPORT_FLAG	double		sel_get_position_profit(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取详细开仓时间
	 * @details 获取选股策略中指定合约和标记的开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param openTag 开仓标记，用于区分不同来源的仓位
	 * @return WtUInt64 返回开仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取详细开仓成本
	 * @details 获取选股策略中指定合约和标记的开仓成本
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param openTag 开仓标记，用于区分不同来源的仓位
	 * @return double 返回开仓成本
	 */
	EXPORT_FLAG	double		sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取详细盈亏
	 * @details 获取选股策略中指定合约和标记的盈亏信息
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param openTag 开仓标记，用于区分不同来源的仓位
	 * @param flag 盈亏标志，0-浮动盈亏，1-平仓盈亏
	 * @return double 返回盈亏金额
	 */
	EXPORT_FLAG	double		sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag);

	/**
	 * @brief 获取持仓均价
	 * @details 获取选股策略中指定合约的持仓均价
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回持仓均价
	 */
	EXPORT_FLAG	double		sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取日线价格数据
	 * @details 获取选股策略中指定合约的日线价格数据
	 * @param stdCode 标准化合约代码
	 * @param flag 价格标志，0-开盘价，1-最高价，2-最低价，3-收盘价
	 * @return double 返回对应的价格数据
	 */
	EXPORT_FLAG	double 		sel_get_day_price(const char* stdCode, int flag);

	/**
	 * @brief 获取选股策略资金数据
	 * @details 获取选股策略账户的资金相关数据
	 * @param cHandle 策略上下文句柄
	 * @param flag 资金数据标志，0-动态权益，1-静态权益，2-可用资金
	 * @return double 返回资金数据
	 */
	EXPORT_FLAG	double		sel_get_fund_data(CtxHandler cHandle, int flag);

	/**
	 * @brief 获取当前交易日期
	 * @details 获取选股策略回测引擎中当前的交易日期
	 * @return WtUInt32 返回交易日期，格式YYYYMMDD
	 */
	EXPORT_FLAG	WtUInt32 	sel_get_tdate();

	/**
	 * @brief 获取首次开仓时间
	 * @details 获取选股策略中指定合约的首次开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtUInt64 返回首次开仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	sel_get_first_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次开仓时间
	 * @details 获取选股策略中指定合约的最后一次开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtUInt64 返回最后一次开仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	sel_get_last_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次平仓时间
	 * @details 获取选股策略中指定合约的最后一次平仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtUInt64 返回最后一次平仓时间，格式为YYYYMMDDHHMMSSmmm
	 */
	EXPORT_FLAG	WtUInt64	sel_get_last_exittime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次开仓价格
	 * @details 获取选股策略中指定合约的最后一次开仓价格
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回最后一次开仓价格
	 */
	EXPORT_FLAG	double		sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取最后一次开仓标记
	 * @details 获取选股策略中指定合约的最后一次开仓标记
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return WtString 返回最后一次开仓标记
	 */
	EXPORT_FLAG	WtString	sel_get_last_entertag(CtxHandler cHandle, const char* stdCode);

#pragma endregion "SEL接口"

	//////////////////////////////////////////////////////////////////////////
//HFT策略接口
#pragma  region "HFT接口"

	/**
	 * @brief 获取HFT策略持仓量
	 * @details 获取高频策略中指定合约的持仓量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只计算有效仓位
	 * @return double 返回持仓量
	 */
	EXPORT_FLAG	double		hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid);

	/**
	 * @brief 获取持仓盈亏
	 * @details 获取高频策略中指定合约的持仓盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回持仓盈亏金额
	 */
	EXPORT_FLAG	double		hft_get_position_profit(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取持仓均价
	 * @details 获取高频策略中指定合约的持仓均价
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回持仓均价
	 */
	EXPORT_FLAG	double		hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取未完成数量
	 * @details 获取高频策略中指定合约的未完成委托数量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @return double 返回未完成委托数量
	 */
	EXPORT_FLAG	double		hft_get_undone(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取合约最新价格
	 * @details 获取高频策略中指定合约的最新价格
	 * @param stdCode 标准化合约代码
	 * @return double 返回合约最新价格
	 */
	EXPORT_FLAG	double 		hft_get_price(const char* stdCode);

	/**
	 * @brief 获取当前日期
	 * @details 获取高频策略回测引擎中当前的自然日期
	 * @return WtUInt32 返回自然日期，格式YYYYMMDD
	 */
	EXPORT_FLAG	WtUInt32 	hft_get_date();

	/**
	 * @brief 获取当前时间
	 * @details 获取高频策略回测引擎中当前的交易时间
	 * @return WtUInt32 返回交易时间，格式HHMMSS
	 */
	EXPORT_FLAG	WtUInt32 	hft_get_time();

	/**
	 * @brief 获取当前秒数
	 * @details 获取高频策略回测引擎中当前的秒数，用于高精度时间控制
	 * @return WtUInt32 返回当前秒数
	 */
	EXPORT_FLAG	WtUInt32 	hft_get_secs();

	/**
	 * @brief 获取K线数据
	 * @details 获取高频策略中指定合约的历史K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param period 周期类型，如"m1"表示1分钟，"d1"表示日线
	 * @param barCnt 请求的K线数量
	 * @param cb 回调函数，用于接收K线数据
	 * @return WtUInt32 返回实际获取的K线数量
	 */
	EXPORT_FLAG	WtUInt32	hft_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb);

	/**
	 * @brief 获取Tick数据
	 * @details 获取高频策略中指定合约的历史Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param tickCnt 请求的Tick数量
	 * @param cb 回调函数，用于接收Tick数据
	 * @return WtUInt32 返回实际获取的Tick数量
	 */
	EXPORT_FLAG	WtUInt32	hft_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	/**
	 * @brief 获取委托队列数据
	 * @details 获取高频策略中指定合约的历史委托队列数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param tickCnt 请求的委托队列数量
	 * @param cb 回调函数，用于接收委托队列数据
	 * @return WtUInt32 返回实际获取的委托队列数量
	 */
	EXPORT_FLAG	WtUInt32	hft_get_ordque(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetOrdQueCallback cb);

	/**
	 * @brief 获取委托明细数据
	 * @details 获取高频策略中指定合约的历史委托明细数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param tickCnt 请求的委托明细数量
	 * @param cb 回调函数，用于接收委托明细数据
	 * @return WtUInt32 返回实际获取的委托明细数量
	 */
	EXPORT_FLAG	WtUInt32	hft_get_orddtl(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetOrdDtlCallback cb);

	/**
	 * @brief 获取成交明细数据
	 * @details 获取高频策略中指定合约的历史成交明细数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param tickCnt 请求的成交明细数量
	 * @param cb 回调函数，用于接收成交明细数据
	 * @return WtUInt32 返回实际获取的成交明细数量
	 */
	EXPORT_FLAG	WtUInt32	hft_get_trans(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTransCallback cb);

	/**
	 * @brief 输出日志信息
	 * @details 在高频策略中输出日志信息
	 * @param cHandle 策略上下文句柄
	 * @param level 日志级别
	 * @param message 日志消息内容
	 */
	EXPORT_FLAG	void		hft_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	/**
	 * @brief 订阅Tick数据
	 * @details 在高频策略中订阅指定合约的Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 */
	EXPORT_FLAG	void		hft_sub_ticks(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 订阅委托队列数据
	 * @details 在高频策略中订阅指定合约的委托队列数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 */
	EXPORT_FLAG	void		hft_sub_order_queue(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 订阅委托明细数据
	 * @details 在高频策略中订阅指定合约的委托明细数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 */
	EXPORT_FLAG	void		hft_sub_order_detail(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 订阅成交明细数据
	 * @details 在高频策略中订阅指定合约的成交明细数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 */
	EXPORT_FLAG	void		hft_sub_transaction(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 撤销委托
	 * @details 在高频策略中撤销指定本地ID的委托
	 * @param cHandle 策略上下文句柄
	 * @param localid 委托的本地ID
	 * @return bool 返回是否撤单成功
	 */
	EXPORT_FLAG	bool		hft_cancel(CtxHandler cHandle, WtUInt32 localid);

	/**
	 * @brief 撤销所有委托
	 * @details 在高频策略中撤销指定合约的所有委托
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否只撤销买单，true-只撤销买单，false-只撤销卖单
	 * @return WtString 返回撤单结果信息
	 */
	EXPORT_FLAG	WtString	hft_cancel_all(CtxHandler cHandle, const char* stdCode, bool isBuy);

	/**
	 * @brief 发出买入委托
	 * @details 在高频策略中发出买入委托
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param price 委托价格
	 * @param qty 委托数量
	 * @param userTag 用户自定义标记
	 * @param flag 委托标志，如市价单、限价单等
	 * @return WtString 返回委托ID
	 */
	EXPORT_FLAG	WtString	hft_buy(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag);

	/**
	 * @brief 发出卖出委托
	 * @details 在高频策略中发出卖出委托
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param price 委托价格
	 * @param qty 委托数量
	 * @param userTag 用户自定义标记
	 * @param flag 委托标志，如市价单、限价单等
	 * @return WtString 返回委托ID
	 */
	EXPORT_FLAG	WtString	hft_sell(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag);

	/**
	 * @brief 保存用户数据
	 * @details 将用户自定义数据保存到高频策略上下文中
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param val 数据值
	 */
	EXPORT_FLAG	void		hft_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	/**
	 * @brief 加载用户数据
	 * @details 从高频策略上下文中加载用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param defVal 默认值，当键名不存在时返回此值
	 * @return WtString 返回加载的数据值
	 */
	EXPORT_FLAG	WtString	hft_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	/**
	 * @brief 执行策略单步
	 * @details 手动执行高频策略的单步运行，用于手动控制策略运行节奏
	 * @param cHandle 策略上下文句柄
	 */
	EXPORT_FLAG	void		hft_step(CtxHandler cHandle);
#pragma endregion "HFT接口"
#ifdef __cplusplus
}
#endif