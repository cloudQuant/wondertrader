/*!
 * \file WtDiffMinImpactExeUnit.h
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 差量最小冲击执行单元头文件
 * \details 本文件定义了WonderTrader的差量最小冲击执行单元类，该执行单元通过对交易时机、
 *          价格、数量的控制，实现对市场影响最小化的订单执行策略，适用于大单交易场景
 */
#pragma once
#include "../Includes/ExecuteDefs.h"
#include "WtOrdMon.h"

USING_NS_WTP;

/**
 * @brief 差量最小冲击执行单元
 * @details 通过控制交易时机、价格和数量，实现对市场影响最小化的订单执行策略
 * 差量执行单元根据目标仓位与当前仓位的差值，逐笔分批执行订单，并可以自适应市场状况调整执行价格
 */
class WtDiffMinImpactExeUnit : public ExecuteUnit
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化差量最小冲击执行单元的成员变量
	 */
	WtDiffMinImpactExeUnit();

	/**
	 * @brief 析构函数
	 * @details 清理差量最小冲击执行单元的资源
	 */
	virtual ~WtDiffMinImpactExeUnit();

private:
	/**
	 * @brief 执行差量交易的核心计算逻辑
	 * @details 根据差量、市场状态和配置参数计算并发出订单，
	 *          包含买卖方向判断、交易数量计算、交易价格计算等逻辑
	 */
	void	do_calc();

public:
	/**
	 * @brief 获取所属执行器工厂名称
	 * @details 返回差量最小冲击执行单元所属的执行器工厂名称
	 * @return 执行器工厂名称
	 */
	virtual const char* getFactName() override;

	/**
	 * @brief 获取执行单元名称
	 * @details 返回差量最小冲击执行单元的名称
	 * @return 执行单元名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 初始化执行单元
	 * @details 根据给定的配置初始化差量最小冲击执行单元，设置交易参数等
	 * @param ctx 执行单元运行环境，提供执行单元所需的基础设施和接口
	 * @param stdCode 管理的合约代码
	 * @param cfg 执行单元配置，包含价格模式、过期时间等参数
	 */
	virtual void init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override;

	/**
	 * @brief 订单回报处理
	 * @details 处理从交易平台返回的订单状态变化信息，包括撤单、成交等
	 * @param localid 本地订单号，用于识别和跟踪订单
	 * @param stdCode 合约代码
	 * @param isBuy 买卖方向，true表示买入，false表示卖出
	 * @param leftover 剩余未成交数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销，true表示已撤销，false表示未撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override;

	/**
	 * @brief 行情数据回调处理
	 * @details 处理最新的行情数据，更新内部状态并触发核心交易逻辑
	 * @param newTick 最新的tick行情数据，包含最新价、买卖盟、成交量等信息
	 */
	virtual void on_tick(WTSTickData* newTick) override;

	/**
	 * @brief 成交回报处理
	 * @details 处理订单成交信息，更新差量仓位信息
	 * @param localid 本地订单号
	 * @param stdCode 合约代码
	 * @param isBuy 买卖方向，true表示买入，false表示卖出
	 * @param vol 成交数量，这里没有正负区分，通过isBuy确定买入还是卖出
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/**
	 * @brief 委托单回报处理
	 * @details 处理委托下单的结果回报，判断是否成功
	 * @param localid 本地订单号
	 * @param stdCode 合约代码
	 * @param bSuccess 下单是否成功，true表示成功，false表示失败
	 * @param message 错误消息，当下单失败时包含具体原因
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 设置新的目标仓位
	 * @details 设置合约的新目标仓位并计算差量，触发执行计算
	 * @param stdCode 合约代码
	 * @param newVol 新的目标仓位数量，正数表示多头持仓，负数表示空头持仓
	 */
	virtual void set_position(const char* stdCode, double newVol) override;

	/**
	 * @brief 清理全部持仓
	 * @details 将指定合约的目标仓位设置为零，触发平仓操作
	 * @param stdCode 合约代码
	 */
	virtual void clear_all_position(const char* stdCode) override;

	/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道连接就绪时触发，检查未完成订单并重新计算
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 交易通道丢失回调
	 * @details 当交易通道断开连接时触发，处理断开连接后的逻辑
	 */
	virtual void on_channel_lost() override;

private:
	//! 最近的行情数据
	WTSTickData*	_last_tick;
	//! 未执行完的差量，正数表示待买入，负数表示待卖出
	double			_left_diff;
	//! 计算互斥锁，防止多线程同时执行计算逻辑
	StdUniqueMutex	_mtx_calc;

	//! 品种信息，包含合约属性、价格精度等
	WTSCommodityInfo*	_comm_info;
	//! 交易时段信息，用于判断是否在交易时段内
	WTSSessionInfo*		_sess_info;

	//////////////////////////////////////////////////////////////////////////
	//执行参数
	//! 价格偏移跳数，一般和订单同方向，用于设置委托价格
	int32_t		_price_offset;
	//! 订单超时秒数，超过该时间未成交的订单会被撤销
	uint32_t	_expire_secs;
	//! 价格类型：0-最新价, -1-最优价, 1-对手价, 2-自动
	int32_t		_price_mode;
	//! 发单时间间隔，单位毫秒，控制发单频率
	uint32_t	_entrust_span;
	//! 是否按照对手挂单数的比例下单，true则使用rate字段，false则使用lots字段
	bool		_by_rate;
	//! 单次发单手数，当by_rate=false时使用
	double		_order_lots;
	//! 下单手数比例，当by_rate=true时，按对手盘挂单量的比例计算下单量
	double		_qty_rate;

	//! 订单监控器，用于管理本地订单
	WtOrdMon	_orders_mon;
	//! 当前正在撤销的订单数量
	uint32_t	_cancel_cnt;
	//! 撤单次数，用于动态调整下单价格
	uint32_t	_cancel_times;

	//! 上次下单时间，用于控制下单频率
	uint64_t	_last_place_time;
	//! 上次行情时间戳，用于判断行情是否更新
	uint64_t	_last_tick_time;

	//! 是否正在计算中的标志，防止重复计算
	std::atomic<bool>	_in_calc;

	/**
	 * @brief 计算标志结构体
	 * @details 用于防止多线程同时执行计算逻辑的RAII类型标志结构体
	 * 在构造时尝试获取计算标志，在析构时自动释放标志
	 */
	typedef struct _CalcFlag
	{
		//! 获取标志的结果，true表示已有其他线程正在计算
		bool				_result;
		//! 指向原子布尔标志的指针
		std::atomic<bool>*	_flag;

		/**
		 * @brief 构造函数
		 * @param flag 原子布尔标志指针
		 */
		_CalcFlag(std::atomic<bool>* flag):_flag(flag)
		{
			_result = _flag->exchange(true, std::memory_order_acq_rel);
		}

		/**
		 * @brief 析构函数，自动释放标志
		 */
		~_CalcFlag()
		{
			if(_flag)
				_flag->exchange(false, std::memory_order_acq_rel);
		}

		/**
		 * @brief 布尔操作符重载
		 * @return 是否已有其他线程正在计算
		 */
		operator bool() const { return _result; }
	} CalcFlag;
};

