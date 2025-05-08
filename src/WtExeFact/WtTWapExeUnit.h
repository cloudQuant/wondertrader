/**
 * @file WtTWapExeUnit.h
 * @brief TWAP执行单元定义
 * @details 定义了基于时间加权平均价格（Time Weighted Average Price）策略的执行单元
 *          该执行单元将交易指令分解为多个小单，并在设定的时间范围内平均执行
 *          以降低市场冲击和执行成本
 */
#pragma once
#include "WtOrdMon.h"
#include "../Includes/ExecuteDefs.h"
#include "../Share/StdUtils.hpp"

USING_NS_WTP;

/**
 * @brief TWAP执行单元类
 * @details 基于时间加权平均价格策略的执行单元实现
 *          将大单分解为多个小单，并在设定的时间范围内平均执行
 *          支持多种价格模式和订单管理策略，如超时撤单、打板收尾等
 */
class WtTWapExeUnit : public ExecuteUnit
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化TWAP执行单元对象并设置默认参数
	 */
	WtTWapExeUnit();

	/**
	 * @brief 析构函数
	 * @details 清理资源并释放内存
	 */
	virtual ~WtTWapExeUnit();

private:
	/**
	 * @brief 执行仓位计算和发单操作
	 * @details 根据目标仓位和当前实际仓位计算需要发送的订单数量和价格
	 *          包含订单撤销、价格计算、数量分配等逻辑
	 */
	void	do_calc();

	/**
	 * @brief 立即发单
	 * @details 根据给定的数量立即发送交易订单，包括计算委托价格、确定交易方向等
	 * @param qty 目标交易数量，正数表示买入，负数表示卖出
	 */
	void	fire_at_once(double qty);

public:
	/**
	 * @brief 获取所属执行器工厂名称
	 * @details 返回该执行单元所属的工厂名称，用于执行单元的注册和管理
	 * @return 工厂名称字符串
	 */
	virtual const char* getFactName() override;

	/**
	 * @brief 获取执行单元名称
	 * @details 返回该执行单元的名称，用于标识不同类型的执行单元
	 * @return 执行单元名称字符串
	 */
	virtual const char* getName() override;

	/**
	 * @brief 初始化执行单元
	 * @details 根据提供的执行上下文、合约代码和配置参数初始化执行单元
	 *          设置各种交易参数如执行时间、价格模式、发单间隔等
	 * @param ctx 执行单元运行环境，提供交易接口和市场数据
	 * @param stdCode 标准化合约代码，指定要交易的合约
	 * @param cfg 配置参数，包含执行单元的各种设置
	 */
	virtual void init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override;

	/**
	 * @brief 处理订单回报
	 * @details 处理订单状态变化，包括成交、撤销等情况，并更新内部订单管理状态
	 *          如果订单被撤销且目标仓位未达到，则会重新发送订单
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入订单
	 * @param leftover 剩余未成交数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override;

	/**
	 * @brief 处理行情数据回调
	 * @details 当收到新的行情数据时调用，更新内部行情缓存并触发相关的交易逻辑
	 *          包括首次行情处理、交易时间校验、计算目标仓位、超时撤单等
	 * @param newTick 新的行情数据指针
	 */
	virtual void on_tick(WTSTickData* newTick) override;

	/**
	 * @brief 处理成交回报
	 * @details 当收到成交回报时调用，当前实现中不在此处触发交易逻辑，而是在on_tick中处理
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入成交
	 * @param vol 成交数量，这里没有正负，通过isBuy确定买入还是卖出
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/**
	 * @brief 处理委托回报
	 * @details 当收到委托回报时调用，主要处理委托失败的情况
	 *          如果委托失败，会从订单监控器中移除该订单并重新计算
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托回报消息
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 设置目标仓位
	 * @details 设置执行单元的目标仓位，并触发仓位计算和发单操作
	 *          当目标仓位与当前目标仓位不同时，重置发单次数并触发do_calc方法
	 * @param stdCode 标准化合约代码
	 * @param newVol 新的目标仓位，可以使用DBL_MAX表示清仓
	 */
	virtual void set_position(const char* stdCode, double newVol) override;

	/**
	 * @brief 处理交易通道就绪
	 * @details 当交易通道就绪时调用，设置内部通道状态标记并触发仓位计算
	 *          如果有目标仓位且当前有有效的行情数据，则重新计算并发单
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 处理交易通道丢失
	 * @details 当交易通道断开时调用，重置内部通道状态标记
	 *          在当前实现中，仅将通道状态标记设置为关闭
	 */
	virtual void on_channel_lost() override;


