/*!
 * \file WtBtRunner.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 回测引擎类定义文件
 * \details 该文件定义了回测引擎WtBtRunner类，提供完整的CTA、SEL及HFT策略回测框架支持
 *          实现了历史数据回放、回调事件分发、策略环境模拟等核心回测功能
 *          是WonderTrader回测系统与外部组件交互的核心接口
 */
#pragma once
#include "PorterDefs.h"
#include "../WtBtCore/EventNotifier.h"
#include "../WtBtCore/HisDataReplayer.h"
#include "../Includes/WTSMarcos.h"


NS_WTP_BEGIN
class WTSTickData;
struct WTSBarStruct;
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

typedef enum tagEngineType
{
	ET_CTA = 999,	//CTA引擎	
	ET_HFT,			//高频引擎
	ET_SEL			//选股引擎
} EngineType;

class SelMocker;
class CtaMocker;
class HftMocker;
class ExecMocker;

/**
 * @brief 回测引擎类
 * @details 提供了完整的策略回测环境，继承IBtDataLoader接口，实现历史数据加载
 *          支持CTA策略、选股策略和高频策略的回测
 *          实现了事件传递、回调调度和数据回放的核心功能
 *          是WonderTrader回测系统与外部组件交互的核心类
 */
class WtBtRunner : public IBtDataLoader
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化回测引擎对象，初始化成员变量并设置默认状态
	 */
	WtBtRunner();

	/**
	 * @brief 析构函数
	 * @details 清理回测引擎资源，释放所有策略模拟器和用户数据
	 */
	~WtBtRunner();


	//////////////////////////////////////////////////////////////////////////
	//IBtDataLoader
	/**
	 * @brief 加载已处理的历史K线数据
	 * @param obj 用户指针，可传递给回调函数
	 * @param stdCode 标准化合约代码
	 * @param period K线周期类型
	 * @param cb 读取回调函数
	 * @return 是否成功加载
	 * @details 重写自 IBtDataLoader 接口，用于加载已经过复权处理的历史最终K线数据
	 */
	virtual bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override;

	/**
	 * @brief 加载原始历史K线数据
	 * @param obj 用户指针，可传递给回调函数
	 * @param stdCode 标准化合约代码
	 * @param period K线周期类型
	 * @param cb 读取回调函数
	 * @return 是否成功加载
	 * @details 重写自 IBtDataLoader 接口，用于加载未经过复权处理的原始K线数据
	 */
	virtual bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override;

	/**
	 * @brief 加载全部复权因子
	 * @param obj 用户指针，可传递给回调函数
	 * @param cb 复权因子读取回调函数
	 * @return 是否成功加载
	 * @details 重写自 IBtDataLoader 接口，用于加载所有合约的复权因子数据
	 */
	virtual bool loadAllAdjFactors(void* obj, FuncReadFactors cb) override;

	/**
	 * @brief 加载指定合约的复权因子
	 * @param obj 用户指针，可传递给回调函数
	 * @param stdCode 标准化合约代码
	 * @param cb 复权因子读取回调函数
	 * @return 是否成功加载
	 * @details 重写自 IBtDataLoader 接口，用于加载指定合约的复权因子数据
	 */
	virtual bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override;

	/**
	 * @brief 加载原始Tick数据
	 * @param obj 用户指针，可传递给回调函数
	 * @param stdCode 标准化合约代码
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param cb Tick数据读取回调函数
	 * @return 是否成功加载
	 * @details 重写自 IBtDataLoader 接口，用于加载指定日期的原始Tick数据
	 */
	virtual bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override;

	/**
	 * @brief 检查是否自动转存数据
	 * @return 是否启用数据自动转存
	 * @details 重写自 IBtDataLoader 接口，该方法表示当数据加载器加载数据时是否自动将数据转存到本地
	 */
	virtual bool isAutoTrans() override
	{
		return _loader_auto_trans;
	}

	/**
	 * @brief 输入原始K线数据
	 * @param bars K线数据数组
	 * @param count 数据数量
	 * @details 用于将原始K线数据直接输入到回测引擎中，通常配合外部数据源使用
	 */
	void feedRawBars(WTSBarStruct* bars, uint32_t count);
	/**
	 * @brief 输入原始Tick数据
	 * @param ticks Tick数据数组
	 * @param count 数据数量
	 * @details 用于将原始Tick数据直接输入到回测引擎中，通常用于实时数据或高频策略测试
	 */
	void feedRawTicks(WTSTickStruct* ticks, uint32_t count);
	/**
	 * @brief 输入复权因子数据
	 * @param stdCode 标准化合约代码
	 * @param dates 日期数组，每个复权因子对应的日期
	 * @param factors 复权因子数组
	 * @param count 数据数量
	 * @details 用于将合约的复权因子数据直接输入到回测引擎中，通常用于股票等需要复权的品种
	 */
	void feedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count);

