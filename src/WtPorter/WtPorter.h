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

	/**
	 * @brief 注册执行器工厂
	 * @param factFolder 工厂文件夹路径，包含执行器动态库文件
	 * @return 注册成功返回true，失败返回false
	 * @details 注册执行器工厂，框架会自动加载指定文件夹中的所有执行器动态库
	 *          执行器负责将策略生成的交易信号转化为实际的交易指令并发送到交易系统
	 */
	EXPORT_FLAG	bool		reg_exe_factories(const char* factFolder);

	/**
	 * @brief 释放Porter模块资源
	 * @details 清理并释放Porter模块占用的各种资源
	 *          在异步运行模式下，应在程序退出前调用此函数以释放资源
	 *          调用后不能再使用其他Porter接口函数，除非重新初始化
	 */
	EXPORT_FLAG	void		release_porter();

	/**
	 * @brief 创建外部数据解析器
	 * @param id 数据解析器标识符
	 * @return 创建成功返回true，失败返回false
	 * @details 创建外部数据解析器实例，用于接入自定义的数据源
	 *          需要先调用register_parser_callbacks注册相关回调对外部解析器进行处理
	 */
	EXPORT_FLAG	bool		create_ext_parser(const char* id);

	/**
	 * @brief 创建外部执行器
	 * @param id 执行器标识符
	 * @return 创建成功返回true，失败返回false
	 * @details 创建外部执行器实例，用于接入自定义的交易执行器
	 *          需要先调用register_exec_callbacks注册相关回调对外部执行器进行处理
	 *          执行器负责将策略生成的交易信号转化为实际的交易指令
	 */
	EXPORT_FLAG	bool		create_ext_executer(const char* id);

	/**
	 * @brief 获取原始标准合约代码
	 * @param stdCode 标准合约代码
	 * @return 原始合约代码字符串
	 * @details 将框架内部使用的标准化合约代码转换为原始的交易所合约代码
	 *          当需要与外部系统交互或在用户界面显示原始合约时非常有用
	 */
	EXPORT_FLAG	WtString	get_raw_stdcode(const char* stdCode);

	//////////////////////////////////////////////////////////////////////////
	//CTA策略接口
