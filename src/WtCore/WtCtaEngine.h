/*!
 * \file WtCtaEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 * 
 * WtCtaEngine是WonderTrader框架中的CTA策略引擎类
 * 继承自WtEngine基类，实现了CTA策略的运行、管理和交易执行
 * CTA（Commodity Trading Advisor）策略主要用于期货、期权等品种的量化交易
 * 本文件定义了CTA策略引擎的核心类和接口
 */
#pragma once
#include "../Includes/ICtaStraCtx.h"
#include "../Share/threadpool.hpp"
#include "WtExecMgr.h"
#include "WtEngine.h"

NS_WTP_BEGIN
class WTSVariant;
typedef std::shared_ptr<ICtaStraCtx> CtaContextPtr;

class WtCtaRtTicker;

/**
 * @brief CTA策略引擎类
 * @details 继承自WtEngine基类和IExecuterStub接口，实现了CTA策略的运行、管理和交易执行
 *          负责协调多个CTA策略的运行，处理行情数据和交易信号
 *          实现了定时器、交易执行等功能
 */
class WtCtaEngine : public WtEngine, public IExecuterStub
{
public:
	/**
	 * @brief 构造函数
	 */
	WtCtaEngine();

	/**
	 * @brief 析构函数
	 */
	virtual ~WtCtaEngine();

public:
	//////////////////////////////////////////////////////////////////////////
	//WtEngine接口
	/**
	 * @brief 处理推送的行情数据
	 * @details 重写自WtEngine基类，处理新的Tick行情数据，分发给各个策略
	 * 
	 * @param newTick 新的Tick行情数据
	 */
	virtual void handle_push_quote(WTSTickData* newTick) override;

	/**
	 * @brief 处理Tick数据回调
	 * @details 重写自WtEngine基类，当收到新的Tick数据时调用，处理行情并更新策略
	 * 
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前Tick数据
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* curTick) override;

	/**
	 * @brief 处理K线数据回调
	 * @details 重写自WtEngine基类，当收到新的K线数据时调用，分发给各个策略
	 * 
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如m1、m5等
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 引擎初始化回调
	 * @details 重写自WtEngine基类，在引擎初始化完成时调用，用于初始化策略
	 */
	virtual void on_init() override;

	/**
	 * @brief 交易日开始回调
	 * @details 重写自WtEngine基类，在交易日开始时调用，用于通知各个策略
	 */
	virtual void on_session_begin() override;

	/**
	 * @brief 交易日结束回调
	 * @details 重写自WtEngine基类，在交易日结束时调用，用于通知各个策略并进行结算
	 */
	virtual void on_session_end() override;

	/**
	 * @brief 运行引擎
	 * @details 重写自WtEngine基类，启动CTA引擎的主循环
	 */
	virtual void run() override;

	/**
	 * @brief 初始化引擎
	 * @details 重写自WtEngine基类，初始化CTA引擎，加载配置和策略
	 * 
	 * @param cfg 配置项
	 * @param bdMgr 基础数据管理器
	 * @param dataMgr 数据管理器
	 * @param hotMgr 主力合约管理器
	 * @param notifier 事件通知器
	 */
	virtual void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override;

	/**
	 * @brief 检查是否在交易时段
	 * @details 重写自WtEngine基类，根据当前时间判断是否在交易时段内
	 * 
	 * @return true 在交易时段内
	 * @return false 不在交易时段内
	 */
	virtual bool isInTrading() override;

	/**
	 * @brief 将时间转换为分钟数
	 * @details 重写自WtEngine基类，将HHMM格式的时间转换为分钟数，用于时间比较
	 * 
	 * @param uTime HHMM格式的时间，如1430表示14:30
	 * @return uint32_t 转换后的分钟数，如1430转换为870分钟(14*60+30)
	 */
	virtual uint32_t transTimeToMin(uint32_t uTime) override;

	///////////////////////////////////////////////////////////////////////////
	//IExecuterStub 接口
	/**
	 * @brief 获取当前真实时间
	 * @details 实现IExecuterStub接口，返回当前的真实时间戳，用于交易执行器
	 * 
	 * @return uint64_t 当前真实时间戳
	 */
	virtual uint64_t get_real_time() override;

	/**
	 * @brief 获取商品信息
	 * @details 实现IExecuterStub接口，根据标准化合约代码获取对应的商品信息
	 * 
	 * @param stdCode 标准化合约代码
	 * @return WTSCommodityInfo* 商品信息对象指针
	 */
	virtual WTSCommodityInfo* get_comm_info(const char* stdCode) override;

	/**
	 * @brief 获取交易时段信息
	 * @details 实现IExecuterStub接口，根据标准化合约代码获取对应的交易时段信息
	 * 
	 * @param stdCode 标准化合约代码
	 * @return WTSSessionInfo* 交易时段信息对象指针
	 */
	virtual WTSSessionInfo* get_sess_info(const char* stdCode) override;

