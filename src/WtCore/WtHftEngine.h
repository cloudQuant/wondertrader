/*!
 * \file WtHftEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易引擎头文件
 * \details 本文件定义了WonderTrader高频交易引擎类(WtHftEngine)
 *          高频交易引擎负责管理和执行高频交易策略
 *          包括接收市场数据、处理订单和成交信息、管理策略上下文等
 */
#pragma once
#include "WtEngine.h"
#include "WtLocalExecuter.h"

#include "../Includes/IHftStraCtx.h"

NS_WTP_BEGIN

class WTSVariant;
class WtHftRtTicker;

/**
 * @brief 高频策略上下文指针类型定义
 */
typedef std::shared_ptr<IHftStraCtx> HftContextPtr;

/**
 * @brief 高频交易引擎类
 * @details 高频交易引擎继承自WtEngine基类，负责管理和执行高频交易策略
 *          主要功能包括：
 *          1. 接收市场数据（Tick、委托队列、委托明细、成交明细等）
 *          2. 管理高频策略上下文
 *          3. 处理交易信号和订单
 *          4. 提供市场数据查询接口
 *          高频交易引擎主要针对低延迟、高频率的交易场景
 */
class WtHftEngine :	public WtEngine
{
public:
	/**
	 * @brief 高频交易引擎构造函数
	 * @details 初始化高频交易引擎对象
	 */
	WtHftEngine();

	/**
	 * @brief 高频交易引擎析构函数
	 * @details 清理高频交易引擎资源，包括策略上下文、定时器等
	 */
	virtual ~WtHftEngine();

public:
	//////////////////////////////////////////////////////////////////////////
	//WtEngine 接口
	/**
	 * @brief 初始化高频交易引擎
	 * @param cfg 配置项
	 * @param bdMgr 基础数据管理器
	 * @param dataMgr 数据管理器
	 * @param hotMgr 主力合约管理器
	 * @param notifier 事件通知器
	 * @details 初始化高频交易引擎，加载配置并初始化相关组件
	 *          该函数重写了WtEngine基类的init函数
	 */
	virtual void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override;

	/**
	 * @brief 启动高频交易引擎
	 * @details 启动高频交易引擎的运行，包括初始化定时器、启动策略等
	 *          该函数重写了WtEngine基类的run函数
	 */
	virtual void run() override;

	/**
	 * @brief 处理推送的行情数据
	 * @param newTick 新的Tick数据
	 * @details 处理推送的行情数据，并分发给相关策略
	 *          该函数重写了WtEngine基类的handle_push_quote函数
	 */
	virtual void handle_push_quote(WTSTickData* newTick) override;

	/**
	 * @brief 处理推送的委托明细数据
	 * @param curOrdDtl 当前委托明细数据
	 * @details 处理推送的委托明细数据，并分发给订阅的策略
	 *          该函数重写了WtEngine基类的handle_push_order_detail函数
	 */
	virtual void handle_push_order_detail(WTSOrdDtlData* curOrdDtl) override;

	/**
	 * @brief 处理推送的委托队列数据
	 * @param curOrdQue 当前委托队列数据
	 * @details 处理推送的委托队列数据，并分发给订阅的策略
	 *          该函数重写了WtEngine基类的handle_push_order_queue函数
	 */
	virtual void handle_push_order_queue(WTSOrdQueData* curOrdQue) override;

	/**
	 * @brief 处理推送的成交明细数据
	 * @param curTrans 当前成交明细数据
	 * @details 处理推送的成交明细数据，并分发给订阅的策略
	 *          该函数重写了WtEngine基类的handle_push_transaction函数
	 */
	virtual void handle_push_transaction(WTSTransData* curTrans) override;

	/**
	 * @brief Tick数据回调函数
	 * @param stdCode 标准合约代码
	 * @param curTick 当前Tick数据
	 * @details 当新的Tick数据到达时调用，处理并分发给相关策略
	 *          该函数重写了WtEngine基类的on_tick函数
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* curTick) override;

	/**
	 * @brief K线数据回调函数
	 * @param stdCode 标准合约代码
	 * @param period 周期标识，如"m1"表示1分钟，"d1"表示日线
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 * @details 当新的K线数据生成时调用，处理并分发给相关策略
	 *          该函数重写了WtEngine基类的on_bar函数
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 交易日开始回调函数
	 * @details 在每个交易日开始时调用，执行交易日开始时的相关操作
	 *          该函数重写了WtEngine基类的on_session_begin函数
	 */
	virtual void on_session_begin() override;

