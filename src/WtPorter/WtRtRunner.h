/*!
 * \file WtRtRunner.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader实时交易运行器定义
 * \details 定义了WonderTrader的实时交易运行器，用于管理和协调各类策略引擎、数据源和交易接口
 */
#pragma once
#include <string>

#include "PorterDefs.h"

#include "../Includes/ILogHandler.h"
#include "../Includes/IDataReader.h"

#include "../WtCore/EventNotifier.h"
#include "../WtCore/CtaStrategyMgr.h"
#include "../WtCore/HftStrategyMgr.h"
#include "../WtCore/SelStrategyMgr.h"
#include "../WtCore/WtCtaEngine.h"
#include "../WtCore/WtHftEngine.h"
#include "../WtCore/WtSelEngine.h"
#include "../WtCore/WtLocalExecuter.h"
#include "../WtCore/WtDiffExecuter.h"
#include "../WtCore/WtDistExecuter.h"
#include "../WtCore/WtArbiExecuter.h"
#include "../WtCore/TraderAdapter.h"
#include "../WtCore/ParserAdapter.h"
#include "../WtCore/WtDtMgr.h"
#include "../WtCore/ActionPolicyMgr.h"

#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"

NS_WTP_BEGIN
class WTSVariant;
class WtDataStorage;
NS_WTP_END

USING_NS_WTP;

/**
 * @brief 引擎类型枚举
 * @details 定义了WonderTrader支持的不同策略引擎类型
 */
typedef enum tagEngineType
{
	ET_CTA = 999,	//CTA引擎	
	ET_HFT,			//高频引擎
	ET_SEL			//选股引擎
} EngineType;

/**
 * @brief WonderTrader实时交易运行器类
 * @details 实现了实时交易的各种功能，包括初始化、配置、运行和回调等
 *          继承自IEngineEvtListener、ILogHandler和IHisDataLoader接口
 *          用于管理CTA、HFT和SEL等不同类型的策略引擎
 */
class WtRtRunner : public IEngineEvtListener, public ILogHandler, public IHisDataLoader
{
public:
	/**
	 * @brief 构造函数
	 */
	WtRtRunner();
	
	/**
	 * @brief 析构函数
	 */
	~WtRtRunner();

public:
	//////////////////////////////////////////////////////////////////////////
	//IHisDataLoader接口实现
	/**
	 * @brief 加载最终历史K线数据
	 * @details 加载指定合约的最终历史K线数据，已经过滤和调整
	 * @param obj 传入的上下文指针
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param cb 数据读取回调函数
	 * @return 是否成功
	 */
	virtual bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override;

	/**
	 * @brief 加载原始历史K线数据
	 * @details 加载指定合约的原始历史K线数据，未经过滤和调整
	 * @param obj 传入的上下文指针
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param cb 数据读取回调函数
	 * @return 是否成功
	 */
	virtual bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override;

	/**
	 * @brief 加载所有复权因子
	 * @details 加载系统中所有合约的复权因子
	 * @param obj 传入的上下文指针
	 * @param cb 数据读取回调函数
	 * @return 是否成功
	 */
	virtual bool loadAllAdjFactors(void* obj, FuncReadFactors cb) override;

	/**
	 * @brief 加载指定合约的复权因子
	 * @details 加载指定合约的复权因子
	 * @param obj 传入的上下文指针
	 * @param stdCode 标准化合约代码
	 * @param cb 数据读取回调函数
	 * @return 是否成功
	 */
	virtual bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override;

	/**
	 * @brief 输入原始K线数据
	 * @details 用于外部模块向运行器提供原始K线数据
	 * @param bars K线数据数组
	 * @param count 数据数量
	 */
	void feedRawBars(WTSBarStruct* bars, uint32_t count);

	/**
	 * @brief 输入复权因子数据
	 * @details 用于外部模块向运行器提供复权因子数据
	 * @param stdCode 标准化合约代码
	 * @param dates 日期数组
	 * @param factors 因子数组
	 * @param count 数据数量
	 */
	void feedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count);