public:
	/**
	 * @brief 注册CTA策略回调函数
	 * @param cbInit 策略初始化回调
	 * @param cbTick 实时行情回调
	 * @param cbCalc 定时计算回调
	 * @param cbBar K线更新回调
	 * @param cbSessEvt 交易日事件回调
	 * @param cbCalcDone 计算完成回调，可选
	 * @param cbCondTrigger 条件触发回调，可选
	 * @details 用于外部系统注册CTA策略的各类事件回调函数，这些函数将在相应事件发生时被回测引擎调用
	 */
	void	registerCtaCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
		FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone = NULL, FuncStraCondTriggerCallback cbCondTrigger = NULL);
	/**
	 * @brief 注册选股策略回调函数
	 * @param cbInit 策略初始化回调
	 * @param cbTick 实时行情回调
	 * @param cbCalc 定时计算回调
	 * @param cbBar K线更新回调
	 * @param cbSessEvt 交易日事件回调
	 * @param cbCalcDone 计算完成回调，可选
	 * @details 用于外部系统注册选股策略的各类事件回调函数，选股策略主要关注多个标的的比较和选择
	 */
	void	registerSelCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, 
		FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCalcCallback cbCalcDone = NULL);
	/**
	 * @brief 注册高频策略回调函数
	 * @param cbInit 策略初始化回调
	 * @param cbTick 实时行情回调
	 * @param cbBar K线更新回调
	 * @param cbChnl 通道就绪回调
	 * @param cbOrd 委托回报回调
	 * @param cbTrd 成交回报回调
	 * @param cbEntrust 委托返回回调
	 * @param cbOrdDtl 逃单回调
	 * @param cbOrdQue 委托队列回调
	 * @param cbTrans 逐笔成交回调
	 * @param cbSessEvt 交易日事件回调
	 * @details 用于外部系统注册高频策略的各类事件回调函数，高频策略需要更多的市场微观结构数据支持
	 */
	void registerHftCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar,
		FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
		FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt);

	/**
	 * @brief 注册全局事件回调函数
	 * @param cbEvt 事件回调函数
	 * @details 用于注册全局事件回调，如初始化事件、回测结束事件等重要的全局性事件
	 *          这些事件允许外部系统在回测的关键时刻执行相应的处理逻辑
	 */
	void registerEvtCallback(FuncEventCallback cbEvt)
	{
		_cb_evt = cbEvt;
	}

	/**
	 * @brief 注册外部数据加载器
	 * @param fnlBarLoader 处理后的历史K线加载回调
	 * @param rawBarLoader 原始K线加载回调
	 * @param fctLoader 复权因子加载回调
	 * @param tickLoader Tick数据加载回调
	 * @param bAutoTrans 是否自动转存数据，默认为true
	 * @details 注册外部数据加载器，允许回测引擎从自定义数据源加载数据
	 *          通过这种方式，可以将回测引擎与不同的数据来源集成，如数据库、网络或自定义文件格式
	 */
	void		registerExtDataLoader(FuncLoadFnlBars fnlBarLoader, FuncLoadRawBars rawBarLoader, FuncLoadAdjFactors fctLoader, FuncLoadRawTicks tickLoader, bool bAutoTrans = true)
	{
		_ext_fnl_bar_loader = fnlBarLoader;
		_ext_raw_bar_loader = rawBarLoader;
		_ext_adj_fct_loader = fctLoader;
		_ext_tick_loader = tickLoader;
		_loader_auto_trans = bAutoTrans;
	}

	/**
	 * @brief 初始化CTA策略模拟器
	 * @param name 策略名称
	 * @param slippage 回测中的滑点设置，默认为0
	 * @param hook 是否启用钩子函数，默认为false
	 * @param persistData 是否持久化数据，默认为true
	 * @param bIncremental 是否增量模式，默认为false
	 * @param isRatioSlp 滑点是否为百分比模式，默认为false
	 * @return 返回策略上下文ID
	 * @details 初始化CTA策略回测模拟器，创建并配置一个CTA策略的回测环境
	 *          实际创建的是CtaMocker对象，用于模拟策略行为和维护策略上下文
	 */
	uint32_t	initCtaMocker(const char* name, int32_t slippage = 0, bool hook = false, bool persistData = true, bool bIncremental = false, bool isRatioSlp = false);
	/**
	 * @brief 初始化高频策略模拟器
	 * @param name 策略名称
	 * @param hook 是否启用钩子函数，默认为false
	 * @return 返回策略上下文ID
	 * @details 初始化高频策略回测模拟器，创建并配置一个高频策略的回测环境
	 *          实际创建的是HftMocker对象，用于模拟高频策略的交易行为和市场微观结构事件
	 */
	uint32_t	initHftMocker(const char* name, bool hook = false);
	/**
	 * @brief 初始化选股策略模拟器
	 * @param name 策略名称
	 * @param date 回测起始日期，格式为YYYYMMDD
	 * @param time 回测起始时间，格式为HHMMSS
	 * @param period 策略周期，如"d1"表示日线
	 * @param trdtpl 交易模板，默认为"CHINA"
	 * @param session 交易时段，默认为"TRADING"
	 * @param slippage 回测中的滑点设置，默认为0
	 * @param isRatioSlp 滑点是否为百分比模式，默认为false
	 * @return 返回策略上下文ID
	 * @details 初始化选股策略回测模拟器，创建并配置一个选股策略的回测环境
	 *          实际创建的是SelMocker对象，用于模拟多标的筛选和资产配置策略
	 */
	uint32_t	initSelMocker(const char* name, uint32_t date, uint32_t time, const char* period, 
		const char* trdtpl = "CHINA", const char* session = "TRADING", int32_t slippage = 0, bool isRatioSlp = false);

	/**
	 * @brief 初始化事件通知器
	 * @param cfg 事件通知器配置
	 * @return 初始化是否成功
	 * @details 初始化回测引擎的事件通知器，用于将回测中的各类事件通知到外部系统
	 *          事件通知器可以配置不同的通知方式，如日志、数据库、回调函数等
	 */
	bool	initEvtNotifier(WTSVariant* cfg);

	/**
	 * @brief 策略上下文初始化事件
	 * @param id 策略上下文ID
	 * @param eType 引擎类型，CTA/HFT/SEL
	 * @details 当策略上下文初始化时触发此事件，将初始化事件传递给已注册的回调函数
	 *          不同类型的引擎将调用对应的回调函数处理初始化逻辑
	 */
	void	ctx_on_init(uint32_t id, EngineType eType);
	/**
	 * @brief 交易日事件
	 * @param id 策略上下文ID
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @param isBegin 是否为交易日开始事件，默认为true，false表示交易日结束
	 * @param eType 引擎类型，默认为ET_CTA
	 * @details 当交易日开始或结束时触发此事件，将交易日事件传递给已注册的回调函数
	 *          交易日事件对于策略的仓位管理和数据处理非常重要，它标志着每个交易日的开始和结束
	 */
	void	ctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin = true, EngineType eType = ET_CTA);
	/**
	 * @brief 实时行情数据事件
	 * @param id 策略上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick行情数据
	 * @param eType 引擎类型
	 * @details 当收到新的Tick行情数据时触发此事件，将行情数据传递给已注册的回调函数
	 *          Tick数据包含了合约的实时价格、成交量等实时信息，是高频和CTA策略的重要数据来源
	 */
	void	ctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType);
	/**
	 * @brief 策略计算事件
	 * @param id 策略上下文ID
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS
	 * @param eType 引擎类型
	 * @details 当回测引擎在特定时间点需要进行策略计算时触发此事件
	 *          将计算事件传递给已注册的回调函数，用于重新估算指标或进行定时策略逻辑
	 */
	void	ctx_on_calc(uint32_t id, uint32_t uDate, uint32_t uTime, EngineType eType);
	/**
	 * @brief 策略计算完成事件
	 * @param id 策略上下文ID
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS
	 * @param eType 引擎类型
	 * @details 当策略在特定时间点完成计算流程后触发此事件
	 *          将计算完成事件传递给已注册的回调函数，用于定时任务或最终决策逻辑
	 *          该事件比ctx_on_calc晚，可以确保所有的计算指标都已经更新完成
	 */
	void	ctx_on_calc_done(uint32_t id, uint32_t uDate, uint32_t uTime, EngineType eType);
	/**
	 * @brief K线周期数据事件
	 * @param id 策略上下文ID
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"、"d1"等
	 * @param newBar 新的K线数据
	 * @param eType 引擎类型
	 * @details 当新的K线数据形成时触发此事件，将K线数据传递给已注册的回调函数
	 *          K线数据包含了价格的开高低收和成交量等信息，是策略分析和交易决策的重要依据
	 *          不同周期的K线数据将通过period参数区分
	 */
	void	ctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType);
	/**
	 * @brief 条件触发事件
	 * @param id 策略上下文ID
	 * @param stdCode 标准化合约代码
	 * @param target 目标价格或条件值
	 * @param price 当前实际价格
	 * @param usertag 用户自定义标签，用于识别不同的触发条件
	 * @param eType 引擎类型，默认为ET_CTA
	 * @details 当设定的条件被触发时触发此事件，将触发信息传递给已注册的回调函数
	 *          条件触发事件通常用于实现止盈、止损或自定义条件的交易信号
	 *          usertag可用于区分不同类型的条件触发，如止盈、止损等
	 */
	void	ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType = ET_CTA);

	/**
	 * @brief 高频委托队列数据事件
	 * @param id 策略上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 新的委托队列数据
	 * @details 当收到新的委托队列数据时触发此事件，将数据传递给已注册的高频策略回调函数
	 *          委托队列数据包含了市场委托的深度和数量信息，是高频交易的关键微观数据
	 */
	void	hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue);
	/**
	 * @brief 高频委托明细数据事件
	 * @param id 策略上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 新的委托明细数据
	 * @details 当收到新的委托明细数据时触发此事件，将数据传递给已注册的高频策略回调函数
	 *          委托明细数据包含了市场上每笔委托的详细信息，是分析市场流动性的重要数据
	 */
	void	hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl);
	/**
	 * @brief 高频逐笔成交数据事件
	 * @param id 策略上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newTranns 新的逐笔成交数据
	 * @details 当收到新的逐笔成交数据时触发此事件，将数据传递给已注册的高频策略回调函数
	 *          逐笔成交数据包含了市场上每笔成交的详细信息，是分析市场微观结构的重要数据
	 */
	void	hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTranns);

	/**
	 * @brief 高频交易通道就绪事件
	 * @param cHandle 通道句柄
	 * @param trader 交易器标识
	 * @details 当高频交易通道就绪时触发此事件，将通道就绪信息传递给已注册的高频策略回调函数
	 *          在回测中模拟交易通道的就绪状态，可以使高频策略测试更接近实际交易环境
	 */
	void	hft_on_channel_ready(uint32_t cHandle, const char* trader);
	/**
	 * @brief 高频委托回报事件
	 * @param cHandle 通道句柄
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买单
	 * @param totalQty 总数量
	 * @param leftQty 剩余数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 * @param userTag 用户自定义标签
	 * @details 当高频策略委托状态变化时触发此事件，将委托信息传递给已注册的高频策略回调函数
	 *          在回测中模拟委托的生命周期，包括新委托、部分成交和撤单等状态变化
	 */
	void	hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag);
	/**
	 * @brief 高频成交回报事件
	 * @param cHandle 通道句柄
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买单成交
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @param userTag 用户自定义标签
	 * @details 当高频策略委托成交时触发此事件，将成交信息传递给已注册的高频策略回调函数
	 *          在回测中模拟成交过程，包括成交价格、数量等关键信息
	 */
	void	hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag);
	/**
	 * @brief 高频委托回报响应事件
	 * @param cHandle 通道句柄
	 * @param localid 本地委托ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托响应消息
	 * @param userTag 用户自定义标签
	 * @details 当高频策略发出委托请求后收到响应时触发此事件，将委托响应信息传递给已注册的高频策略回调函数
	 *          在回测中模拟交易所对委托请求的响应，包括委托是否被接受或被拒绝
	 */
	void	hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag);

	/**
	 * @brief 初始化回测引擎
	 * @param logProfile 日志配置文件或配置项，默认为空串
	 * @param isFile 是否为文件路径，默认为true。如果设置为false，则logProfile将被视为配置内容
	 * @param outDir 输出数据目录，默认为"./outputs_bt"
	 * @details 初始化回测引擎，设置日志和输出目录
	 *          这是使用回测引擎的第一步，在调用其他方法前必须先调用此方法
	 */
	void	init(const char* logProfile = "", bool isFile = true, const char* outDir = "./outputs_bt");
	/**
	 * @brief 配置回测引擎
	 * @param cfgFile 配置文件路径或配置内容
	 * @param isFile 是否为文件路径，默认为true。如果设置为false，则cfgFile将被视为配置内容
	 * @details 加载回测引擎的配置，包括策略参数、数据源设置、回测区间等
	 *          在调用init方法之后，run方法之前必须调用此方法进行配置
	 */
	void	config(const char* cfgFile, bool isFile = true);
	/**
	 * @brief 运行回测引擎
	 * @param bNeedDump 是否需要输出详细回测数据，默认为false
	 * @param bAsync 是否异步运行，默认为false
	 * @details 启动回测引擎并执行回测流程
	 *          当bNeedDump设置为true时，会输出更详细的回测数据到输出目录
	 *          当bAsync设置为true时，回测将在独立线程中异步运行，不会阻塞当前线程
	 *          异步模式下您可以调用stop方法来停止回测
	 */
	void	run(bool bNeedDump = false, bool bAsync = false);
	/**
	 * @brief 释放回测引擎资源
	 * @details 释放回测引擎占用的所有资源，包括策略模拟器、数据缓存等
	 *          在回测结束后或在程序退出前应调用此函数进行资源清理
	 */
	void	release();
	/**
	 * @brief 停止回测引擎
	 * @details 停止正在运行的回测过程
	 *          仅在异步模式下有意义，即当run方法的bAsync参数设置为true时
	 *          调用此方法后会终止异步线程中的回测过程
	 */
	void	stop();

	/**
	 * @brief 设置回测时间范围
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS的整数
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS的整数
	 * @details 设置回测的时间范围，限定回测数据的时间范围
	 *          该方法会覆盖配置文件中的时间范围设置
	 *          应在调用run方法之前调用此方法来设置自定义的时间范围
	 */
	void	set_time_range(WtUInt64 stime, WtUInt64 etime);

	/**
	 * @brief 启用或禁用Tick数据回测
	 * @param bEnabled 是否启用Tick数据，默认为true
	 * @details 设置回测引擎是否使用Tick数据进行回测
	 *          当启用Tick数据时，回测将基于Tick级别数据进行，精度更高，但效率较低
	 *          当禁用Tick数据时，回测将仅基于K线数据进行，效率更高，但精度较低
	 *          应在调用run方法之前调用此方法
	 */
	void	enable_tick(bool bEnabled = true);

	/**
	 * @brief 清理回测数据缓存
	 * @details 清理回测引擎中的数据缓存
	 *          在多次连续运行回测时，可以调用此方法清理上一次回测的数据缓存
	 *          这样可以减少内存占用并确保下一次回测使用全新的数据
	 */
	void	clear_cache();

	/**
	 * @brief 获取原始标准化合约代码
	 * @param stdCode 标准化合约代码
	 * @return 原始标准化合约代码字符串
	 * @details 从标准化合约代码获取原始合约代码
	 *          在回测系统中，合约代码可能经过加工或规范化，该方法用于获取原始的代码格式
	 *          原始合约代码通常用于访问原始数据或与外部系统交互
	 */
	const char*	get_raw_stdcode(const char* stdCode);

	/**
	 * @brief 获取CTA策略模拟器实例
	 * @return CTA策略模拟器指针
	 * @details 获取当前创建的CTA策略模拟器实例，可以用于直接操作策略模拟器
	 */
	inline CtaMocker*		cta_mocker() { return _cta_mocker; }

	/**
	 * @brief 获取选股策略模拟器实例
	 * @return 选股策略模拟器指针
	 * @details 获取当前创建的选股策略模拟器实例，可以用于直接操作策略模拟器
	 */
	inline SelMocker*		sel_mocker() { return _sel_mocker; }

	/**
	 * @brief 获取高频策略模拟器实例
	 * @return 高频策略模拟器指针
	 * @details 获取当前创建的高频策略模拟器实例，可以用于直接操作策略模拟器
	 */
	inline HftMocker*		hft_mocker() { return _hft_mocker; }

	/**
	 * @brief 获取历史数据回放器实例
	 * @return 历史数据回放器引用
	 * @details 获取内部的历史数据回放器实例，可以用于直接控制数据回放的行为
	 */
	inline HisDataReplayer&	replayer() { return _replayer; }

	/**
	 * @brief 获取当前是否为异步模式
	 * @return 如果当前处于异步模式返回true，否则返回false
	 * @details 检查当前回测引擎是否处于异步运行模式
	 *          当run方法的bAsync参数设置为true时，此方法将返回true
	 */
	inline bool	isAsync() const { return _async; }

