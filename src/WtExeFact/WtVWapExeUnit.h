#pragma once
/**
 * @file WtVWapExeUnit.h
 * @brief VWAP(成交量加权平均价格)执行单元定义
 * @details 实现基于VWAP算法的智能交易执行单元，通过预测成交量分布来优化订单执行
 * @author zhaoyv
 * @date 2023-05-23
 */
#include "WtOrdMon.h"
#include "../Includes/ExecuteDefs.h"
#include "../Share/StdUtils.hpp"
#include <fstream>
#include "rapidjson/document.h"
USING_NS_WTP;

/**
 * @brief VWAP(成交量加权平均价格)执行单元
 * @details 基于VWAP算法的智能交易执行单元，通过预测成交量分布
 *          将大单拆分成多个小单分时段执行，以实现接近VWAP价格的执行效果
 *          主要特点是根据历史成交量分布模式来预测未来成交量，从而优化订单执行
 */
class WtVWapExeUnit : public ExecuteUnit {

public:
	/**
	 * @brief 构造函数
	 * @details 初始化VWAP执行单元对象，设置默认参数和初始状态
	 */
	WtVWapExeUnit();

	/**
	 * @brief 析构函数
	 * @details 清理VWAP执行单元对象的资源，释放相关内存
	 */
	virtual ~WtVWapExeUnit();

private:
	/**
	 * @brief 执行仓位计算和发单操作
	 * @details 根据VWAP算法计算当前时间点应该执行的仓位量，并触发相应的交易操作
	 *          包括计算当前时间点的目标仓位、生成订单、管理订单状态等
	 */
	void	do_calc();

	/**
	 * @brief 立即发单
	 * @details 根据当前市场状况和配置的价格模式立即发送指定数量的订单
	 * @param qty 要发送的订单数量，正数表示买入，负数表示卖出
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
	 * @details 根据提供的执行上下文、合约代码和配置参数初始化VWAP执行单元
	 *          设置各种交易参数如执行时间、价格模式、发单间隔等
	 *          并加载历史VWAP成交量分布数据用于预测
	 * @param ctx 执行单元运行环境，提供交易接口和市场数据
	 * @param stdCode 标准化合约代码，指定要交易的合约
	 * @param cfg 配置参数，包含执行单元的各种设置
	 */
	virtual void init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override;

	/**
	 * @brief 处理订单回报
	 * @details 处理订单状态变化，包括成交、撤销等情况，并更新内部订单管理状态
	 *          如果订单被撤销且目标仓位未达到，则会重新发送订单
	 *          在VWAP执行策略中，这个方法还会跟踪当前的成交进度以调整执行计划
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
	 *          包括首次行情处理、交易时间校验、计算VWAP成交量分布、超时撤单等
	 *          这是VWAP执行单元的核心方法，负责根据当前市场状况调整执行计划
	 * @param newTick 新的行情数据指针
	 */
	virtual void on_tick(WTSTickData* newTick) override;

	/**
	 * @brief 处理成交回报
	 * @details 当收到成交回报时调用，更新内部成交状态和VWAP执行进度
	 *          在VWAP执行单元中，成交回报用于跟踪实际成交情况并调整后续执行计划
	 *          同时计算当前实际的VWAP价格与目标价格的偏差
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
	 *          如果委托失败，会从订单监控器中移除该订单并重新计算VWAP执行计划
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托回报消息
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 设置目标仓位
	 * @details 设置执行单元的目标仓位，并触发VWAP仓位计算和发单操作
	 *          当目标仓位与当前目标仓位不同时，重置发单次数并重新计算VWAP执行计划
	 *          这是触发VWAP执行策略的入口点
	 * @param stdCode 标准化合约代码
	 * @param newVol 新的目标仓位，可以使用DBL_MAX表示清仓
	 */
	virtual void set_position(const char* stdCode, double newVol) override;

	/**
	 * @brief 处理交易通道就绪
	 * @details 当交易通道就绪时调用，设置内部通道状态标记并触发VWAP仓位计算
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
	/** @brief 上一笔行情数据指针 */
	WTSTickData* _last_tick;
	/** @brief 目标仓位，正数表示多头，负数表示空头 */
	double		_target_pos;
	/** @brief 交易通道是否就绪标志 */
	bool		_channel_ready;
	/** @brief 计算锁，用于保护do_calc方法的并发调用 */
	StdUniqueMutex	_mtx_calc;

	/** @brief 品种信息指针，包含合约的交易规则和参数 */
	WTSCommodityInfo* _comm_info;
	/** @brief 交易时间信息指针，包含交易时段和休市信息 */
	WTSSessionInfo*	_sess_info;
	/** @brief 撤单次数统计 */
	uint32_t	_cancel_times;