public:
	/**
	 * @brief 初始化运行器
	 * @details 初始化运行器，包括日志等基础组件
	 * @param logCfg 日志配置文件或内容
	 * @param isFile 是否为文件路径，true表示文件路径，false表示配置内容
	 * @param genDir 数据生成目录
	 * @return 是否初始化成功
	 */
	bool init(const char* logCfg = "logcfg.prop", bool isFile = true, const char* genDir = "");

	/**
	 * @brief 配置运行器
	 * @details 使用指定的配置文件或内容配置运行器
	 * @param cfgFile 配置文件或内容
	 * @param isFile 是否为文件路径，true表示文件路径，false表示配置内容
	 * @return 是否配置成功
	 */
	bool config(const char* cfgFile, bool isFile = true);

	/**
	 * @brief 运行交易引擎
	 * @details 启动交易引擎，可以同步或异步模式运行
	 * @param bAsync 是否异步运行，true表示异步，false表示同步
	 */
	void run(bool bAsync = false);

	/**
	 * @brief 释放运行器资源
	 * @details 清理并释放运行器的所有资源
	 */
	void release();

	/**
	 * @brief 注册CTA策略回调函数
	 * @details 注册CTA策略的各种事件回调函数
	 * @param cbInit 初始化回调
	 * @param cbTick Tick数据回调
	 * @param cbCalc 计算回调
	 * @param cbBar K线回调
	 * @param cbSessEvt 交易日事件回调
	 * @param cbCondTrigger 条件触发回调，可选
	 */
	void registerCtaCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCondTriggerCallback cbCondTrigger = NULL);
	/**
	 * @brief 注册SEL选股策略回调函数
	 * @details 注册SEL选股策略的各种事件回调函数
	 * @param cbInit 初始化回调
	 * @param cbTick Tick数据回调
	 * @param cbCalc 计算回调
	 * @param cbBar K线回调
	 * @param cbSessEvt 交易日事件回调
	 */
	void registerSelCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt);
	/**
	 * @brief 注册HFT高频策略回调函数
	 * @details 注册HFT高频策略的各种事件回调函数
	 * @param cbInit 初始化回调
	 * @param cbTick Tick数据回调
	 * @param cbBar K线回调
	 * @param cbChnl 通道事件回调
	 * @param cbOrd 订单回调
	 * @param cbTrd 成交回调
	 * @param cbEntrust 委托回调
	 * @param cbOrdDtl 订单明细回调
	 * @param cbOrdQue 订单队列回调
	 * @param cbTrans 成交回调
	 * @param cbSessEvt 交易日事件回调
	 * @param cbPosition 持仓回调
	 */
	void registerHftCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraBarCallback cbBar,
		FuncHftChannelCallback cbChnl, FuncHftOrdCallback cbOrd, FuncHftTrdCallback cbTrd, FuncHftEntrustCallback cbEntrust,
		FuncStraOrdDtlCallback cbOrdDtl, FuncStraOrdQueCallback cbOrdQue, FuncStraTransCallback cbTrans, FuncSessionEvtCallback cbSessEvt, FuncHftPosCallback cbPosition);

	/**
	 * @brief 注册事件回调函数
	 * @details 注册引擎事件回调函数，用于接收引擎事件通知
	 * @param cbEvt 事件回调函数
	 */
	void registerEvtCallback(FuncEventCallback cbEvt);

	/**
	 * @brief 注册Parser数据解析器回调函数
	 * @details 注册Parser数据解析器的事件和订阅回调函数
	 * @param cbEvt 事件回调函数
	 * @param cbSub 订阅回调函数
	 */
	void registerParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub);

	/**
	 * @brief 注册Executer执行器回调函数
	 * @details 注册Executer执行器的初始化和命令回调函数
	 * @param cbInit 初始化回调函数
	 * @param cbExec 命令回调函数
	 */
	void registerExecuterPorter(FuncExecInitCallback cbInit, FuncExecCmdCallback cbExec);

	/**
	 * @brief 注册外部数据加载器
	 * @details 注册外部数据源的各种数据加载函数
	 * @param fnlBarLoader 最终K线数据加载器
	 * @param rawBarLoader 原始K线数据加载器
	 * @param fctLoader 复权因子加载器
	 * @param tickLoader Tick数据加载器，可选
	 */
	void		registerExtDataLoader(FuncLoadFnlBars fnlBarLoader, FuncLoadRawBars rawBarLoader, FuncLoadAdjFactors fctLoader, FuncLoadRawTicks tickLoader = NULL)
	{
		_ext_fnl_bar_loader = fnlBarLoader;
		_ext_raw_bar_loader = rawBarLoader;
		_ext_adj_fct_loader = fctLoader;
	}

	/**
	 * @brief 创建外部数据解析器
	 * @details 创建一个外部数据解析器，用于处理外部数据源
	 * @param id 解析器ID
	 * @return 是否创建成功
	 */
	bool			createExtParser(const char* id);
	
	/**
	 * @brief 创建外部执行器
	 * @details 创建一个外部执行器，用于处理外部交易指令
	 * @param id 执行器ID
	 * @return 是否创建成功
	 */
	bool			createExtExecuter(const char* id);

	/**
	 * @brief 创建CTA策略上下文
	 * @details 创建一个CTA策略的运行上下文
	 * @param name 策略名称
	 * @param slippage 滑点设置
	 * @return 上下文ID
	 */
	uint32_t		createCtaContext(const char* name, int32_t slippage);
	
	/**
	 * @brief 创建HFT高频策略上下文
	 * @details 创建一个HFT高频策略的运行上下文
	 * @param name 策略名称
	 * @param trader 交易通道ID
	 * @param bAgent 是否为代理模式
	 * @param slippage 滑点设置
	 * @return 上下文ID
	 */
	uint32_t		createHftContext(const char* name, const char* trader, bool bAgent, int32_t slippage);
	
	/**
	 * @brief 创建SEL选股策略上下文
	 * @details 创建一个SEL选股策略的运行上下文
	 * @param name 策略名称
	 * @param date 日期，格式为YYYYMMDD
	 * @param time 时间，格式为HHMMSS
	 * @param period 周期设置
	 * @param slippage 滑点设置
	 * @param trdtpl 交易模板，默认为"CHINA"
	 * @param session 交易时段，默认为"TRADING"
	 * @return 上下文ID
	 */
	uint32_t		createSelContext(const char* name, uint32_t date, uint32_t time, const char* period, int32_t slippage, const char* trdtpl = "CHINA", const char* session="TRADING");

	/**
	 * @brief 获取CTA策略上下文
	 * @param id 上下文ID
	 * @return CTA策略上下文指针
	 */
	CtaContextPtr	getCtaContext(uint32_t id);
	
	/**
	 * @brief 获取SEL选股策略上下文
	 * @param id 上下文ID
	 * @return SEL选股策略上下文指针
	 */
	SelContextPtr	getSelContext(uint32_t id);
	
	/**
	 * @brief 获取HFT高频策略上下文
	 * @param id 上下文ID
	 * @return HFT高频策略上下文指针
	 */
	HftContextPtr	getHftContext(uint32_t id);
	
	/**
	 * @brief 获取当前使用的引擎
	 * @return 当前使用的引擎指针
	 */
	WtEngine*		getEngine(){ return _engine; }

	/**
	 * @brief 获取原始标准化代码
	 * @details 从标准化合约代码中获取原始的标准化代码
	 * @param stdCode 标准化合约代码
	 * @return 原始标准化代码
	 */
	const char*	get_raw_stdcode(const char* stdCode);