#pragma region "CTA接口"
	/**
	 * @brief 创建CTA策略上下文
	 * @param name 策略名称
	 * @param slippage 滑点设置，默认为0
	 * @return 返回策略上下文句柄
	 * @details 创建一个新的CTA策略上下文，用于执行策略相关的操作
	 *          每个策略实例都需要一个独立的上下文来管理其状态和交互
	 *          滑点参数用于模拟实际交易中的价格滑点，提高回测真实性
	 */
	EXPORT_FLAG	CtxHandler	create_cta_context(const char* name, int slippage = 0);

	/**
	 * @brief CTA策略开多仓
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于标识交易
	 * @param limitprice 限价，如果希望市价委托，可以传入0
	 * @param stopprice 止损价，不使用止损价可以传入0
	 * @details 执行多头开仓操作，即买入并持有合约
	 *          可以指定限价和止损价，实现更精细的交易控制
	 *          userTag可以用于跟踪交易标的来源和目的
	 */
	EXPORT_FLAG	void		cta_enter_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief CTA策略平多仓
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param qty 平仓数量
	 * @param userTag 用户自定义标签，用于标识交易
	 * @param limitprice 限价，如果希望市价委托，可以传入0
	 * @param stopprice 止损价，不使用止损价可以传入0
	 * @details 执行多头平仓操作，即卖出持有的多头合约
	 *          可以指定限价和止损价，实现更精细的交易控制
	 *          userTag可以用于跟踪交易标的来源和目的
	 */
	EXPORT_FLAG	void		cta_exit_long(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief CTA策略开空仓
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param qty 交易数量
	 * @param userTag 用户自定义标签，用于标识交易
	 * @param limitprice 限价，如果希望市价委托，可以传入0
	 * @param stopprice 止损价，不使用止损价可以传入0
	 * @details 执行空头开仓操作，即卖出合约并形成空头持仓
	 *          可以指定限价和止损价，实现更精细的交易控制
	 *          userTag可以用于跟踪交易标的来源和目的
	 */
	EXPORT_FLAG	void		cta_enter_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief CTA策略平空仓
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param qty 平仓数量
	 * @param userTag 用户自定义标签，用于标识交易
	 * @param limitprice 限价，如果希望市价委托，可以传入0
	 * @param stopprice 止损价，不使用止损价可以传入0
	 * @details 执行空头平仓操作，即买入合约并平掉空头持仓
	 *          可以指定限价和止损价，实现更精细的交易控制
	 *          userTag可以用于跟踪交易标的来源和目的
	 */
	EXPORT_FLAG	void		cta_exit_short(CtxHandler cHandle, const char* stdCode, double qty, const char* userTag, double limitprice, double stopprice);

	/**
	 * @brief 获取指定合约的持仓盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回持仓盈亏金额，如果没有持仓则返回0
	 * @details 获取当前策略对指定合约的持仓盈亏
	 *          持仓盈亏是根据最新行情价格计算的浮动盈亏
	 *          在策略运行中可以用来实时监控盈亏状况
	 */
	EXPORT_FLAG	double		cta_get_position_profit(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定标签持仓的开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param openTag 开仓标签
	 * @return 返回开仓时间，格式为YYYYMMDDHHMMSS，如果没有存在相应持仓返回0
	 * @details 获取策略指定合约和标签的开仓时间
	 *          开仓标签是在开仓时自定义的标识，可以用于跟踪特定交易
	 *          时间格式为YYYYMMDDHHMMSS，便于进行时间计算和比较
	 */
	EXPORT_FLAG	WtUInt64	cta_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取指定标签持仓的开仓成本
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param openTag 开仓标签
	 * @return 返回开仓成本，如果没有存在相应持仓返回0
	 * @details 获取策略指定合约和标签的开仓成本
	 *          开仓成本是开仓时的单位平均价格，用于计算持仓盈亏
	 *          可用于与当前市场价格比较，进行止损或止盈决策
	 */
	EXPORT_FLAG	double		cta_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取指定标签持仓的收益
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param openTag 开仓标签
	 * @param flag 收益类型标志，0-浮动盈亏，1-平仓盈亏
	 * @return 返回指定标签持仓的收益值，如果没有存在相应持仓返回0
	 * @details 获取策略指定合约、标签和类型的持仓收益
	 *          当flag为0时，返回根据当前行情价格计算的浮动盈亏
	 *          当flag为1时，返回已实现的平仓盈亏
	 *          可用于策略的收益跟踪和风险管理
	 */
	EXPORT_FLAG	double		cta_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag);

	/**
	 * @brief 获取指定合约的持仓平均价格
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回持仓平均价格，如果没有持仓则返回0
	 * @details 获取策略指定合约的所有持仓的平均价格
	 *          平均价格考虑了多次开仓的价格差异，是根据总成本除以总量计算得到的
	 *          可用于判断当前持仓的成本水平和进行止盈止损决策
	 */
	EXPORT_FLAG	double		cta_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定合约的持仓量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param bOnlyValid 是否只计算有效仓位，不包括过渡仓位
	 * @param openTag 开仓标签，如果不为空，则只取指定标签的持仓
	 * @return 返回持仓量，多头仓位返回正数，空头仓位返回负数
	 * @details 获取策略指定合约的持仓数量
	 *          有效仓位是指已经确认开仓成功的持仓，不包括过渡仓位
	 *          过渡仓位是指发出开仓请求但还未成交的持仓
	 */
	EXPORT_FLAG	double		cta_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag);

	/**
	 * @brief 设置CTA策略指定合约的目标仓位
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量，正数表示多头仓位，负数表示空头仓位
	 * @param uesrTag 用户自定义标签
	 * @param limitprice 限价，如果希望市价委托，可以传入0
	 * @param stopprice 止损价，不使用止损价可以传入0
	 * @details 设置策略指定合约的目标仓位，框架会自动根据当前仓位和目标仓位的差异进行开仓或平仓
	 *          这是一个非常方便的函数，可以直接设置目标持仓，而不需要手动计算开平仓数量
	 *          如果当前没有持仓，会直接开仓到目标仓位；如果已有持仓，会自动计算差异并加减仓位
	 */
	EXPORT_FLAG	void		cta_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* uesrTag, double limitprice, double stopprice);

	/**
	 * @brief 获取指定合约的最新价格
	 * @param stdCode 标准合约代码
	 * @return 返回合约的最新价格，如果没有行情数据则返回0
	 * @details 获取指定合约的最新市场价格
	 *          这个价格通常是最新成交价，用于策略判断当前市场状况
	 *          在策略计算中非常有用，如计算指标、做出交易决策等
	 */
	EXPORT_FLAG	double 		cta_get_price(const char* stdCode);

	/**
	 * @brief 获取指定合约的当日价格数据
	 * @param stdCode 标准合约代码
	 * @param flag 价格标志：0-开盘价，1-最高价，2-最低价，3-收盘价
	 * @return 返回指定类型的当日价格，如果没有数据则返回0
	 * @details 获取指定合约当天的开盘价、最高价、最低价或收盘价
	 *          这些价格信息在进行日内策略计算时非常有用
	 *          策略可以基于当日的高低点来设定交易策略和风险控制
	 */
	EXPORT_FLAG	double 		cta_get_day_price(const char* stdCode, int flag);

	/**
	 * @brief 获取策略账户资金数据
	 * @param cHandle 策略上下文句柄
	 * @param flag 资金数据标志：0-动态权益，1-静态权益，2-可用资金
	 * @return 返回指定类型的资金数据
	 * @details 获取策略账户的资金相关数据
	 *          动态权益包含浮动盈亏，静态权益不包含浮动盈亏
	 *          可用资金是当前可以用于开仓的资金量
	 *          这些数据在策略的风险管理和资金分配中非常有用
	 */
	EXPORT_FLAG	double		cta_get_fund_data(CtxHandler cHandle, int flag);

	/**
	 * @brief 获取当前交易日期
	 * @return 返回当前交易日期，格式YYYYMMDD
	 * @details 获取系统当前的交易日期
	 *          交易日期是指股票、期货市场的交易日，非自然日
	 *          在策略计算中经常需要知道当前的交易日期以进行各种判断
	 */
	EXPORT_FLAG	WtUInt32 	cta_get_tdate();

	/**
	 * @brief 获取当前日期
	 * @return 返回当前日期，格式YYYYMMDD
	 * @details 获取系统当前日期，这是自然日期，除非模拟交易环境下会根据回测数据设置
	 *          与交易日期不同，自然日期包含了周末和节假日
	 *          在策略中可用于日志记录、时间计算等场景
	 */
	EXPORT_FLAG	WtUInt32 	cta_get_date();

	/**
	 * @brief 获取当前时间
	 * @return 返回当前时间，格式HHMMSS，例如235959
	 * @details 获取系统当前时间，除非模拟交易环境下会根据回测数据设置
	 *          这个时间在策略中非常重要，特别是在日内策略中
	 *          可用于判断当前是否在交易时段内、交易时间限制等
	 */
	EXPORT_FLAG	WtUInt32 	cta_get_time();

	/**
	 * @brief 获取历史K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @param barCnt 请求的K线数量
	 * @param isMain 是否是主力合约
	 * @param cb 回调函数，用于接收返回的K线数据
	 * @return 返回实际获取到的K线数量
	 * @details 获取指定合约的历史K线数据，并通过回调函数返回给策略
	 *          実际返回的K线数量可能少于请求的数量，这取决于数据库中的可用数据
	 *          策略可以根据这些K线数据计算指标、生成交易信号等
	 */
	EXPORT_FLAG	WtUInt32	cta_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, bool isMain, FuncGetBarsCallback cb);

	/**
	 * @brief 获取历史Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param tickCnt 请求的Tick数量
	 * @param cb 回调函数，用于接收返回的Tick数据
	 * @return 返回实际获取到的Tick数量
	 * @details 获取指定合约的历史Tick数据，并通过回调函数返回给策略
	 *          Tick数据是最小的行情单元，包含密集的成交、委托信息
	 *          高频策略中经常需要这些微观市场结构数据来分析和决策
	 */
	EXPORT_FLAG	WtUInt32	cta_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	/**
	 * @brief 获取策略的所有持仓信息
	 * @param cHandle 策略上下文句柄
	 * @param cb 回调函数，用于接收返回的持仓数据
	 * @details 获取策略当前的所有持仓信息，并通过回调函数返回
	 *          每个持仓包含标准合约代码、持仓方向、数量等信息
	 *          这对于策略的风险管理和持仓监控非常有用
	 */
	EXPORT_FLAG	void		cta_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb);

	/**
	 * @brief 获取指定合约的首次开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回首次开仓时间，格式YYYYMMDDHHMMSS，如果没有相关持仓则返回0
	 * @details 获取指定合约的最早一笔开仓的时间
	 *          开仓时间是指策略对某个合约建立仓位的时间
	 *          可用于计算持仓时间，判断显示持仓久度等
	 */
	EXPORT_FLAG	WtUInt64	cta_get_first_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定合约的最近开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近开仓时间，格式YYYYMMDDHHMMSS，如果没有相关持仓则返回0
	 * @details 获取指定合约的最近一笔开仓的时间
	 *          与cta_get_first_entertime不同，这个函数返回的是最近的开仓时间
	 *          可用于判断最近是否有新增仓位，跟踪近期的开仓操作等
	 */
	EXPORT_FLAG	WtUInt64	cta_get_last_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定合约的最近平仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近平仓时间，格式YYYYMMDDHHMMSS，如果没有平仓记录则返回0
	 * @details 获取指定合约的最近一笔平仓的时间
	 *          平仓时间是指策略对某个合约清空仓位的时间
	 *          可用于判断最近是否有平仓操作，以及实现一些时间限制，如平仓后的冷静期
	 */
	EXPORT_FLAG	WtUInt64	cta_get_last_exittime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定合约的最近开仓价格
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近开仓价格，如果没有相关持仓则返回0
	 * @details 获取指定合约的最近一笔开仓的价格
	 *          开仓价格是指策略对某个合约建立仓位时的成交价格
	 *          可用于计算浮动盈亏、设置止盈止损价位等
	 */
	EXPORT_FLAG	double		cta_get_last_enterprice(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定合约的最近开仓标签
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近开仓标签，如果没有相关持仓则返回空字符串
	 * @details 获取指定合约的最近一笔开仓的标签
	 *          开仓标签是在开仓时自定义的标识，用于标记交易的来源或目的
	 *          可用于记录开仓原因、策略信号类型等信息，便于后续分析
	 */
	EXPORT_FLAG	WtString	cta_get_last_entertag(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 将文本写入策略日志
	 * @param cHandle 策略上下文句柄
	 * @param level 日志级别，数值越大级别越高
	 * @param message 日志消息内容
	 * @details 将指定级别的文本信息写入到策略的日志中
	 *          日志系统会根据级别进行过滤和管理
	 *          在策略开发和调试中，日志记录是非常重要的工具
	 */
	EXPORT_FLAG	void		cta_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	/**
	 * @brief 保存用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param val 数据值
	 * @details 将键值对保存到策略的持久化存储中
	 *          用户数据存储可以在策略实例重启或系统重启后仍然可用
	 *          常用于保存策略状态、统计信息、历史交易记录等
	 */
	EXPORT_FLAG	void		cta_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	/**
	 * @brief 加载用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param defVal 默认值，当指定键不存在时返回此值
	 * @return 返回保存的数据值，如果不存在则返回默认值
	 * @details 从策略的持久化存储中加载指定键的数据
	 *          与cta_save_userdata配合使用，实现用户数据的读写
	 *          可以用于恢复策略状态、读取历史统计信息等
	 */
	EXPORT_FLAG	WtString	cta_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	/**
	 * @brief 订阅合约的Tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @details 订阅指定合约的Tick行情数据
	 *          订阅后，可以通过策略的on_tick回调函数接收该合约的Tick数据
	 *          Tick数据包含合约最新的成交价、买卖目标价、成交量等信息
	 */
	EXPORT_FLAG	void		cta_sub_ticks(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 订阅合约的K线事件
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @details 订阅指定合约的指定周期K线事件
	 *          订阅后，当新的K线生成时，策略的on_bar回调函数会被触发
	 *          K线事件在策略开发中非常重要，可用于指标计算、交易信号生成等
	 */
	EXPORT_FLAG	void		cta_sub_bar_events(CtxHandler cHandle, const char* stdCode, const char* period);

	/**
	 * @brief 设置图表K线
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @details 设置策略图表中显示的K线合约和周期
	 *          这个函数用于策略开发环境中，允许在UI中显示指定合约的K线图表
	 *          帮助策略开发者可视化地分析市场和策略行为
	 */
	EXPORT_FLAG void		cta_set_chart_kline(CtxHandler cHandle, const char* stdCode, const char* period);

	/**
	 * @brief 添加图表标记
	 * @param cHandle 策略上下文句柄
	 * @param price 标记价格位置
	 * @param icon 图标名称
	 * @param tag 标记标签文本
	 * @details 在策略图表上添加标记点，用于标记重要的交易信号或事件
	 *          标记会显示在指定的价格位置，并使用指定的图标和标签
	 *          在策略开发和调试中提供可视化的信号标记，帮助理解策略行为
	 */
	EXPORT_FLAG void		cta_add_chart_mark(CtxHandler cHandle, double price, const char* icon, const char* tag);

	/**
	 * @brief 注册图表指标
	 * @param cHandle 策略上下文句柄
	 * @param idxName 指标名称
	 * @param indexType 指标类型：0-主图指标，1-副图指标
	 * @details 在策略图表上注册一个指标，指标可以显示在主图或副图上
	 *          主图指标与K线在同一个图表区域，副图指标则显示在单独的区域
	 *          注册成功后，还需要使用cta_register_index_line添加指标线，并使用cta_set_index_value设置指标值
	 */
	EXPORT_FLAG void		cta_register_index(CtxHandler cHandle, const char* idxName, WtUInt32 indexType);

	/**
	 * @brief 注册指标线
	 * @param cHandle 策略上下文句柄
	 * @param idxName 指标名称，与cta_register_index中的名称一致
	 * @param lineName 线条名称，在同一指标中唯一
	 * @param lineType 线性类型，0-曲线
	 * @return 注册成功返回true，失败返回false
	 * @details 在已注册的指标上添加一条线，每个指标可以包含多条线
	 *          注册指标线后，需要使用cta_set_index_value函数设置线的具体数值
	 *          不同的线可以使用不同的颜色来表示，便于在图表上区分
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
	/**
	 * @brief 创建选股策略上下文
	 * @param name 策略名称
	 * @param date 日期，格式YYYYMMDD
	 * @param time 时间，格式HHMMSS
	 * @param period 周期，如d1/w1/m1等
	 * @param trdtpl 交易模板，默认为"CHINA"
	 * @param session 交易时段，默认为"TRADING"
	 * @param slippage 滑点设置，默认为0
	 * @return 返回策略上下文句柄
	 * @details 创建一个选股策略的上下文环境
	 *          选股策略主要用于管理股票池，可以根据不同标准选择股票
	 *          策略初始化时可以指定日期、时间和周期，对应不同的选股频率
	 */
	EXPORT_FLAG	CtxHandler	create_sel_context(const char* name, uint32_t date, uint32_t time, const char* period, const char* trdtpl = "CHINA", const char* session = "TRADING", int32_t slippage = 0);

	/**
	 * @brief 获取选股策略的持仓量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param bOnlyValid 是否只计算有效仓位，不包括过渡仓位
	 * @param openTag 开仓标签，如果不为空，则只取指定标签的持仓
	 * @return 返回持仓量，多头仓位返回正数，空头仓位返回负数
	 * @details 获取选股策略指定合约的持仓数量
	 *          有效仓位是指已经确认开仓成功的持仓，不包括过渡仓位
	 *          选股策略中通常只有多头仓位，私募股票池将应用于实际交易策略
	 */
	EXPORT_FLAG	double		sel_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid, const char* openTag);

	/**
	 * @brief 设置选股策略的目标仓位
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param qty 目标仓位数量，正数表示多头仓位，负数表示空头仓位
	 * @param uesrTag 用户自定义标签
	 * @details 设置选股策略指定合约的目标仓位
	 *          框架会根据当前仓位和目标仓位的差异自动进行调整
	 *          在选股策略中主要用于发送选股结果到股票池
	 *          标签可用于标记不同选股来源或策略
	 */
	EXPORT_FLAG	void		sel_set_position(CtxHandler cHandle, const char* stdCode, double qty, const char* uesrTag);

	/**
	 * @brief 获取指定合约的最新价格
	 * @param stdCode 标准合约代码
	 * @return 返回合约的最新价格，如果没有行情数据则返回0
	 * @details 获取选股策略中指定合约的最新市场价格
	 *          在选股策略中，通常用于获取股票的当前价格信息
	 *          可以在选股算法中用于计算价格相关指标或信号
	 */
	EXPORT_FLAG	double 		sel_get_price(const char* stdCode);

	/**
	 * @brief 获取选股策略当前日期
	 * @return 返回当前日期，格式YYYYMMDD
	 * @details 获取选股策略的当前日期
	 *          在回测模式下，此日期为回测时间线的当前日期
	 *          在实盘运行时，该日期为实际交易日期
	 *          可用于选股策略中的日期判断和周期计算
	 */
	EXPORT_FLAG	WtUInt32 	sel_get_date();

	/**
	 * @brief 获取选股策略当前时间
	 * @return 返回当前时间，格式HHMMSS
	 * @details 获取选股策略的当前时间
	 *          在回测模式下，此时间为回测时间线的当前时间
	 *          在实盘运行时，该时间为实际系统时间
	 *          可用于选股策略中的时间判断和执行时间控制
	 */
	EXPORT_FLAG	WtUInt32 	sel_get_time();

	/**
	 * @brief 获取合约的K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @param barCnt 请求的K线数量
	 * @param cb 获取K线数据的回调函数
	 * @return 返回实际获取到的K线数量
	 * @details 获取选股策略中指定合约的历史K线数据
	 *          数据通过回调函数返回，每一条K线包含开高低收量等信息
	 *          在选股策略中经常用于获取历史数据进行技术指标计算
	 */
	EXPORT_FLAG	WtUInt32	sel_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb);

	/**
	 * @brief 获取合约的tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param tickCnt 请求的tick数量
	 * @param cb 获取tick数据的回调函数
	 * @return 返回实际获取到的tick数量
	 * @details 获取选股策略中指定合约的历史tick数据
	 *          tick数据包含成交价、买卖价格、买卖量等市场微观信息
	 *          选股策略中很少使用tick数据，因为选股通常在更长的周期进行
	 */
	EXPORT_FLAG	WtUInt32	sel_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	/**
	 * @brief 获取选股策略的所有持仓信息
	 * @param cHandle 策略上下文句柄
	 * @param cb 持仓信息回调函数
	 * @details 获取选股策略中所有合约的持仓信息
	 *          每个持仓都通过回调函数返回，包含合约代码、持仓数量、持仓方向等信息
	 *          可用于获取当前选股策略的所有股票池成分，进行批量操作或统计分析
	 */
	EXPORT_FLAG	void		sel_get_all_position(CtxHandler cHandle, FuncGetPositionCallback cb);

	/**
	 * @brief 写入选股策略日志
	 * @param cHandle 策略上下文句柄
	 * @param level 日志级别，数字越大级别越高
	 * @param message 日志内容
	 * @details 在选股策略中写入日志信息
	 *          框架会自动将日志写入到日志文件中，方便调试和跟踪策略运行
	 *          可用于记录选股策略的主要决策过程、错误信息或运行状态
	 */
	EXPORT_FLAG	void		sel_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	/**
	 * @brief 保存选股策略用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param val 数据值（字符串格式）
	 * @details 将选股策略中的自定义数据保存到存储中
	 *          数据以键值对的形式存储，可以在下次运行时通过sel_load_userdata函数加载
	 *          常用于保存选股策略的中间结果、算法参数或运行状态等信息
	 */
	EXPORT_FLAG	void		sel_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	/**
	 * @brief 加载选股策略的用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param defVal 默认值，当键不存在时返回此值
	 * @return 返回加载的数据字符串，如果键不存在则返回默认值
	 * @details 从存储中加载选股策略的自定义数据
	 *          与sel_save_userdata函数配合使用，用于恢复策略运行状态
	 *          常用于在策略启动时加载先前保存的选股结果、算法参数或状态信息
	 */
	EXPORT_FLAG	WtString	sel_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);

	/**
	 * @brief 在选股策略中订阅合约的tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @details 订阅指定合约的tick行情数据
	 *          订阅后，每当新的tick数据到达时，策略的on_tick回调函数将被触发
	 *          虽然选股策略通常在日线级别工作，但有时也需要实时行情数据进行特殊策略判断
	 */
	EXPORT_FLAG	void		sel_sub_ticks(CtxHandler cHandle, const char* stdCode);

	//By Wesley @ 2023.05.17
	//扩展SEL的接口，主要是和CTA接口做一个同步
	/**
	 * @brief 获取选股策略中指定合约的持仓盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回持仓的浮动盈亏，正数表示盈利，负数表示亏损
	 * @details 获取选股策略中指定合约当前的浮动盈亏
	 *          盈亏基于当前市场价格和开仓均价计算
	 *          该函数是为了与CTA接口保持一致而扩展的SEL接口
	 * @author Wesley
	 * @date 2023.05.17
	 */
	EXPORT_FLAG	double		sel_get_position_profit(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取选股策略中指定合约的开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param openTag 开仓标签，用于区分不同的开仓来源
	 * @return 返回开仓时间，格式为时间戳
	 * @details 获取选股策略中指定合约和开仓标签的开仓时间
	 *          这个时间可以用于计算持仓时间或判断仓位的年龄
	 *          在选股策略中可用于实现基于持仓时间的选股轮动策略
	 */
	EXPORT_FLAG	WtUInt64	sel_get_detail_entertime(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取选股策略中指定合约的开仓成本
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param openTag 开仓标签，用于区分不同的开仓来源
	 * @return 返回开仓成本，包含开仓价格和手续费
	 * @details 获取选股策略中指定合约和开仓标签的开仓成本
	 *          这个成本可以用于交易节奏的控制或计算策略的收益率
	 *          在选股策略中可以帮助比较不同标的股票购买成本
	 */
	EXPORT_FLAG	double		sel_get_detail_cost(CtxHandler cHandle, const char* stdCode, const char* openTag);

	/**
	 * @brief 获取选股策略中指定合约的盈亏细节
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param openTag 开仓标签，用于区分不同的开仓来源
	 * @param flag 盈亏标记，0-浮动盈亏，1-平仓盈亏
	 * @return 返回策略盈亏值，正数表示盈利，负数表示亏损
	 * @details 获取选股策略中指定合约和开仓标签的盈亏细节
	 *          可以获取浮动盈亏或平仓盈亏，取决于参数flag
	 *          在选股策略中可用于记录和跟踪各个股票的交易效果
	 */
	EXPORT_FLAG	double		sel_get_detail_profit(CtxHandler cHandle, const char* stdCode, const char* openTag, int flag);

	/**
	 * @brief 获取选股策略中指定合约的持仓均价
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回指定合约的持仓均价，如果没有持仓则返回0
	 * @details 获取选股策略中指定合约的持仓均价
	 *          对于多次开仓的情况，返回的是加权平均价
	 *          在选股策略中可用于计算浮动盈亏或分析交易成本
	 */
	EXPORT_FLAG	double		sel_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取合约的日线价格数据
	 * @param stdCode 标准合约代码
	 * @param flag 价格标记，0-开盘价，1-最高价，2-最低价，3-收盘价，4-最新成交价
	 * @return 返回指定价格类型的数值，如果没有数据则返回0
	 * @details 在选股策略中获取合约的日线级别价格数据
	 *          可以根据不同的flag参数获取开盘价、最高价、最低价等
	 *          在选股策略中经常用于计算日线指标或做日线价格策略
	 */
	EXPORT_FLAG	double 		sel_get_day_price(const char* stdCode, int flag);

	/**
	 * @brief 获取选股策略的资金数据
	 * @param cHandle 策略上下文句柄
	 * @param flag 资金数据标记，0-动态权益，1-静态权益，2-可用资金
	 * @return 返回指定类型的资金数据
	 * @details 获取选股策略中的资金相关数据
	 *          动态权益包含浮动盈亏，静态权益不包含浮动盈亏
	 *          可用资金表示策略账户中当前可使用的资金量
	 *          在选股策略中可用于估算当前可购买股票的数量或监控资金情况
	 */
	EXPORT_FLAG	double		sel_get_fund_data(CtxHandler cHandle, int flag);

	/**
	 * @brief 获取选股策略当前交易日期
	 * @return 返回当前交易日期，格式YYYYMMDD
	 * @details 获取系统当前交易日期，这个日期与sel_get_date的区别是：
	 *          tdate是交易日期，表示市场当前交易日，非交易日不变
	 *          date是自然日期，表示实际日期，每天都会变化
	 *          在选股策略中通常用于判断当前是否是交易日或运行日线周期策略
	 */
	EXPORT_FLAG	WtUInt32 	sel_get_tdate();

	/**
	 * @brief 获取选股策略中指定合约的首次开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回首次开仓时间，格式为时间戳，如果没有持仓则返回0
	 * @details 获取选股策略中指定合约的最早一笔开仓时间
	 *          可用于判断策略持仓时间，或实现基于持仓时间的选股逐出策略
	 *          在选股轮动策略中，可以用来实现基于持仓时间的股票替换机制
	 */
	EXPORT_FLAG	WtUInt64	sel_get_first_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取选股策略中指定合约的最近开仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近开仓时间，格式为时间戳，如果没有持仓则返回0
	 * @details 获取选股策略中指定合约的最近一笔开仓时间
	 *          与sel_get_first_entertime不同，当有多笔开仓时，返回最近的一次开仓时间
	 *          可用于识别近期调仓操作，或在平滑加仓策略中控制加仓频率
	 */
	EXPORT_FLAG	WtUInt64	sel_get_last_entertime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取选股策略中指定合约的最近平仓时间
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近平仓时间，格式为时间戳，如果没有平仓记录则返回0
	 * @details 获取选股策略中指定合约的最近一笔平仓时间
	 *          对于已平仓的合约，返回最后一次平仓的时间
	 *          可用于选股策略中实现交易冷静期控制，避免频繁交易同一股票
	 */
	EXPORT_FLAG	WtUInt64	sel_get_last_exittime(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取选股策略中指定合约的最近开仓价格
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近一笔开仓的价格，如果没有开仓记录则返回0
	 * @details 获取选股策略中指定合约的最近一笔开仓的价格
	 *          当有多笔开仓记录时，返回最近一次的开仓价格
	 *          可用于选股策略中比较当前价格与开仓价格的差异，评估投资效果
	 */
	EXPORT_FLAG	double		sel_get_last_enterprice(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取选股策略中指定合约的最近开仓标签
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回最近一笔开仓的标签字符串，如果没有开仓记录则返回空字符串
	 * @details 获取选股策略中指定合约的最近一笔开仓的标签
	 *          标签通常是在开仓时传入的自定义字符串，用于标记交易来源或类型
	 *          在选股策略中可用于识别不同类型的股票或交易来源，进行分类统计或分析
	 */
	EXPORT_FLAG	WtString	sel_get_last_entertag(CtxHandler cHandle, const char* stdCode);
#pragma endregion "SEL接口"

	//////////////////////////////////////////////////////////////////////////
	//HFT策略接口
#pragma  region "HFT接口"
	/**
	 * @brief 创建HFT（高频交易）策略上下文
	 * @param name 策略名称
	 * @param trader 交易通道名称
	 * @param agent 是否启用代理模式
	 * @param slippage 滑点设置，默认为0
	 * @return 返回策略上下文句柄
	 * @details 创建一个高频交易策略的上下文环境
	 *          高频交易策略通常对市场微观结构敏感，需要更高的实时性
	 *          可以指定交易通道，并配置是否需要代理模式和滑点
	 */
	EXPORT_FLAG	CtxHandler	create_hft_context(const char* name, const char* trader, bool agent, int32_t slippage = 0);

	/**
	 * @brief 获取高频策略中指定合约的持仓量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param bOnlyValid 是否只计算有效仓位，不包括过渡仓位
	 * @return 返回持仓量，正数表示多头仓位，负数表示空头仓位
	 * @details 获取高频策略中指定合约的当前持仓数量
	 *          有效仓位是指已经确认开仓成功的持仓，不包括过渡仓位
	 *          在高频策略中，由于订单可能存在延迟，需要特别注意识别过渡仓位和有效仓位
	 */
	EXPORT_FLAG	double		hft_get_position(CtxHandler cHandle, const char* stdCode, bool bOnlyValid);

	/**
	 * @brief 获取高频策略中指定合约的持仓盈亏
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回指定合约的浮动盈亏，正数表示盈利，负数表示亏损
	 * @details 获取高频策略中指定合约的当前浮动盈亏
	 *          盈亏是根据当前市场价格和开仓均价计算得出
	 *          在高频策略中，此函数可用于实时跟踪交易效果和风险控制
	 */
	EXPORT_FLAG	double		hft_get_position_profit(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取高频策略中指定合约的持仓均价
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回指定合约的持仓均价，如果没有持仓则返回0
	 * @details 获取高频策略中指定合约的持仓均价
	 *          对于多笔开仓的情况，均价是根据交易数量加权平均计算的
	 *          在高频策略中，持仓均价是计算盈亏和交易效果的重要依据
	 */
	EXPORT_FLAG	double		hft_get_position_avgpx(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取高频策略中指定合约的未完成订单数量
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @return 返回未完成的订单数量，正数表示多头未完成量，负数表示空头未完成量
	 * @details 获取高频策略中指定合约当前未完成的订单数量
	 *          这个数量包括未成交、部分成交的订单中剩余的待成交量
	 *          在高频策略中，跟踪未完成订单对于计算实际持仓和控制订单风险非常重要
	 */
	EXPORT_FLAG	double		hft_get_undone(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 获取指定合约的最新价格
	 * @param stdCode 标准合约代码
	 * @return 返回合约的最新价格，如果没有行情数据则返回0
	 * @details 获取高频策略中指定合约的当前市场最新价格
	 *          在高频策略中，这个价格经常用于实时分析或计算指标
	 *          高频策略需要特别注意对这个价格的延迟性和准确性评估
	 */
	EXPORT_FLAG	double 		hft_get_price(const char* stdCode);

	/**
	 * @brief 获取高频策略当前日期
	 * @return 返回当前日期，格式YYYYMMDD
	 * @details 获取高频策略的当前日期
	 *          在回测模式下，此日期为回测时间线的当前日期
	 *          在实盘交易中，返回系统实际日期
	 *          高频策略可用该日期进行日内策略切换或交易时段控制
	 */
	EXPORT_FLAG	WtUInt32 	hft_get_date();

	/**
	 * @brief 获取高频策略当前时间
	 * @return 返回当前时间，格式HHMMSS
	 * @details 获取高频策略的当前时间
	 *          在回测模式下，此时间为回测时间线的当前时间
	 *          在实盘交易中，返回系统实际时间
	 *          高频策略需要精确把握时间，进行交易时段判断和定时交易
	 */
	EXPORT_FLAG	WtUInt32 	hft_get_time();

	/**
	 * @brief 获取高频策略当前秒数
	 * @return 返回当前时间的秒数计数，从0点开始的秒数
	 * @details 获取高频策略的当前秒级时间
	 *          返回从当天零点开始的秒数，范围为0-86399（86400-1）
	 *          在高频策略中，秒级时间对于逻辑判断和精确交易非常重要
	 *          可用于实现微秒级策略或测量订单延迟
	 */
	EXPORT_FLAG	WtUInt32 	hft_get_secs();

	/**
	 * @brief 获取高频策略中合约的K线数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @param barCnt 请求的K线数量
	 * @param cb 获取K线数据的回调函数
	 * @return 返回实际获取到的K线数量
	 * @details 获取高频策略中指定合约的历史K线数据
	 *          数据通过回调函数返回，每一条K线包含开高低收量等信息
	 *          高频策略中可用于获取分钟级别的K线进行技术指标分析
	 */
	EXPORT_FLAG	WtUInt32	hft_get_bars(CtxHandler cHandle, const char* stdCode, const char* period, WtUInt32 barCnt, FuncGetBarsCallback cb);

	/**
	 * @brief 获取高频策略中合约的tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param tickCnt 请求的tick数量
	 * @param cb 获取tick数据的回调函数
	 * @return 返回实际获取到的tick数量
	 * @details 获取高频策略中指定合约的历史tick数据
	 *          tick数据是市场的原子级信息，包含成交价、买卖相关信息
	 *          高频策略通常依赖tick数据进行微观的市场分析和交易决策
	 */
	EXPORT_FLAG	WtUInt32	hft_get_ticks(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTicksCallback cb);

	/**
	 * @brief 获取高频策略中合约的委托队列数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param tickCnt 请求的数据条数
	 * @param cb 获取委托队列数据的回调函数
	 * @return 返回实际获取到的委托队列数据数量
	 * @details 获取高频策略中指定合约的历史委托队列数据
	 *          委托队列数据是行情的一种详细数据，反映当前市场委托价格排列情况
	 *          高频策略可以使用该数据分析市场深度和买卖力量对比，进行更精准的交易决策
	 */
	EXPORT_FLAG	WtUInt32	hft_get_ordque(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetOrdQueCallback cb);

	/**
	 * @brief 获取高频策略中合约的委托明细数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param tickCnt 请求的数据条数
	 * @param cb 获取委托明细数据的回调函数
	 * @return 返回实际获取到的委托明细数据数量
	 * @details 获取高频策略中指定合约的历史委托明细数据
	 *          委托明细数据包含具体委托时间、价格、方向等详细信息
	 *          高频策略可以分析该数据以识别市场中的大单委托和交易意图
	 */
	EXPORT_FLAG	WtUInt32	hft_get_orddtl(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetOrdDtlCallback cb);

	/**
	 * @brief 获取高频策略中合约的逐笔成交数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param tickCnt 请求的数据条数
	 * @param cb 获取逐笔成交数据的回调函数
	 * @return 返回实际获取到的逐笔成交数据数量
	 * @details 获取高频策略中指定合约的历史逐笔成交数据
	 *          逐笔成交数据包含每笔具体的成交价格、数量、时间等信息
	 *          高频策略可以分析这些数据来检测市场中的流动性和大单交易模式
	 */
	EXPORT_FLAG	WtUInt32	hft_get_trans(CtxHandler cHandle, const char* stdCode, WtUInt32 tickCnt, FuncGetTransCallback cb);

	/**
	 * @brief 在高频策略中输出日志
	 * @param cHandle 策略上下文句柄
	 * @param level 日志级别，数字越大级别越高
	 * @param message 日志内容
	 * @details 在高频策略中输出指定级别的日志信息
	 *          日志级别决定了消息的重要性，框架会根据级别决定日志的处理方式
	 *          高频策略中的日志记录对于跟踪实时交易状态和调试问题非常重要
	 */
	EXPORT_FLAG	void		hft_log_text(CtxHandler cHandle, WtUInt32 level, const char* message);

	/**
	 * @brief 在高频策略中订阅合约的tick数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @details 在高频策略中订阅指定合约的tick数据
	 *          订阅后，每当新的tick数据到达时，策略的on_tick回调函数将被触发
	 *          这是高频策略中最基本的数据订阅方式，用于获取实时市场数据
	 */
	EXPORT_FLAG	void		hft_sub_ticks(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 在高频策略中订阅合约的委托队列数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @details 在高频策略中订阅指定合约的委托队列数据
	 *          订阅后，每当委托队列变化时，策略的on_ordque_data回调函数将被触发
	 *          委托队列数据可用于分析市场深度和买卖双方的力量对比
	 */
	EXPORT_FLAG	void		hft_sub_order_queue(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 在高频策略中订阅合约的委托明细数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @details 在高频策略中订阅指定合约的委托明细数据
	 *          订阅后，每当有新的委托明细数据时，策略的on_orddtl_data回调函数将被触发
	 *          委托明细数据包含具体委托信息，可用于分析大单行为和市场微观结构
	 */
	EXPORT_FLAG	void		hft_sub_order_detail(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 在高频策略中订阅合约的逐笔成交数据
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @details 在高频策略中订阅指定合约的逐笔成交数据
	 *          订阅后，每当有新的成交发生时，策略的on_trans_data回调函数将被触发
	 *          逐笔成交数据包含每一笔成交的具体信息，可用于监控市场流动性和交易模式
	 */
	EXPORT_FLAG	void		hft_sub_transaction(CtxHandler cHandle, const char* stdCode);

	/**
	 * @brief 在高频策略中撤销订单
	 * @param cHandle 策略上下文句柄
	 * @param localid 订单的本地ID
	 * @return 撤单请求发送成功返回true，失败返回false
	 * @details 在高频策略中通过本地订单ID撤销订单
	 *          本地ID是订单发送时由系统生成并返回的唯一标识
	 *          高频策略中及时的撤单对于管理交易风险非常重要
	 */
	EXPORT_FLAG	bool		hft_cancel(CtxHandler cHandle, WtUInt32 localid);

	/**
	 * @brief 在高频策略中批量撤销订单
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码，为空表示撤销所有合约的订单
	 * @param isBuy 是否只撤销买单，true表示只撤销买单，false表示只撤销卖单
	 * @return 返回撤销成功的订单ID列表，以逗号分隔
	 * @details 在高频策略中批量撤销符合条件的所有未完成订单
	 *          可以通过合约代码和订单方向进行过滤
	 *          在高频策略中常用于稳定低延迟行情时期或策略退出时清理所有未完成订单
	 */
	EXPORT_FLAG	WtString	hft_cancel_all(CtxHandler cHandle, const char* stdCode, bool isBuy);

	/**
	 * @brief 在高频策略中发送买入委托
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param price 买入价格，如果是市价单则为0
	 * @param qty 买入数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param flag 委托标志，0-限价单，1-市价单，其他可能是特殊订单类型
	 * @return 返回订单的本地ID，如果失败则返回空字符串
	 * @details 在高频策略中发送买入委托单
	 *          订单会立即发送到交易所，并返回本地生成的订单ID
	 *          高频策略需要保存该ID以进行后续的订单状态跟踪或撤单操作
	 */
	EXPORT_FLAG	WtString	hft_buy(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag);

	/**
	 * @brief 在高频策略中发送卖出委托
	 * @param cHandle 策略上下文句柄
	 * @param stdCode 标准合约代码
	 * @param price 卖出价格，如果是市价单则为0
	 * @param qty 卖出数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param flag 委托标志，0-限价单，1-市价单，其他可能是特殊订单类型
	 * @return 返回订单的本地ID，如果失败则返回空字符串
	 * @details 在高频策略中发送卖出委托单
	 *          订单会立即发送到交易所，并返回本地生成的订单ID
	 *          高频策略需要保存该ID以进行后续的订单状态跟踪或撤单操作
	 */
	EXPORT_FLAG	WtString	hft_sell(CtxHandler cHandle, const char* stdCode, double price, double qty, const char* userTag, int flag);

	/**
	 * @brief 保存高频策略的用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param val 数据值（字符串格式）
	 * @details 将高频策略中的自定义数据保存到存储中
	 *          数据以键值对的形式存储，可以在下次运行时通过hft_load_userdata函数加载
	 *          在高频策略中可以用于保存交易状态、策略参数或其他需要持久化的数据
	 */
	EXPORT_FLAG	void		hft_save_userdata(CtxHandler cHandle, const char* key, const char* val);

	/**
	 * @brief 加载高频策略的用户自定义数据
	 * @param cHandle 策略上下文句柄
	 * @param key 数据键名
	 * @param defVal 默认值，当键不存在时返回此值
	 * @return 返回加载的数据字符串，如果键不存在则返回默认值
	 * @details 从存储中加载高频策略的自定义数据
	 *          与hft_save_userdata函数配合使用，用于恢复之前保存的数据
	 *          在高频策略中可用于加载策略状态、参数设置或更复杂的数据结构（以JSON字符串形式）
	 */
	EXPORT_FLAG	WtString	hft_load_userdata(CtxHandler cHandle, const char* key, const char* defVal);
#pragma endregion "HFT接口"

#pragma region "扩展Parser接口"
	/**
	 * @brief 将行情数据推送到解析器
	 * @param id 解析器ID
	 * @param curTick 当前Tick数据结构
	 * @param uProcFlag 处理标志，用于控制数据处理方式
	 * @details 将行情数据推送到指定的解析器中进行处理
	 *          这是扩展Parser接口的一部分，用于外部数据源将数据推送到框架中
	 *          解析器会根据收到的数据进行处理并分发给相应的策略
	 */
	EXPORT_FLAG	void		parser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag);
#pragma endregion "扩展Parser接口"

#ifdef __cplusplus
}
#endif