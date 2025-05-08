/*!
 * \file WtStockMinImpactExeUnit.h
 * \brief 股票最小冲击执行单元头文件
 *
 * 本文件定义了适用于股票交易的最小冲击算法的执行单元类，
 * 该类用于最小化市场冲击的方式执行股票交易，支持按股数、金额或比例三种方式
 * 指定目标仓位
 *
 * \author Wesley
 * \date 2020/03/30
 */
#pragma once
#include "../Includes/ExecuteDefs.h"
#include "WtOrdMon.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"

USING_NS_WTP;

/**
 * @brief 下单价格模式常量定义
 */
#define BESTPX -1 ///< 己方最优价，买入时使用买一价，卖出时使用卖一价
#define LASTPX 0  ///< 最新成交价
#define MARKET 1  ///< 对手价，买入时使用卖一价，卖出时使用买一价
#define AUTOPX 2  ///< 自动模式，根据市场深度自动判断使用对手价或己方价

/**
 * @brief 仓位枚举回调函数类型
 * @details 用于枚举并回调各交易通道的仓位信息
 * @param const char* 标准化合约代码
 * @param bool 是否为多仓（买入）
 * @param double 当前仓位
 * @param double 可用仓位
 * @param double 占用保证金
 * @param double 浮动盈亏
 */
typedef std::function<void(const char*, bool, double, double, double, double)> FuncEnumChnlPosCallBack;

/**
 * @brief 股票最小冲击执行单元类
 * @details 适用于股票交易的最小冲击算法的执行单元类，支持按股数、金额或比例指定目标仓位
 * 通过智能划分订单并选择合适的价格来最小化交易冲击成本
 */
class WtStockMinImpactExeUnit : public ExecuteUnit
{
private:
	/** @brief 可转换债券产品代码 */
	const char* cbondStr = "CBOND";
	/** @brief 股票产品代码 */
	const char* stockStr = "STK";

		/**
	 * @brief 目标仓位模式枚举
	 * @details 定义了目标仓位的指定方式
	 */
	enum class TargetMode
	{
		stocks = 0,  ///< 使用股数指定目标仓位
		amount,      ///< 使用金额指定目标仓位
		ratio,       ///< 使用比例指定目标仓位
	};

private:
	/**
	 * @brief 价格模式名称数组
	 * @details 存储各种价格模式的名称，用于日志输出和调试
	 */
	std::vector<std::string> PriceModeNames = {
		"BESTPX",		///< 最优价
		"LASTPX",		///< 最新价
		"MARKET",		///< 对手价
		"AUTOPX"		///< 自动
	};

public:
	/**
	 * @brief 构造函数
	 * @details 初始化股票最小冲击执行单元的各内部变量
	 */
	WtStockMinImpactExeUnit();
	
	/**
	 * @brief 析构函数
	 * @details 释放内部资源，包括行情数据和合约信息
	 */
	virtual ~WtStockMinImpactExeUnit();

private:
	/**
	 * @brief 执行交易计算
	 * @details 根据当前目标仓位和实际仓位的差异，计算并生成相应的交易订单
	 */
	void	do_calc();
	
	/**
	 * @brief 检查是否为清仓状态
	 * @details 检查当前是否处于清仓状态，用于判断交易策略
	 * @return 如果为清仓状态返回true，否则返回false
	 */
	bool	is_clear();
public:
		/**
	 * @brief 获取所属执行器工厂名称
	 * @details 返回创建该执行单元的工厂名称
	 * @return 工厂名称字符串
	 */
	virtual const char* getFactName() override;

		/**
	 * @brief 获取执行单元名称
	 * @details 返回股票最小冲击执行单元的名称
	 * @return 执行单元名称字符串
	 */
	virtual const char* getName() override;

		/**
	 * @brief 初始化执行单元
	 * @details 根据输入的配置初始化股票最小冲击执行单元，设置各种执行参数
	 * @param ctx 执行单元运行的上下文环境
	 * @param stdCode 管理的标准化合约代码
	 * @param cfg 执行单元配置项
	 */
	virtual void init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override;