//////////////////////////////////////////////////////////////////////////
//ILogHandler接口实现
public:
	/**
	 * @brief 日志处理函数
	 * @details 实现ILogHandler接口的日志处理函数
	 * @param ll 日志级别
	 * @param msg 日志消息
	 */
	virtual void handleLogAppend(WTSLogLevel ll, const char* msg) override;

//////////////////////////////////////////////////////////////////////////
//扩展Parser接口
public:
	/**
	 * @brief 初始化Parser数据解析器
	 * @param id Parser数据解析器ID
	 */
	void parser_init(const char* id);
	
	/**
	 * @brief 连接Parser数据解析器
	 * @param id Parser数据解析器ID
	 */
	void parser_connect(const char* id);
	
	/**
	 * @brief 释放Parser数据解析器
	 * @param id Parser数据解析器ID
	 */
	void parser_release(const char* id);
	
	/**
	 * @brief 断开Parser数据解析器连接
	 * @param id Parser数据解析器ID
	 */
	void parser_disconnect(const char* id);
	
	/**
	 * @brief Parser数据解析器订阅数据
	 * @param id Parser数据解析器ID
	 * @param code 合约代码
	 */
	void parser_subscribe(const char* id, const char* code);
	
	/**
	 * @brief Parser数据解析器取消订阅
	 * @param id Parser数据解析器ID
	 * @param code 合约代码
	 */
	void parser_unsubscribe(const char* id, const char* code);

	/**
	 * @brief 外部Parser数据解析器行情回调
	 * @param id Parser数据解析器ID
	 * @param curTick 当前Tick数据
	 * @param uProcFlag 处理标记
	 */
	void on_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag);