	/**
	 * @brief 获取主力合约管理器
	 * @details 实现IExecuterStub接口，返回引擎使用的主力合约管理器
	 * 
	 * @return IHotMgr* 主力合约管理器指针
	 */
	virtual IHotMgr* get_hot_mon() { return _hot_mgr; }

	/**
	 * @brief 获取当前交易日
	 * @details 实现IExecuterStub接口，返回当前的交易日期
	 * 
	 * @return uint32_t 当前交易日期，格式为YYYYMMDD
	 */
	virtual uint32_t get_trading_day() { return _cur_tdate; }


public:
	/**
	 * @brief 定时器回调函数
	 * @details 在指定的日期和时间触发定时器事件，用于定时执行策略逻辑
	 * 
	 * @param curDate 当前日期，格式为YYYYMMDD
	 * @param curTime 当前时间，格式为HHMM
	 */
	void on_schedule(uint32_t curDate, uint32_t curTime);	

	/**
	 * @brief 处理持仓变化
	 * @details 处理策略发出的持仓变化信号，调整实际持仓
	 * 
	 * @param straName 策略名称
	 * @param stdCode 标准化合约代码
	 * @param diffPos 持仓变化量，正数表示增加，负数表示减少
	 */
	void handle_pos_change(const char* straName, const char* stdCode, double diffPos);

	/**
	 * @brief 添加策略上下文
	 * @details 将新的CTA策略上下文添加到引擎中管理
	 * 
	 * @param ctx 策略上下文指针
	 */
	void addContext(CtaContextPtr ctx);
	
	/**
	 * @brief 获取策略上下文
	 * @details 根据ID获取对应的CTA策略上下文
	 * 
	 * @param id 策略上下文ID
	 * @return CtaContextPtr 策略上下文指针
	 */
	CtaContextPtr	getContext(uint32_t id);

	/**
	 * @brief 添加交易执行器
	 * @details 将交易执行器添加到执行管理器中，并设置执行器的根底层
	 * 
	 * @param executer 交易执行器指针
	 */
	inline void addExecuter(ExecCmdPtr executer)
	{
		_exec_mgr.add_executer(executer);
		executer->setStub(this);
	}

	/**
	 * @brief 加载路由规则
	 * @details 加载交易路由规则，用于决定交易指令如何路由到交易接口
	 * 
	 * @param cfg 路由规则配置
	 * @return true 加载成功
	 * @return false 加载失败
	 */
	inline bool loadRouterRules(WTSVariant* cfg)
	{
		return _exec_mgr.load_router_rules(cfg);
	}

public:
	/**
	 * @brief 通知图表标记
	 * @details 向图表上添加标记，用于在图表上显示特定事件，如交易信号、重要时间点等
	 * 
	 * @param time 时间戳
	 * @param straId 策略ID
	 * @param price 价格，用于确定标记在图表上的垂直位置
	 * @param icon 图标名称
	 * @param tag 标记文本
	 */
	void notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag);

	/**
	 * @brief 通知图表指标
	 * @details 向图表上添加指标数据，用于在图表上显示策略计算的指标线
	 * 
	 * @param time 时间戳
	 * @param straId 策略ID
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param val 指标值
	 */
	void notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val);

	/**
	 * @brief 通知交易信息
	 * @details 通知策略产生的交易信息，用于记录和显示交易详情
	 * 
	 * @param straId 策略ID
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头交易
	 * @param isOpen 是否为开仓操作
	 * @param curTime 当前时间戳
	 * @param price 交易价格
	 * @param userTag 用户自定义标签
	 */
	void notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag);

private:
	/**
	 * @brief CTA策略上下文映射类型
	 * @details 使用高效的哈希表映射ID到策略上下文指针
	 */
	typedef wt_hashmap<uint32_t, CtaContextPtr> ContextMap;

	/**
	 * @brief 策略上下文映射表
	 * @details 存储所有注册到引擎的CTA策略上下文，以ID为键
	 */
	ContextMap		_ctx_map;

	/**
	 * @brief 实时定时器
	 * @details 用于定时触发策略的计算和交易信号生成
	 */
	WtCtaRtTicker*	_tm_ticker;

	/**
	 * @brief 交易执行管理器
	 * @details 管理所有交易执行器，负责将策略生成的交易信号转发到实际交易接口
	 */
	WtExecuterMgr	_exec_mgr;

	/**
	 * @brief 配置项
	 * @details 存储引擎的配置信息，包括策略配置、交易时间配置等
	 */
	WTSVariant*		_cfg;

	/**
	 * @brief 线程池指针类型
	 * @details 定义线程池的智能指针类型，用于管理线程资源
	 */
	typedef std::shared_ptr<boost::threadpool::pool> ThreadPoolPtr;

	/**
	 * @brief 线程池
	 * @details 用于并发执行策略计算任务，提高性能
	 */
	ThreadPoolPtr		_pool;
};

NS_WTP_END

