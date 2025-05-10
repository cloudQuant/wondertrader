/*!
 * @file WtHftStraDemo.h
 * @author wondertrader
 *
 * @brief 高频交易策略示例类定义
 * @details 演示了一个基于理论价格和市场实际价格差异的简单高频交易策略
 */

#pragma once
#include <unordered_set>
#include <memory>
#include <thread>
#include <mutex>

#include "../Includes/HftStrategyDefs.h"

/**
 * @brief 高频交易策略示例类
 * @details 继承自HftStrategy基类，实现了一个简单的高频交易策略，
 *          通过比较理论价格和市场实际价格生成交易信号
 */
class WtHftStraDemo : public HftStrategy
{
public:
	/**
	 * @brief 构造函数
	 * @param id 策略ID
	 * @details 创建高频交易策略实例，并初始化成员变量
	 */
	WtHftStraDemo(const char* id);

	/**
	 * @brief 析构函数
	 * @details 清理策略资源，释放行情数据
	 */
	~WtHftStraDemo();

private:
	/**
	 * @brief 检查订单状态
	 * @details 检查策略当前未完成订单，如果超过指定时间未成交则撤单
	 */
	void	check_orders();

	/**
	 * @brief 执行策略计算
	 * @param ctx 高频策略上下文
	 * @details 基于理论价格和实际价格比较计算交易信号并执行交易
	 */
	void	do_calc(IHftStraCtx* ctx);

public:
	/**
	 * @brief 获取策略名称
	 * @return 策略名称
	 * @details 返回策略的唯一标识名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 获取所属工厂名称
	 * @return 工厂名称
	 * @details 返回创建当前策略的工厂名称
	 */
	virtual const char* getFactName() override;

	/**
	 * @brief 初始化策略
	 * @param cfg 策略配置
	 * @return 初始化是否成功
	 * @details 从配置对象中加载策略参数，如合约代码、秒数、频率等
	 */
	virtual bool init(WTSVariant* cfg) override;

	/**
	 * @brief 策略初始化回调
	 * @param ctx 策略上下文
	 * @details 在策略启动时调用，用于订阅数据、初始化资源等
	 */
	virtual void on_init(IHftStraCtx* ctx) override;

	/**
	 * @brief 行情数据回调
	 * @param ctx 策略上下文
	 * @param code 合约代码
	 * @param newTick 新的行情数据
	 * @details 收到新的行情数据时调用，执行策略逻辑
	 */
	virtual void on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick) override;

	/**
	 * @brief K线数据回调
	 * @param ctx 策略上下文
	 * @param code 合约代码
	 * @param period 周期标识
	 * @param times 周期倍数
	 * @param newBar 新的K线数据
	 * @details 收到新的K线数据时调用
	 */
	virtual void on_bar(IHftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 成交回调
	 * @param ctx 策略上下文
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param isBuy 是否买入
	 * @param qty 成交数量
	 * @param price 成交价格
	 * @param userTag 用户标签
	 * @details 成交发生时调用，更新策略状态并计算新的交易信号
	 */
	virtual void on_trade(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double qty, double price, const char* userTag) override;

	/**
	 * @brief 持仓更新回调
	 * @param ctx 策略上下文
	 * @param stdCode 合约代码
	 * @param isLong 是否多头仓位
	 * @param prevol 前持仓量
	 * @param preavail 前可用仓位
	 * @param newvol 新持仓量
	 * @param newavail 新可用仓位
	 * @details 持仓量变化时调用
	 */
	virtual void on_position(IHftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail) override;

	/**
	 * @brief 订单状态回调
	 * @param ctx 策略上下文
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param isBuy 是否买入
	 * @param totalQty 总数量
	 * @param leftQty 剩余数量
	 * @param price 价格
	 * @param isCanceled 是否已撤销
	 * @param userTag 用户标签
	 * @details 订单状态变化时调用，如成交、撤单等
	 */
	virtual void on_order(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag) override;

	/**
	 * @brief 交易通道就绪回调
	 * @param ctx 策略上下文
	 * @details 当交易通道就绪可用时调用，清理未完成的订单
	 */
	virtual void on_channel_ready(IHftStraCtx* ctx) override;

	/**
	 * @brief 交易通道断开回调
	 * @param ctx 策略上下文
	 * @details 当交易通道断开时调用，标记通道为不可用状态
	 */
	virtual void on_channel_lost(IHftStraCtx* ctx) override;

	/**
	 * @brief 委托回报回调
	 * @param localid 本地订单ID
	 * @param bSuccess 委托是否成功
	 * @param message 委托回报消息
	 * @param userTag 用户标签
	 * @details 委托发出后收到回报时调用
	 */
	virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message, const char* userTag) override;

private:
	WTSTickData*	_last_tick;     /**< 上一次的行情数据 */
	IHftStraCtx*	_ctx;           /**< 策略上下文指针 */
	std::string		_code;          /**< 交易合约代码 */
	uint32_t		_secs;          /**< 订单超时秒数 */
	uint32_t		_freq;          /**< 交易频率（毫秒） */
	int32_t			_offset;        /**< 下单价格与最新价偏移跳数 */
	uint32_t		_unit;          /**< 交易单位，股票为100，期货为1 */
	double			_reserved;      /**< 保留仓位数量 */
	bool			_stock;         /**< 是否为股票 */

	typedef std::unordered_set<uint32_t> IDSet;  /**< 订单ID集合类型定义 */
	IDSet			_orders;        /**< 当前未完成订单ID集合 */
	std::mutex		_mtx_ords;      /**< 订单集合互斥锁 */

	uint64_t		_last_entry_time; /**< 上次入场时间（微秒） */

	bool			_channel_ready;  /**< 交易通道是否就绪 */
	uint32_t		_last_calc_time; /**< 上次计算时间（分钟） */
	uint32_t		_cancel_cnt;     /**< 撤单计数器 */
};