public:
	/**
	 * @brief 初始化事件处理
	 * @details 处理回测引擎的初始化事件
	 *          当回测引擎完成初始化后调用此方法触发外部注册的事件回调
	 *          使用EVENT_ENGINE_INIT事件类型通知外部注册的事件回调函数
	 */
	inline void on_initialize_event()
	{
		if (_cb_evt)
			_cb_evt(EVENT_ENGINE_INIT, 0, 0);
	}

	/**
	 * @brief 调度事件处理
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS
	 * @details 处理回测引擎的计算调度事件
	 *          当回测引擎触发调度事件时调用此方法，通知外部注册的事件回调
	 *          使用EVENT_ENGINE_SCHDL事件类型，并传递当前的日期和时间
	 */
	inline void on_schedule_event(uint32_t uDate, uint32_t uTime)
	{
		if (_cb_evt)
			_cb_evt(EVENT_ENGINE_SCHDL, uDate, uTime);
	}

	/**
	 * @brief 交易日事件处理
	 * @param uDate 交易日期，格式为YYYYMMDD
	 * @param isBegin 是否为交易日开始事件，默认为true，false表示交易日结束事件
	 * @details 处理回测引擎的交易日事件
	 *          当回测引擎触发交易日开始或结束事件时调用此方法
	 *          根据isBegin参数决定使用EVENT_SESSION_BEGIN或EVENT_SESSION_END事件类型
	 *          并传递当前的交易日期
	 */
	inline void on_session_event(uint32_t uDate, bool isBegin = true)
	{
		if (_cb_evt)
		{
			_cb_evt(isBegin ? EVENT_SESSION_BEGIN : EVENT_SESSION_END, uDate, 0);
		}
	}

	/**
	 * @brief 回测结束事件处理
	 * @details 处理回测引擎的回测结束事件
	 *          当回测引擎完成所有回测流程后调用此方法
	 *          使用EVENT_BACKTEST_END事件类型通知外部注册的事件回调函数
	 *          在此事件中可以进行回测结果的统计、分析和输出
	 */
	inline void on_backtest_end()
	{
		if (_cb_evt)
			_cb_evt(EVENT_BACKTEST_END, 0, 0);
	}