	//////////////////////////////////////////////////////////////////////////
	/** @name 执行参数
	 * @{ */
	/** @brief 订单监控器，管理所有发出的订单状态 */
	WtOrdMon		_orders_mon;
	/** @brief 撤单计数器，记录当前执行周期内的撤单次数 */
	uint32_t		_cancel_cnt;
	/** @brief VWAP目标数组，按分钟记录的目标VWAP预测总报单量 */
	vector<double>	VwapAim;
	/** @} */
	//////////////////////////////////////////////////////////////////////////
	/** @name 配置参数
	 * @{ */
	/** @brief 执行总时间，单位秒，指定完成整个交易的总时间范围 */
	uint32_t		_total_secs;
	/** @brief 总执行次数，指定将订单分拆为多少次执行 */
	uint32_t		_total_times;
	/** @brief 执行尾部时间，单位秒，用于调整最后阶段的执行策略 */
	uint32_t		_tail_secs;
	/** @brief 挂单时限，单位秒，指定订单挂单后多长时间未成交则撤单 */
	uint32_t		_ord_sticky;
	/** @brief 价格模式: 0-最新价, 1-最优价, 2-对手价，决定发单价格的计算方式 */
	uint32_t		_price_mode;
	/** @brief 挂单价格偏移，相对于基准价格的偏移量，买入时为正，卖出时为负 */
	uint32_t		_price_offset;
	/** @brief 开始时间，格式为HHMM，如1000表示10:00 */
	uint32_t        _begin_time;
	/** @brief 结束时间，格式为HHMM，如1030表示10:30 */
	uint32_t		_end_time;
	/** @brief 最小开仓数量，当需要交易的数量小于此值时不发单 */
	double			_min_open_lots;
	/** @brief 单次发单手数，指定每次发单的标准数量 */
	double			_order_lots;
	/** @brief 是否允许撤单标志 */
	bool			isCanCancel;
	/** @} */
	//////////////////////////////////////////////////////////////////////////
	/** @name 临时变量
	 * @{ */
	/** @brief 本轮目标仓位，当前时间点应该达到的仓位量 */
	double			_this_target;
	/** @brief 发单间隔，单位毫秒，根据总执行时间和次数计算得出 */
	uint32_t		_fire_span;
	/** @brief 已执行次数，记录当前执行周期内已经发送的订单次数 */
	uint32_t		_fired_times;
	/** @brief 上次执行的时间戳，单位毫秒，用于计算下次发单时间 */
	uint64_t		_last_fire_time;
	/** @brief 上个下单时间戳，单位毫秒，用于计算订单是否超时需要撤单 */
	uint64_t		_last_place_time;
	/** @brief 上个行情数据的时间戳，单位毫秒，用于判断行情数据是否有效 */
	uint64_t		_last_tick_time;
	/** @brief VWAP单位时间下单量，根据VWAP算法计算的每个时间单元应发送的订单量 */
	double			_Vwap_vol;
	/** @brief VWAP价格，当前计算得到的成交量加权平均价格 */
	double			_Vwap_prz;
	/** @} */

	/** @brief 计算锁标志，用于防止do_calc方法被并发调用 */
	std::atomic<bool> _in_calc;

	/**
	 * @brief 计算锁标志结构体，实现RAII模式的原子锁
	 * @details 使用RAII模式实现的原子锁，在构造时获取锁，在析构时自动释放锁
	 *          主要用于保护do_calc方法的并发调用，确保同一时刻只有一个线程在执行计算
	 */
	typedef struct _CalcFlag
	{
		/** @brief 获取锁的结果，true表示锁已被占用，false表示成功获取锁 */
		bool _result;
		/** @brief 原子锁指针，指向要操作的原子变量 */
		std::atomic<bool>* _flag;

		/**
		 * @brief 构造函数，尝试获取锁
		 * @param flag 原子锁指针
		 */
		_CalcFlag(std::atomic<bool>*flag) :_flag(flag) {
			_result = _flag->exchange(true, std::memory_order_acq_rel);
		}

		/**
		 * @brief 析构函数，自动释放锁
		 */
		~_CalcFlag() {
			if (_flag)
				_flag->exchange(false, std::memory_order_acq_rel);
		}

		/**
		 * @brief 转换为bool类型的操作符重载
		 * @return 获取锁的结果，true表示锁已被占用，false表示成功获取锁
		 */
		operator bool() const { return _result; }
	}CalcFlag;
};