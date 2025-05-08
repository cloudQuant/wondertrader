/*!
 * \file WtMinImpactExeUnit.h
 * \brief 最小冲击执行单元头文件
 *
 * 本文件定义了最小冲击执行单元类，用于实现通过尽可能减小市场冲击的方式执行订单
 * 
 * \author Wesley
 * \date 2020/03/30
 */
#pragma once
#include "../Includes/ExecuteDefs.h"
#include "WtOrdMon.h"

USING_NS_WTP;

/**
 * @brief 最小冲击执行单元类
 * @details 通过控制下单策略、下单时机和下单量，尽可能减少对市场的冲击并实现目标仓位
 */
class WtMinImpactExeUnit : public ExecuteUnit
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化成员变量并设置默认参数
	 */
	WtMinImpactExeUnit();

	/**
	 * @brief 析构函数
	 * @details 释放上一个行情数据和商品信息
	 */
	virtual ~WtMinImpactExeUnit();

private:
	/**
	 * @brief 执行单元核心计算逻辑
	 * @details 基于当前市场状态和目标仓位计算交易信号，并发送交易订单
	 *          包含价格计算、数量控制、平仓判断和订单发送的全部逻辑
	 */
	void	do_calc();

public:
		/**
	 * @brief 获取所属执行器工厂名称
	 * @return 执行器工厂名称常量
	 */
	virtual const char* getFactName() override;

		/**
	 * @brief 获取执行单元名称
	 * @return 实现类的执行单元名称
	 */
	virtual const char* getName() override;

		/**
	 * @brief 初始化执行单元
	 * @details 从配置中读取参数并设置到执行单元中，获取合约信息和交易时间模板
	 * @param ctx 执行单元运行环境上下文
	 * @param stdCode 管理的合约代码
	 * @param cfg 执行单元配置参数，包含价格偏移、超时时间、价格模式等
	 */
	virtual void init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override;

		/**
	 * @brief 订单回报处理
	 * @details 处理订单状态变化的回调函数，更新订单监控器状态并在订单撤销时触发重新计算
	 * @param localid 本地订单号
	 * @param stdCode 合约代码
	 * @param isBuy 订单方向，true为买入，false为卖出
	 * @param leftover 剩余数量，当为0时表示订单已完全成交
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override;

		/**
	 * @brief 行情数据回调处理
	 * @details 处理新到的行情数据，更新内部状态并触发交易计算逻辑
	 * @param newTick 新的行情数据对象指针
	 */
	virtual void on_tick(WTSTickData* newTick) override;

		/**
	 * @brief 成交回报处理
	 * @details 处理成交信息，更新订单监控器状态和相关统计数据
	 * @param localid 本地订单号
	 * @param stdCode 合约代码
	 * @param isBuy 交易方向，true为买入，false为卖出
	 * @param vol 成交数量，始终为正值，通过isBuy确定方向
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

		/**
	 * @brief 委托下单结果回报处理
	 * @details 处理委托下单失败的情况，在下单失败时清理订单监控器状态
	 * @param localid 本地订单号
	 * @param stdCode 合约代码
	 * @param bSuccess 下单是否成功
	 * @param message 失败原因消息，当bSuccess为false时有效
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

		/**
	 * @brief 设置新的目标仓位
	 * @details 更新执行单元的目标仓位并触发交易计算，当目标仓位与当前仓位不同时会执行操作
	 * @param stdCode 合约代码
	 * @param newVol 新的目标仓位，当为DBL_MAX时表示清仓
	 */
	virtual void set_position(const char* stdCode, double newVol) override;

		/**
	 * @brief 清理全部持仓
	 * @details 将合约的所有持仓设置为清仓状态，并触发交易计算逻辑
	 * @param stdCode 合约代码
	 */
	virtual void clear_all_position(const char* stdCode) override;

		/**
	 * @brief 交易通道就绪回调
	 * @details 处理交易通道就绪的事件，检查未完成订单并触发交易计算逻辑
	 */
	virtual void on_channel_ready() override;

		/**
	 * @brief 交易通道丢失回调
	 * @details 处理交易通道断开的事件，清理订单监控状态
	 */
	virtual void on_channel_lost() override;

private:
	/// @brief 最近一笔行情数据
	WTSTickData* _last_tick;
	/// @brief 目标仓位
	double		_target_pos;
	/// @brief 计算线程互斥锁
	StdUniqueMutex	_mtx_calc;

	/// @brief 商品/合约信息
	WTSCommodityInfo*	_comm_info;
	/// @brief 交易时间模板信息
	WTSSessionInfo*		_sess_info;

	//////////////////////////////////////////////////////////////////////////
	// 执行参数
	/// @brief 价格偏移跳数，正值为加价，负值为减价
	int32_t		_price_offset;
	/// @brief 订单超时秒数，超过该时间后将撤单
	uint32_t	_expire_secs;
	/// @brief 价格模式：-1最优价，0最新价，1对手价，2自动价
	int32_t		_price_mode;
	/// @brief 发单时间间隔，单位毫秒
	uint32_t	_entrust_span; 
	/// @brief 是否按比例下单，如果true则使用_qty_rate，如果false则使用_order_lots
	bool		_by_rate;
	/// @brief 单次发单手数，当_by_rate为false时生效
	double		_order_lots;
	/// @brief 下单数量比例，当_by_rate为true时生效
	double		_qty_rate;

	/**
	 * @brief 最小开仓数量
	 * @details By Wesley @ 2022.12.15
	 *          最小开仓数量用于限制单笔开仓数量的下限
	 *          没有设置最小平仓数量的原因是平仓要根据持仓来定，无法限制
	 */
	double		_min_open_lots;

	/// @brief 订单监控器
	WtOrdMon	_orders_mon;
	/// @brief 在途撤单数量
	uint32_t	_cancel_cnt;
	/// @brief 撤单次数，用于调整价格偏移
	uint32_t	_cancel_times;

	/// @brief 上一次下单时间，用于控制发单频率
	uint64_t	_last_place_time;
	/// @brief 上一个Tick时间
	uint64_t	_last_tick_time;

	/// @brief 是否正在计算中的原子布尔变量
	std::atomic<bool>	_in_calc;

		/**
	 * @brief 计算标志结构体
	 * @details 使用RAII模式的结构体，用于管理计算状态的原子锁定和释放
	 *          在构造时尝试获取锁，在析构时自动释放锁
	 */
	typedef struct _CalcFlag
	{
		/// @brief 锁获取的结果，true表示已经被占用，false表示获取成功
		bool				_result;
		/// @brief 原子布尔标志指针
		std::atomic<bool>*	_flag;

		/**
		 * @brief 构造函数
		 * @param flag 原子布尔标志指针
		 */
		_CalcFlag(std::atomic<bool>* flag) :_flag(flag)
		{
			_result = _flag->exchange(true, std::memory_order_acq_rel);
		}

		/**
		 * @brief 析构函数，自动释放锁
		 */
		~_CalcFlag()
		{
			if (_flag)
				_flag->exchange(false, std::memory_order_acq_rel);
		}

		/**
		 * @brief 布尔操作符重载，用于判断锁是否获取成功
		 * @return 返回锁获取结果，true表示已被占用，false表示获取成功
		 */
		operator bool() const { return _result; }
	} CalcFlag;
};