	/**
	 * @brief 交易日结束回调函数
	 * @details 在每个交易日结束时调用，执行交易日结束时的相关操作
	 *          该函数重写了WtEngine基类的on_session_end函数
	 */
	virtual void on_session_end() override;

public:
	/**
	 * @brief 获取委托队列切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准合约代码
	 * @param count 请求的数据条数
	 * @return WTSOrdQueSlice* 委托队列切片数据指针
	 * @details 获取指定合约的委托队列切片数据，用于策略分析
	 *          返回的数据需要由调用者负责释放
	 */
	WTSOrdQueSlice* get_order_queue_slice(uint32_t sid, const char* stdCode, uint32_t count);

	/**
	 * @brief 获取委托明细切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准合约代码
	 * @param count 请求的数据条数
	 * @return WTSOrdDtlSlice* 委托明细切片数据指针
	 * @details 获取指定合约的委托明细切片数据，用于策略分析
	 *          返回的数据需要由调用者负责释放
	 */
	WTSOrdDtlSlice* get_order_detail_slice(uint32_t sid, const char* stdCode, uint32_t count);

	/**
	 * @brief 获取成交明细切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准合约代码
	 * @param count 请求的数据条数
	 * @return WTSTransSlice* 成交明细切片数据指针
	 * @details 获取指定合约的成交明细切片数据，用于策略分析
	 *          返回的数据需要由调用者负责释放
	 */
	WTSTransSlice* get_transaction_slice(uint32_t sid, const char* stdCode, uint32_t count);

public:
	/**
	 * @brief 分钟结束回调函数
	 * @param curDate 当前日期，格式YYYYMMDD
	 * @param curTime 当前时间，格式HHMMSS或HHMMSS000
	 * @details 在每分钟结束时调用，执行分钟结束时的相关操作
	 *          包括通知策略上下文处理分钟结束事件
	 */
	void on_minute_end(uint32_t curDate, uint32_t curTime);

	/**
	 * @brief 添加高频策略上下文
	 * @param ctx 高频策略上下文指针
	 * @details 将高频策略上下文添加到引擎中进行管理
	 *          添加后的策略上下文将能够接收市场数据并执行交易
	 */
	void addContext(HftContextPtr ctx);

	/**
	 * @brief 获取高频策略上下文
	 * @param id 策略ID
	 * @return HftContextPtr 高频策略上下文指针
	 * @details 根据策略ID获取对应的高频策略上下文
	 *          如果策略ID不存在，则返回空指针
	 */
	HftContextPtr	getContext(uint32_t id);

	/**
	 * @brief 订阅委托队列数据
	 * @param sid 策略ID
	 * @param stdCode 标准合约代码
	 * @details 为指定策略订阅指定合约的委托队列数据
	 *          订阅后的数据将通过策略上下文回调函数推送给策略
	 */
	void sub_order_queue(uint32_t sid, const char* stdCode);

	/**
	 * @brief 订阅委托明细数据
	 * @param sid 策略ID
	 * @param stdCode 标准合约代码
	 * @details 为指定策略订阅指定合约的委托明细数据
	 *          订阅后的数据将通过策略上下文回调函数推送给策略
	 */
	void sub_order_detail(uint32_t sid, const char* stdCode);

	/**
	 * @brief 订阅成交明细数据
	 * @param sid 策略ID
	 * @param stdCode 标准合约代码
	 * @details 为指定策略订阅指定合约的成交明细数据
	 *          订阅后的数据将通过策略上下文回调函数推送给策略
	 */
	void sub_transaction(uint32_t sid, const char* stdCode);

private:
	/**
	 * @brief 策略上下文映射类型定义
	 * @details 用于存储策略ID和策略上下文的映射关系
	 */
	typedef wt_hashmap<uint32_t, HftContextPtr> ContextMap;
	
	/**
	 * @brief 策略上下文映射表
	 * @details 存储所有策略ID和策略上下文的映射关系
	 */
	ContextMap		_ctx_map;

	/**
	 * @brief 高频实时定时器
	 * @details 用于处理高频交易引擎的定时任务，如分钟结束事件
	 */
	WtHftRtTicker*	_tm_ticker;
	
	/**
	 * @brief 引擎配置
	 * @details 存储高频交易引擎的配置参数
	 */
	WTSVariant*		_cfg;

	/**
	 * @brief 委托队列订阅表
	 * @details 存储策略对委托队列数据的订阅关系
	 */
	StraSubMap		_ordque_sub_map;	//委托队列订阅表
	
	/**
	 * @brief 委托明细订阅表
	 * @details 存储策略对委托明细数据的订阅关系
	 */
	StraSubMap		_orddtl_sub_map;	//委托明细订阅表
	
	/**
	 * @brief 成交明细订阅表
	 * @details 存储策略对成交明细数据的订阅关系
	 */
	StraSubMap		_trans_sub_map;		//成交明细订阅表
};

NS_WTP_END