		/**
	 * @brief 订单状态回报处理
	 * @details 处理订单状态变更的回报，包括撤单、成交等情况
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入，true表示买入，false表示卖出
	 * @param leftover 剩余未成交数量
	 * @param price 订单委托价格
	 * @param isCanceled 是否已撤销，true表示已撤销，false表示未撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override;

		/**
	 * @brief 处理行情数据回调
	 * @details 接收并处理最新的市场行情数据，根据需要触发交易计算
	 * @param newTick 最新的市场行情数据
	 */
	virtual void on_tick(WTSTickData* newTick) override;

		/**
	 * @brief 成交回报处理
	 * @details 处理订单成交的回报信息，更新内部状态
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入，true表示买入，false表示卖出
	 * @param vol 成交数量，正值，不带方向性，方向由isBuy决定
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

		/**
	 * @brief 下单结果回报处理
	 * @details 处理下单成功或失败的回报信息
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 是否成功，true表示下单成功，false表示下单失败
	 * @param message 错误消息，当下单失败时包含错误原因
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

		/**
	 * @brief 设置新的目标仓位
	 * @details 更新目标仓位，根据目标模式不同有不同的处理适配股数、金额或比例
	 * @param stdCode 标准化合约代码
	 * @param newVol 新的目标仓位，可能是股数、金额或比例，取决于当前目标模式
	 */
	virtual void set_position(const char* stdCode, double newVol) override;

		/**
	 * @brief 清空全部仓位
	 * @details 将指定合约的目标仓位设置为清仓状态，触发清仓操作
	 * @param stdCode 标准化合约代码
	 */
	virtual void clear_all_position(const char* stdCode) override;

		/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道准备就绪可以交易时调用此方法，触发处理未管理订单等操作
	 */
	virtual void on_channel_ready() override;

		/**
	 * @brief 交易通道丢失回调
	 * @details 当交易通道断开连接或不可用时调用此方法，处理交易通道丢失的清理工作
	 */
	virtual void on_channel_lost() override;

		/**
	 * @brief 账户信息回调
	 * @details 接收并处理账户资金信息更新，更新内部账户状态
	 * @param currency 货币类型
	 * @param prebalance 前结余额
	 * @param balance 静态余额
	 * @param dynbalance 动态余额
	 * @param avaliable 可用资金
	 * @param closeprofit 平仓盈亏
	 * @param dynprofit 浮动盈亏
	 * @param margin 占用保证金
	 * @param fee 交易手续费
	 * @param deposit 入金
	 * @param withdraw 出金
	 */
	virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override;

private:
	/**
	 * @brief 检查未管理订单
	 * @details 检查并处理系统中存在的未被执行单元管理的订单，根据配置决定是否进行撤单
	 */
	void check_unmanager_order();

private:
	/** @brief 最新行情数据指针 */
	WTSTickData* _last_tick;
	/** @brief 目标仓位（股数） */
	double		_target_pos;
	/** @brief 目标金额 */
	double		_target_amount;
	/** @brief 目标持仓比例 */
	double		_target_ratio;

	/** @brief 账户可用资金 */
	double		_avaliable{ 0 };

	/** @brief 计算锁，用于保护交易计算过程的线程安全 */
	StdUniqueMutex	_mtx_calc;
	/** @brief 商品信息指针，包含合约相关参数 */
	WTSCommodityInfo* _comm_info;
	/** @brief 交易时段信息指针，包含交易时间相关配置 */
	WTSSessionInfo* _sess_info;

