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

	uint32_t	initCtaMocker(const char* name, int32_t slippage = 0, bool hook = false, bool persistData = true, bool bIncremental = false, bool isRatioSlp = false);
	uint32_t	initHftMocker(const char* name, bool hook = false);
	uint32_t	initSelMocker(const char* name, uint32_t date, uint32_t time, const char* period, 
		const char* trdtpl = "CHINA", const char* session = "TRADING", int32_t slippage = 0, bool isRatioSlp = false);

	bool	initEvtNotifier(WTSVariant* cfg);

	void	ctx_on_init(uint32_t id, EngineType eType);
	void	ctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin = true, EngineType eType = ET_CTA);
	void	ctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType);
	void	ctx_on_calc(uint32_t id, uint32_t uDate, uint32_t uTime, EngineType eType);
	void	ctx_on_calc_done(uint32_t id, uint32_t uDate, uint32_t uTime, EngineType eType);
	void	ctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType);
	void	ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType = ET_CTA);

	void	hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue);
	void	hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl);
	void	hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTranns);

	void	hft_on_channel_ready(uint32_t cHandle, const char* trader);
	void	hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag);
	void	hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag);
	void	hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag);

	void	init(const char* logProfile = "", bool isFile = true, const char* outDir = "./outputs_bt");
	void	config(const char* cfgFile, bool isFile = true);
	void	run(bool bNeedDump = false, bool bAsync = false);
	void	release();
	void	stop();

	void	set_time_range(WtUInt64 stime, WtUInt64 etime);

	void	enable_tick(bool bEnabled = true);

	void	clear_cache();

	const char*	get_raw_stdcode(const char* stdCode);

	inline CtaMocker*		cta_mocker() { return _cta_mocker; }
	inline SelMocker*		sel_mocker() { return _sel_mocker; }
	inline HftMocker*		hft_mocker() { return _hft_mocker; }
	inline HisDataReplayer&	replayer() { return _replayer; }

	inline bool	isAsync() const { return _async; }

public:
	inline void on_initialize_event()
	{
		if (_cb_evt)
			_cb_evt(EVENT_ENGINE_INIT, 0, 0);
	}

	inline void on_schedule_event(uint32_t uDate, uint32_t uTime)
	{
		if (_cb_evt)
			_cb_evt(EVENT_ENGINE_SCHDL, uDate, uTime);
	}

	inline void on_session_event(uint32_t uDate, bool isBegin = true)
	{
		if (_cb_evt)
		{
			_cb_evt(isBegin ? EVENT_SESSION_BEGIN : EVENT_SESSION_END, uDate, 0);
		}
	}

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