private:
	/// @brief 最新的行情数据
	WTSTickData*	 _last_tick;

	/// @brief 目标仓位
	double			_target_pos;

	/// @brief 交易通道是否就绪
	bool			_channel_ready;

	/// @brief 计算锁，防止多线程同时执行计算
	StdUniqueMutex	_mtx_calc;

	/// @brief 商品信息，包含价格单位、最小变动单位等
	WTSCommodityInfo* _comm_info;

	/***---begin---23.5.18---zhaoyk***/
	/// @brief 交易时段信息
	WTSSessionInfo*	_sess_info;

	/// @brief 撤单次数，用于计算价格偏移
	uint32_t		_cancel_times;
	/***---end---23.5.18---zhaoyk***/


	//////////////////////////////////////////////////////////////////////////
	/// @brief 执行参数

	/// @brief 订单监控器，用于管理和跟踪所有发出的订单
	WtOrdMon		_orders_mon;

	/// @brief 当前正在撤销的订单数量
	uint32_t		_cancel_cnt;

	//////////////////////////////////////////////////////////////////////////
	/// @brief 参数

	/// @brief 执行总时间，单位秒，表示将交易指令分解到多长时间内执行
	uint32_t		_total_secs;

	/// @brief 总执行次数，表示将交易指令分解为多少个小单
	uint32_t		_total_times;

	/// @brief 执行尾部时间，单位秒，用于收尾阶段的特殊处理
	uint32_t		_tail_secs;

	/// @brief 挂单时限，单位秒，超过该时间的订单会被自动撤销
	uint32_t		_ord_sticky;

	/// @brief 价格模式: 0-最新价, 1-最优价(买入用买一价，卖出用卖一价), 2-对手价(买入用卖一价，卖出用买一价)
	uint32_t		_price_mode;

	/// @brief 挂单价格偏移，相对于基准价格的偏移量，买入时为正，卖出时为负
	uint32_t		_price_offset;

	/// @brief 开始时间，格式为HHMM，如1000表示10:00
	uint32_t        _begin_time;

	/// @brief 结束时间，格式为HHMM，如1030表示10:30
	uint32_t		_end_time;

	/// @brief 最小开仓数量，每次发单的最小数量
	double			_min_open_lots;

	/// @brief 单次发单手数，每次发单的标准数量
	double			_order_lots;

	/// @brief 订单是否可以被撤销，当订单价格被修正为涨跌停价格时为false
	bool			isCanCancel;
	//////////////////////////////////////////////////////////////////////////
	/// @brief 临时变量

	/// @brief 本轮目标仓位，当前执行轮次的目标仓位
	double			_this_target;

	/// @brief 发单间隔，单位毫秒，两次发单之间的最小时间间隔
	uint32_t		_fire_span;

	/// @brief 已执行次数，当前已经执行的发单次数
	uint32_t		_fired_times;

	/// @brief 上次发单时间，上一次执行发单操作的时间戳
	uint64_t		_last_fire_time;

	/// @brief 上次下单时间，上一次成功下单的时间戳
	uint64_t		_last_place_time;

	/// @brief 上次行情时间，上一次收到行情数据的时间戳
	uint64_t		_last_tick_time;

	/// @brief 计算锁标志，防止多线程同时执行计算
	std::atomic<bool> _in_calc;

	/**
	 * @brief 计算标记类，用于防止多线程同时执行计算
	 * @details 基于 RAII 模式的计算锁实现，构造时获取锁，析构时释放锁
	 *          可以直接用于条件判断，如果返回 true 表示已有其他线程正在计算
	 */
	typedef struct _CalcFlag
	{
		/// @brief 结果标志，如果为 true 表示已有其他线程正在计算
		bool _result;

		/// @brief 原子布尔指针，指向需要保护的标志
		std::atomic<bool>* _flag;

		/**
		 * @brief 构造函数，尝试获取计算锁
		 * @param flag 需要保护的原子布尔指针
		 */
		_CalcFlag(std::atomic<bool>*flag) :_flag(flag) {
			_result = _flag->exchange(true, std::memory_order_acq_rel);
		}

		/**
		 * @brief 析构函数，释放计算锁
		 */
		~_CalcFlag() {
			if (_flag)
				_flag->exchange(false, std::memory_order_acq_rel);
		}

		/**
		 * @brief 布尔转换操作符，允许将对象直接用于条件判断
		 * @return 如果为 true 表示已有其他线程正在计算
		 */
		operator bool() const { return _result; }
	}CalcFlag;
};