//////////////////////////////////////////////////////////////////////////
//扩展Executer接口
public:
	/**
	 * @brief 设置Executer执行器持仓
	 * @param id Executer执行器ID
	 * @param stdCode 标准化合约代码
	 * @param target 目标仓位
	 */
	void executer_set_position(const char* id, const char* stdCode, double target);
	
	/**
	 * @brief 初始化Executer执行器
	 * @param id Executer执行器ID
	 */
	void executer_init(const char* id);

//////////////////////////////////////////////////////////////////////////
//IEngineEvtListener接口实现
public:
	/**
	 * @brief 初始化事件回调
	 * @details IEngineEvtListener接口实现，引擎初始化事件回调
	 */
	virtual void on_initialize_event() override
	{
		if (_cb_evt)
			_cb_evt(EVENT_ENGINE_INIT, 0, 0);
	}

	/**
	 * @brief 调度事件回调
	 * @details IEngineEvtListener接口实现，引擎调度事件回调
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS或HHMMSS000
	 */
	virtual void on_schedule_event(uint32_t uDate, uint32_t uTime) override
	{
		if (_cb_evt)
			_cb_evt(EVENT_ENGINE_SCHDL, uDate, uTime);
	}

	/**
	 * @brief 交易日事件回调
	 * @details IEngineEvtListener接口实现，交易日开始或结束事件回调
	 * @param uDate 交易日日期，格式为YYYYMMDD
	 * @param isBegin 是否为开始事件，true表示开始，false表示结束
	 */
	virtual void on_session_event(uint32_t uDate, bool isBegin = true) override
	{
		if (_cb_evt)
			_cb_evt(isBegin ? EVENT_SESSION_BEGIN : EVENT_SESSION_END, uDate, 0);
	}