		//////////////////////////////////////////////////////////////////////////
	/// @name 执行参数
	/// @{
	/** @brief 价格偏移跳数，当使用对手价时的价格偏移量 */
	int32_t		_price_offset;
	/** @brief 订单超时秒数，当订单超过指定时间未成交则触发撤单 */
	uint32_t	_expire_secs;
	/** @brief 价格模式，对应BESTPX/LASTPX/MARKET/AUTOPX四种模式 */
	int32_t		_price_mode;
	/** @brief 发单时间间隔，单位毫秒，两次发单之间的最小时间间隔 */
	uint32_t	_entrust_span;
	/** @brief 是否按率下单，true表示按对手盘口数量比例下单，false表示按固定手数下单 */
	bool		_by_rate;
	/** @brief 单次发单手数，当by_rate为false时使用 */
	double		_order_lots;
	/** @brief 下单手数比例，当by_rate为true时使用，指定占对手盘口数量的比例 */
	double		_qty_rate;
	/** @brief 最小下单量，确保单次下单数量不小于该值 */
	double		_min_order;
	/** @brief 是否完成标志，表示当前订单执行是否完成 */
	bool		_is_finish;
	/** @brief 开始时间，记录执行开始的时间戳 */
	uint64_t	_start_time;
	/** @brief 开始价格，记录执行开始时的价格 */
	double		_start_price{ 0 };
	/** @brief 是否为第一个Tick标志 */
	bool		_is_first_tick{ true };
	/** @brief 最大撤单次数，如果超过这个次数仍然未撤单，则说明是错单 */
	double		_max_cancel_time{ 3 };
	/** @brief 总资金，用于计算持仓比例时的基数，-1表示使用账户实际资金 */
	double		_total_money{ -1 };
	/** @brief 是否允许T+0交易，对于转债等需要设置为true，股票默认为false */
	double		_is_t0{ false };
	/** @brief 撤单映射表，用于跟踪订单的撤单次数 */
	wt_hashmap< uint32_t, uint32_t > _cancel_map{};
	/// @}

		/** @brief 订单监控器，用于管理和跟踪订单状态 */
	WtOrdMon	_orders_mon;
	//uint32_t	_cancel_cnt;
	/** @brief 撤单次数计数器，记录当前订单被撤销的次数 */
	uint32_t	_cancel_times;
	/** @brief 是否撤销未管理订单标志，当通道准备就绪时是否撤销未被管理的订单 */
	bool		_is_cancel_unmanaged_order{ true };
	/** @brief 最近一次下单时间，用于控制下单频率 */
	uint64_t	_last_place_time;
	/** @brief 最近一次接收Tick数据的时间 */
	uint64_t	_last_tick_time;
	/** @brief 是否为清仓状态标志 */
	bool		_is_clear;
	/** @brief 目标仓位模式，指定目标仓位的方式（股数、金额或比例） */
	TargetMode  _target_mode{ TargetMode::stocks };
	/** @brief 是否为科创板股票标志 */
	bool		_is_KC{ false };
	/** @brief 最小交易单位，由合约类型决定（如股票100股或科创板200股） */
	double		_min_hands{ 0 };
	/** @brief 交易通道是否就绪标志 */
	bool		_is_ready{ false };
	/** @brief 总资金是否就绪标志，指示是否已配置了总资金 */
	bool		_is_total_money_ready{ false };
	/** @brief 市值表，记录各合约的市值信息 */
	std::map<std::string, double> _market_value{};
	/** @brief 当前时间戳，用于记录当前操作的时间 */
	uint64_t _now;

public:
	/**
	 * @brief 将股数调整为最小交易单位的整数倍
	 * @details 根据最小交易单位调整订单股数，采用四舍五入的方式
	 * @param hands 原始股数
	 * @param min_hands 最小交易单位（如股票的100股，科创板的200股等）
	 * @return 调整后的整数倍股数
	 */
	inline int round_hands(double hands, double min_hands)
	{
		return (int)((hands + min_hands / 2) / min_hands) * min_hands;
	}

		/**
	 * @brief 获取特定合约的最小交易单位
	 * @details 根据合约类型和代码规则自动识别并返回合适的最小交易单位
	 *          包括普通股票（100股）、科创板（200股）、可转债（10张）等
	 * @param stdCode 标准化合约代码
	 * @return 最小交易单位
	 */
	inline double get_minOrderQty(std::string stdCode)
	{
		int code = std::stoi(StrUtil::split(stdCode, ".")[2]);
		bool is_KC = false;
		if (code >= 688000)
		{
			is_KC = true;
		}
		WTSCommodityInfo* comm_info = _ctx->getCommodityInfo(stdCode.c_str());
		double min_order = 1.0;
		if (strcmp(comm_info->getProduct(), cbondStr) == 0)
			min_order = 10.0;
		else if (strcmp(comm_info->getProduct(), stockStr) == 0)
			if (is_KC)
				min_order = 200.0;
			else
				min_order = 100.0;
		if (comm_info)
			comm_info->release();
		return min_order;
	}
};

