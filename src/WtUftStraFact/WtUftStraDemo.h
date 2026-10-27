/**
 * @file WtUftStraDemo.h
 * @brief UFT策略演示类头文件
 * @author Wesley
 * @date 未指定
 * 
 * @details 定义了WtUftStraDemo类，该类实现了一个简单的UFT策略示例
 *          包括订单管理、通道状态监控、参数动态更新等功能
 */

#pragma once
#include <unordered_set>
#include <memory>
#include <thread>
#include <mutex>

#include "../Includes/UftStrategyDefs.h"
#include "../Share/SpinMutex.hpp"

/**
 * @brief UFT策略演示类
 * @details 实现了一个简单的UFT策略样例，演示了基于理论价格和实际价格
 *          差异的交易选择逻辑，并包含订单超时管理功能
 */
class WtUftStraDemo : public UftStrategy
{
public:
	/**
	 * @brief 构造函数
	 * @param id 策略实例ID
	 * @details 初始化策略实例并设置默认参数
	 */
	WtUftStraDemo(const char* id);

	/**
	 * @brief 析构函数
	 * @details 清理策略实例的资源
	 */
	~WtUftStraDemo();

private:
	/**
	 * @brief 检查订单状态
	 * @details 检查订单是否超时，如果超过设定时间未成交，则自动撤销
	 */
	void	check_orders();

public:
	/**
	 * @brief 获取策略名称
	 * @return 策略名称字符串
	 * @details 返回策略的唯一标识名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 获取策略所属工厂名称
	 * @return 工厂名称字符串
	 * @details 返回创建该策略的工厂名称，用于策略与工厂的关联
	 */
	virtual const char* getFactName() override;

	/**
	 * @brief 初始化策略
	 * @param cfg 策略配置
	 * @return 初始化是否成功
	 * @details 从配置中读取策略所需的参数，包括交易品种代码、交易时间参数和交易数量等
	 */
	virtual bool init(WTSVariant* cfg) override;

	/**
	 * @brief 策略初始化回调
	 * @param ctx 策略上下文
	 * @details 在策略引擎启动时调用，用于设置参数监视、订阅数据等初始化操作
	 */
	virtual void on_init(IUftStraCtx* ctx) override;

	/**
	 * @brief Tick数据回调
	 * @param ctx 策略上下文
	 * @param code 合约代码
	 * @param newTick 最新Tick数据
	 * @details 在收到新的Tick数据时调用，是策略交易逻辑的核心实现部分
	 */
	virtual void on_tick(IUftStraCtx* ctx, const char* code, WTSTickData* newTick) override;

	/**
	 * @brief K线数据回调
	 * @param ctx 策略上下文
	 * @param code 合约代码
	 * @param period 周期标识符
	 * @param times 周期倦数
	 * @param newBar 最新K线数据
	 * @details 在收到新的K线数据时调用，当前示例策略中未实现具体逻辑
	 */
	virtual void on_bar(IUftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 成交回调
	 * @param ctx 策略上下文
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param isLong 是否为多头方向
	 * @param offset 开平仓标记
	 * @param qty 成交数量
	 * @param price 成交价格
	 * @details 在有成交发生时调用，当前示例策略中未实现具体逻辑
	 */
	virtual void on_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double qty, double price) override;

	/**
	 * @brief 持仓变更回调
	 * @param ctx 策略上下文
	 * @param stdCode 合约代码
	 * @param isLong 是否为多头方向
	 * @param prevol 前期总持仓
	 * @param preavail 前期可用持仓
	 * @param newvol 新的总持仓
	 * @param newavail 新的可用持仓
	 * @details 在持仓变更时调用，记录前一交易日持仓并输出日志
	 */
	virtual void on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail) override;

	/**
	 * @brief 订单状态回调
	 * @param ctx 策略上下文
	 * @param localid 本地订单ID
	 * @param stdCode 合约代码
	 * @param isLong 是否为多头方向
	 * @param offset 开平仓标记
	 * @param totalQty 总数量
	 * @param leftQty 剩余数量
	 * @param price 价格
	 * @param isCanceled 是否已撤销
	 * @details 在订单状态变更时调用，用于管理内部订单集合，当订单完成或撤销时从集合中移除
	 */
	virtual void on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled) override;

	/**
	 * @brief 交易通道就绪回调
	 * @param ctx 策略上下文
	 * @details 在交易通道准备就绪时调用，处理未完成订单并标记通道就绪状态
	 */
	virtual void on_channel_ready(IUftStraCtx* ctx) override;

	/**
	 * @brief 交易通道中断回调
	 * @param ctx 策略上下文
	 * @details 在交易通道中断时调用，标记通道为非就绪状态
	 */
	virtual void on_channel_lost(IUftStraCtx* ctx) override;

	/**
	 * @brief 策略参数更新回调
	 * @details 在策略参数变更时调用，从上下文读取最新的参数值并应用到策略中
	 */
	virtual void on_params_updated() override;

	/**
	 * @brief 委托回调
	 * @param localid 本地订单ID
	 * @param bSuccess 委托是否成功
	 * @param message 委托消息
	 * @details 在委托发送后回调，如果委托失败则从内部订单集合中移除该委托
	 */
	virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message) override;

private:
	/// @brief 最后一个Tick数据
	WTSTickData*	_last_tick;
	/// @brief 策略上下文指针
	IUftStraCtx*	_ctx;
	/// @brief 交易品种代码
	std::string		_code;
	/// @brief 检查订单超时的秒数
	uint32_t		_secs;
	/// @brief 交易频率控制，以毫秒为单位
	uint32_t		_freq;
	/// @brief 下单价格偏移跳数
	int32_t			_offset;
	/// @brief 交易手数
	double			_lots;
	/// @brief 前一交易日持仓
	double			_prev;

	/// @brief 订单ID集合类型定义
	typedef std::unordered_set<uint32_t> IDSet;
	/// @brief 当前活跃订单ID集合
	IDSet			_orders;
	/// @brief 订单集合访问互斥锁
	SpinMutex		_mtx_ords;

	/// @brief 最后一次入场时间
	uint64_t		_last_entry_time;

	/// @brief 交易通道是否就绪
	bool			_channel_ready;
	/// @brief 最后一次计算时间（分钟）
	uint32_t		_last_calc_time;
	/// @brief 撤单计数器
	uint32_t		_cancel_cnt;
};