public:
	/**
	 * @brief 上下文初始化回调
	 * @param id 上下文ID
	 * @param eType 引擎类型，默认为CTA引擎
	 */
	void ctx_on_init(uint32_t id, EngineType eType = ET_CTA);
	
	/**
	 * @brief 上下文交易日事件回调
	 * @param id 上下文ID
	 * @param curTDate 当前交易日日期，格式为YYYYMMDD
	 * @param isBegin 是否为开始事件，true表示开始，false表示结束
	 * @param eType 引擎类型，默认为CTA引擎
	 */
	void ctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin = true, EngineType eType = ET_CTA);
	
	/**
	 * @brief 上下文Tick数据回调
	 * @param id 上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 * @param eType 引擎类型，默认为CTA引擎
	 */
	void ctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType = ET_CTA);
	
	/**
	 * @brief 上下文计算回调
	 * @param id 上下文ID
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMMSS或HHMMSS000
	 * @param eType 引擎类型，默认为CTA引擎
	 */
	void ctx_on_calc(uint32_t id, uint32_t curDate, uint32_t curTime, EngineType eType = ET_CTA);
	
	/**
	 * @brief 上下文K线数据回调
	 * @param id 上下文ID
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param newBar 新的K线数据
	 * @param eType 引擎类型，默认为CTA引擎
	 */
	void ctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType = ET_CTA);
	
	/**
	 * @brief 上下文条件触发回调
	 * @param id 上下文ID
	 * @param stdCode 标准化合约代码
	 * @param target 目标价格
	 * @param price 当前价格
	 * @param usertag 用户标签
	 * @param eType 引擎类型，默认为CTA引擎
	 */
	void ctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType = ET_CTA);

	/**
	 * @brief HFT通道就绪回调
	 * @param cHandle 上下文句柄
	 * @param trader 交易器ID
	 */
	void hft_on_channel_ready(uint32_t cHandle, const char* trader);
	
	/**
	 * @brief HFT通道断开回调
	 * @param cHandle 上下文句柄
	 * @param trader 交易器ID
	 */
	void hft_on_channel_lost(uint32_t cHandle, const char* trader);
	
	/**
	 * @brief HFT订单回调
	 * @param cHandle 上下文句柄
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入
	 * @param totalQty 总数量
	 * @param leftQty 剩余数量
	 * @param price 价格
	 * @param isCanceled 是否已撤单
	 * @param userTag 用户标签
	 */
	void hft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag);
	
	/**
	 * @brief HFT成交回调
	 * @param cHandle 上下文句柄
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入
	 * @param vol 成交量
	 * @param price 成交价
	 * @param userTag 用户标签
	 */
	void hft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag);
	
	/**
	 * @brief HFT委托回调
	 * @param cHandle 上下文句柄
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 是否成功
	 * @param message 消息内容
	 * @param userTag 用户标签
	 */
	void hft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag);
	
	/**
	 * @brief HFT持仓回调
	 * @param cHandle 上下文句柄
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头
	 * @param prevol 前持仓量
	 * @param preavail 前可用仓量
	 * @param newvol 新持仓量
	 * @param newavail 新可用仓量
	 */
	void hft_on_position(uint32_t cHandle, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail);

	/**
	 * @brief HFT订单队列回调
	 * @param id 上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 新订单队列数据
	 */
	void hft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue);
	
	/**
	 * @brief HFT订单明细回调
	 * @param id 上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 新订单明细数据
	 */
	void hft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl);
	
	/**
	 * @brief HFT成交明细回调
	 * @param id 上下文ID
	 * @param stdCode 标准化合约代码
	 * @param newTranns 新成交数据
	 */
	void hft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTranns);

	/**
	 * @brief 添加执行器工厂
	 * @details 从指定文件夹加载执行器工厂模块
	 * @param folder 模块文件夹路径
	 * @return 是否成功加载
	 */
	bool addExeFactories(const char* folder);
	
	/**
	 * @brief 添加CTA策略工厂
	 * @details 从指定文件夹加载CTA策略工厂模块
	 * @param folder 模块文件夹路径
	 * @return 是否成功加载
	 */
	bool addCtaFactories(const char* folder);
	
	/**
	 * @brief 添加HFT高频策略工厂
	 * @details 从指定文件夹加载HFT高频策略工厂模块
	 * @param folder 模块文件夹路径
	 * @return 是否成功加载
	 */
	bool addHftFactories(const char* folder);
	
	/**
	 * @brief 添加SEL选股策略工厂
	 * @details 从指定文件夹加载SEL选股策略工厂模块
	 * @param folder 模块文件夹路径
	 * @return 是否成功加载
	 */
	bool addSelFactories(const char* folder);