private:
	FuncStraInitCallback	_cb_cta_init;
	FuncSessionEvtCallback	_cb_cta_sessevt;
	FuncStraTickCallback	_cb_cta_tick;
	FuncStraCalcCallback	_cb_cta_calc;
	FuncStraCalcCallback	_cb_cta_calc_done;
	FuncStraBarCallback		_cb_cta_bar;
	FuncStraCondTriggerCallback _cb_cta_cond_trigger;

	FuncStraInitCallback	_cb_sel_init;
	FuncSessionEvtCallback	_cb_sel_sessevt;
	FuncStraTickCallback	_cb_sel_tick;
	FuncStraCalcCallback	_cb_sel_calc;
	FuncStraCalcCallback	_cb_sel_calc_done;
	FuncStraBarCallback		_cb_sel_bar;

	FuncStraInitCallback	_cb_hft_init;
	FuncSessionEvtCallback	_cb_hft_sessevt;
	FuncStraTickCallback	_cb_hft_tick;
	FuncStraBarCallback		_cb_hft_bar;
	FuncHftChannelCallback	_cb_hft_chnl;
	FuncHftOrdCallback		_cb_hft_ord;
	FuncHftTrdCallback		_cb_hft_trd;
	FuncHftEntrustCallback	_cb_hft_entrust;

	FuncStraOrdQueCallback	_cb_hft_ordque;
	FuncStraOrdDtlCallback	_cb_hft_orddtl;
	FuncStraTransCallback	_cb_hft_trans;

	FuncEventCallback		_cb_evt;

	FuncLoadFnlBars			_ext_fnl_bar_loader;//最终K线加载器
	FuncLoadRawBars			_ext_raw_bar_loader;//原始K线加载器
	FuncLoadAdjFactors		_ext_adj_fct_loader;//复权因子加载器
	FuncLoadRawTicks		_ext_tick_loader;	//tick加载器
	bool					_loader_auto_trans;	//是否自动转储

	CtaMocker*		_cta_mocker;
	SelMocker*		_sel_mocker;
	ExecMocker*		_exec_mocker;
	HftMocker*		_hft_mocker;
	HisDataReplayer	_replayer;
	EventNotifier	_notifier;

	bool			_inited;
	bool			_running;

	StdThreadPtr	_worker;
	bool			_async;

	void*			_feed_obj;
	FuncReadBars	_feeder_bars;
	FuncReadTicks	_feeder_ticks;
	FuncReadFactors	_feeder_fcts;
	StdUniqueMutex	_feed_mtx;
	WTSVariant* _cfg;
};