private:
	/**
	 * @brief 初始化交易适配器
	 * @details 根据配置初始化交易适配器
	 * @param cfgTrader 交易配置
	 * @return 是否初始化成功
	 */
	bool initTraders(WTSVariant* cfgTrader);
	
	/**
	 * @brief 初始化数据解析器
	 * @details 根据配置初始化数据解析器
	 * @param cfgParser 数据解析器配置
	 * @return 是否初始化成功
	 */
	bool initParsers(WTSVariant* cfgParser);
	
	/**
	 * @brief 初始化执行器
	 * @details 根据配置初始化执行器
	 * @param cfgExecuter 执行器配置
	 * @return 是否初始化成功
	 */
	bool initExecuters(WTSVariant* cfgExecuter);
	
	/**
	 * @brief 初始化数据管理器
	 * @details 初始化数据管理器，包括基础数据和历史数据
	 * @return 是否初始化成功
	 */
	bool initDataMgr();
	
	/**
	 * @brief 初始化事件通知器
	 * @details 初始化事件通知器，用于管理各类事件
	 * @return 是否初始化成功
	 */
	bool initEvtNotifier();
	
	/**
	 * @brief 初始化CTA策略
	 * @details 根据配置初始化CTA策略
	 * @return 是否初始化成功
	 */
	bool initCtaStrategies();
	
	/**
	 * @brief 初始化HFT高频策略
	 * @details 根据配置初始化HFT高频策略
	 * @return 是否初始化成功
	 */
	bool initHftStrategies();
	
	/**
	 * @brief 初始化SEL选股策略
	 * @details 根据配置初始化SEL选股策略
	 * @return 是否初始化成功
	 */
	bool initSelStrategies();
	
	/**
	 * @brief 初始化操作策略
	 * @details 初始化操作策略管理器
	 * @return 是否初始化成功
	 */
	bool initActionPolicy();

	/**
	 * @brief 初始化引擎
	 * @details 初始化交易引擎，根据配置选择不同类型的引擎
	 * @return 是否初始化成功
	 */
	bool initEngine();

private:
	//CTA策略回调函数
	/**
	 * @brief CTA策略初始化回调函数
	 */
	FuncStraInitCallback	_cb_cta_init;
	/**
	 * @brief CTA策略交易日事件回调函数
	 */
	FuncSessionEvtCallback	_cb_cta_sessevt;
	/**
	 * @brief CTA策略Tick数据回调函数
	 */
	FuncStraTickCallback	_cb_cta_tick;
	/**
	 * @brief CTA策略计算回调函数
	 */
	FuncStraCalcCallback	_cb_cta_calc;
	/**
	 * @brief CTA策略K线回调函数
	 */
	FuncStraBarCallback		_cb_cta_bar;
	/**
	 * @brief CTA策略条件触发回调函数
	 */
	FuncStraCondTriggerCallback _cb_cta_cond_trigger;

	//SEL选股策略回调函数
	/**
	 * @brief SEL选股策略初始化回调函数
	 */
	FuncStraInitCallback	_cb_sel_init;
	/**
	 * @brief SEL选股策略交易日事件回调函数
	 */
	FuncSessionEvtCallback	_cb_sel_sessevt;
	/**
	 * @brief SEL选股策略Tick数据回调函数
	 */
	FuncStraTickCallback	_cb_sel_tick;
	/**
	 * @brief SEL选股策略计算回调函数
	 */
	FuncStraCalcCallback	_cb_sel_calc;
	/**
	 * @brief SEL选股策略K线回调函数
	 */
	FuncStraBarCallback		_cb_sel_bar;

	//HFT高频策略回调函数
	/**
	 * @brief HFT高频策略初始化回调函数
	 */
	FuncStraInitCallback	_cb_hft_init;
	/**
	 * @brief HFT高频策略交易日事件回调函数
	 */
	FuncSessionEvtCallback	_cb_hft_sessevt;
	/**
	 * @brief HFT高频策略Tick数据回调函数
	 */
	FuncStraTickCallback	_cb_hft_tick;
	/**
	 * @brief HFT高频策略K线回调函数
	 */
	FuncStraBarCallback		_cb_hft_bar;
	/**
	 * @brief HFT高频策略通道事件回调函数
	 */
	FuncHftChannelCallback	_cb_hft_chnl;
	/**
	 * @brief HFT高频策略订单回调函数
	 */
	FuncHftOrdCallback		_cb_hft_ord;
	/**
	 * @brief HFT高频策略成交回调函数
	 */
	FuncHftTrdCallback		_cb_hft_trd;
	/**
	 * @brief HFT高频策略委托回调函数
	 */
	FuncHftEntrustCallback	_cb_hft_entrust;
	/**
	 * @brief HFT高频策略持仓回调函数
	 */
	FuncHftPosCallback		_cb_hft_position;

	/**
	 * @brief HFT高频策略订单队列回调函数
	 */
	FuncStraOrdQueCallback	_cb_hft_ordque;
	/**
	 * @brief HFT高频策略订单明细回调函数
	 */
	FuncStraOrdDtlCallback	_cb_hft_orddtl;
	/**
	 * @brief HFT高频策略成交明细回调函数
	 */
	FuncStraTransCallback	_cb_hft_trans;

	/**
	 * @brief 引擎事件回调函数
	 */
	FuncEventCallback		_cb_evt;

	/**
	 * @brief Parser解析器事件回调函数
	 */
	FuncParserEvtCallback	_cb_parser_evt;
	/**
	 * @brief Parser解析器订阅回调函数
	 */
	FuncParserSubCallback	_cb_parser_sub;

	/**
	 * @brief Executer执行器命令回调函数
	 */
	FuncExecCmdCallback		_cb_exec_cmd;
	/**
	 * @brief Executer执行器初始化回调函数
	 */
	FuncExecInitCallback	_cb_exec_init;

	/**
	 * @brief 配置信息
	 */
	WTSVariant*			_config;
	/**
	 * @brief 交易适配器管理器
	 */
	TraderAdapterMgr	_traders;
	/**
	 * @brief 数据解析器管理器
	 */
	ParserAdapterMgr	_parsers;
	/**
	 * @brief 执行器工厂
	 */
	WtExecuterFactory	_exe_factory;

	/**
	 * @brief CTA交易引擎
	 */
	WtCtaEngine			_cta_engine;
	/**
	 * @brief HFT高频交易引擎
	 */
	WtHftEngine			_hft_engine;
	/**
	 * @brief SEL选股交易引擎
	 */
	WtSelEngine			_sel_engine;
	/**
	 * @brief 当前使用的引擎指针
	 */
	WtEngine*			_engine;

	/**
	 * @brief 数据存储器
	 */
	WtDataStorage*		_data_store;

	/**
	 * @brief 数据管理器
	 */
	WtDtMgr				_data_mgr;

	/**
	 * @brief 基础数据管理器
	 */
	WTSBaseDataMgr		_bd_mgr;
	/**
	 * @brief 主力合约管理器
	 */
	WTSHotMgr			_hot_mgr;
	/**
	 * @brief 事件通知器
	 */
	EventNotifier		_notifier;

	/**
	 * @brief CTA策略管理器
	 */
	CtaStrategyMgr		_cta_mgr;
	/**
	 * @brief HFT高频策略管理器
	 */
	HftStrategyMgr		_hft_mgr;
	/**
	 * @brief SEL选股策略管理器
	 */
	SelStrategyMgr		_sel_mgr;
	/**
	 * @brief 操作策略管理器
	 */
	ActionPolicyMgr		_act_policy;

	/**
	 * @brief 是否为HFT高频模式
	 */
	bool				_is_hft;
	/**
	 * @brief 是否为SEL选股模式
	 */
	bool				_is_sel;
	/**
	 * @brief 是否退出
	 */
	bool				_to_exit;

	/**
	 * @brief 外部最终K线数据加载器
	 */
	FuncLoadFnlBars		_ext_fnl_bar_loader;
	/**
	 * @brief 外部原始K线数据加载器
	 */
	FuncLoadRawBars		_ext_raw_bar_loader;
	/**
	 * @brief 外部复权因子加载器
	 */
	FuncLoadAdjFactors	_ext_adj_fct_loader;

	/**
	 * @brief 数据预加载对象
	 */
	void*			_feed_obj;
	/**
	 * @brief K线数据预加载回调函数
	 */
	FuncReadBars	_feeder_bars;
	/**
	 * @brief 复权因子预加载回调函数
	 */
	FuncReadFactors	_feeder_fcts;
	/**
	 * @brief 预加载数据同步锁
	 */
	StdUniqueMutex	_feed_mtx;
};

