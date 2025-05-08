#pragma once
/**
 * @file WtStockVWapExeUnit.h
 * @author zhaoyk (23.6.2)
 * @brief 股票VWAP执行单元头文件
 * @details 定义股票的成交量加权平均价(VWAP)执行单元，用于将大单拆分为多个小单并按时间分布执行，减少市场冲击
 */
#include "WtOrdMon.h"
#include "../Includes/ExecuteDefs.h"
#include "../Share/StrUtil.hpp"
#include <fstream>
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"
USING_NS_WTP;

/**
 * @brief 股票VWAP执行单元类
 * @details 实现了基于成交量加权平均价(VWAP)策略的股票订单执行单元
 *          根据预设的时间和数量分布，将大单拆分成多个小单，在指定时间区间内逐步执行
 *          最大程度地降低市场冲击，提高订单执行效率
 */
class WtStockVWapExeUnit : public ExecuteUnit {

private:
	/// @brief 可转债产品代码
	const char* cbondStr = "CBOND";
	/// @brief 股票产品代码
	const char* stockStr = "STK";

	/**
	 * @brief 目标仓位模式枚举
	 * @details 定义不同的目标仓位模式，包括股数、金额和比例三种
	 */
	enum class TargetMode
	{
		stocks = 0,  ///< 股数模式，目标仓位以股数为单位
		amount,      ///< 金额模式，目标仓位以金额为单位
		ratio,       ///< 比例模式，目标仓位以比例为单位
	};

private:
	/**
	 * @brief 价格模式名称列表
	 * @details 定义不同价格模式的名称，用于日志输出和文档显示
	 */
	std::vector<std::string> PriceModeNames = {
		"BESTPX",		//最优价
		"LASTPX",		//最新价
		"MARKET",		//对手价
		"AUTOPX"		//自动
	};
public:
	/**
	 * @brief 构造函数
	 * @details 初始化各成员变量的默认值
	 */
	WtStockVWapExeUnit();

	/**
	 * @brief 析构函数
	 * @details 清理所有分配的资源，包括行情数据和商品信息
	 */
	virtual ~WtStockVWapExeUnit();

private:
	/**
	 * @brief 执行交易计算
	 * @details 根据当前市场状态和目标仓位计算并执行交易
	 *          这是实现VWAP策略的核心方法，根据设定的时间和数量分布执行订单
	 */
	void	do_calc();

	/**
	 * @brief 立即发出订单
	 * @details 不考虑VWAP策略，直接发出指定数量的订单
	 * @param qty 订单数量，正数表示买入，负数表示卖出
	 */
	void	fire_at_once(double qty);

public:
	/*
	*	所属执行器工厂名称
	*/
	/**
	 * @brief 获取所属执行器工厂名称
	 * @return 工厂名称
	 */
	virtual const char* getFactName() override;

	/*
	*	执行单元名称
	*/
	/**
	 * @brief 获取执行单元名称
	 * @return 执行单元名称
	 */
	virtual const char* getName() override;

	/*
	*	初始化执行单元
	*	ctx		执行单元运行环境
	*	code	管理的合约代码
	*/
	/**
	 * @brief 初始化执行单元
	 * @details 根据配置初始化执行单元的各项参数和状态
	 * @param ctx 执行单元运行环境
	 * @param stdCode 标准化合约代码
	 * @param cfg 执行单元配置
	 */
	virtual void init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override;

	/*
	*	订单回报
	*	localid	本地单号
	*	code	合约代码
	*	isBuy	买or卖
	*	leftover	剩余数量
	*	price	委托价格
	*	isCanceled	是否已撤销
	*/
	/**
	 * @brief 订单状态回调
	 * @details 处理订单状态变化的回调函数，包括成交和撤销等情况
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入订单
	 * @param leftover 剩余未成交数量
	 * @param price 订单委托价格
	 * @param isCanceled 是否已撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override;

	/*
	*	tick数据回调
	*	newTick	最新的tick数据
	*/
	/**
	 * @brief 行情数据回调
	 * @details 处理新到的市场行情数据，更新内部状态并触发交易逻辑
	 * @param newTick 新的市场行情数据
	 */
	virtual void on_tick(WTSTickData* newTick) override;

	/*
	*	成交回报
	*	code	合约代码
	*	isBuy	买or卖
	*	vol		成交数量,这里没有正负,通过isBuy确定买入还是卖出
	*	price	成交价格
	*/
	/**
	 * @brief 成交回报回调
	 * @details 处理订单成交的回报信息
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买入成交
	 * @param vol 成交数量
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/*
	*	下单结果回报
	*/
	/**
	 * @brief 下单结果回报
	 * @details 处理订单委托的回报结果，如果失败则重新触发交易计算
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托结果消息
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/*
	*	设置新的目标仓位
	*	code	合约代码
	*	newVol	新的目标仓位
	*/
	/**
	 * @brief 设置目标仓位
	 * @details 设置新的目标仓位，并触发交易计算过程
	 * @param stdCode 标准化合约代码
	 * @param newVol 新的目标仓位数量
	 */
	virtual void set_position(const char* stdCode, double newVol) override;

	/*
	*	交易通道就绪回调
	*/
	/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道连接成功并准备就绪时触发此回调方法
	 */
	virtual void on_channel_ready() override;

	/*
	*	交易通道丢失回调
	*/
	/**
	 * @brief 交易通道丢失回调
	 * @details 当交易通道断开连接或发生错误时触发此回调方法
	 */
	virtual void on_channel_lost() override;

	/**
	 * @brief 清空所有仓位
	 * @details 清空指定合约的所有仓位，进入清仓模式
	 * @param stdCode 标准化合约代码
	 */
	virtual void clear_all_position(const char* stdCode) override;
private:
	/// @brief 最新的市场行情数据
	WTSTickData* _last_tick;
	/// @brief 目标仓位（股数模式）
	double		_target_pos;
	/// @brief 目标金额（金额模式）
	double		_target_amount;
	/// @brief 账户可用资金
	double		_avaliable{ 0 };
	/// @brief 交易通道是否就绪
	bool		_channel_ready;
	/// @brief 计算锁，用于多线程同步
	StdUniqueMutex	_mtx_calc;

	/// @brief 商品信息，包含商品的交易参数
	WTSCommodityInfo* _comm_info;
	/// @brief 交易时段信息
	WTSSessionInfo*	_sess_info;
	/// @brief 撤单次数计数
	uint32_t	_cancel_times;



	//////////////////////////////////////////////////////////////////////////
	/// @brief 执行参数
	/// @brief 订单监控器，管理所有活动订单
	WtOrdMon		_orders_mon;
	/// @brief 撤单次数统计
	uint32_t		_cancel_cnt;
	/// @brief VWAP目标数组，按分钟记录目标总报单量
	vector<double>	VwapAim;
	//////////////////////////////////////////////////////////////////////////
	/// @brief 配置参数
	/// @brief 执行总时间，单位s
	uint32_t		_total_secs;
	/// @brief 总执行次数，即将大单拆分成几个小单
	uint32_t		_total_times;
	/// @brief 执行尾部时间，当剩余时间小于该值时采用不同策略
	uint32_t		_tail_secs;
	/// @brief 挂单时限，超过该时间后自动撤单，单位s
	uint32_t		_ord_sticky;
	/// @brief 价格模式: 0-最新价,1-最优价,2-对手价
	uint32_t		_price_mode;
	/// @brief 挂单价格偏移，相对于基准价格的tick偏移，买入为正，卖出为负
	uint32_t		_price_offset;
	/// @brief 开始时间（格式如：1000表示10:00）
	uint32_t        _begin_time;
	/// @brief 结束时间（格式如：1030表示10:30）
	uint32_t		_end_time;
	/// @brief 最小开仓数量
	double			_min_open_lots;
	/// @brief 单次发单手数
	double			_order_lots;
	/// @brief 是否为科创板股票
	bool			_is_KC{ false };
	/// @brief 订单是否可以撤销
	bool			isCanCancel;
	/// @brief 目标模式，默认为股数模式
	TargetMode		_target_mode{ TargetMode::stocks };
	/// @brief 是否处于清仓状态
	bool			_is_clear;
	/// @brief 最小手数
	double			_min_hands{ 0 };
	/// @brief 开始价格
	double			_start_price{ 0 };
	/// @brief 是否支持T+0交易（转债等为true，股票为false）
	bool			_is_t0{ false };
	/// @brief 是否完成执行
	bool			_is_finish;
	/// @brief 开始执行的时间戳
	uint64_t		_start_time;
	//////////////////////////////////////////////////////////////////////////
	/// @brief 临时变量
	/// @brief 本轮目标仓位
	double			_this_target;
	/// @brief 发单间隔，单位毫秒
	uint32_t		_fire_span;
	/// @brief 已执行次数
	uint32_t		_fired_times;
	/// @brief 上次执行的时间戳
	uint64_t		_last_fire_time;
	/// @brief 上个下单时间戳
	uint64_t		_last_place_time;
	/// @brief 上个tick数据的时间戳
	uint64_t		_last_tick_time;
	/// @brief VWAP单位时间下单量
	double			_Vwap_vol;
	/// @brief VWAP价格
	double			_Vwap_prz;
	
	/// @brief 原子变量，标记是否在计算过程中，用于防止多线程竞争
	std::atomic<bool> _in_calc;

	/**
	 * @brief 计算标记结构体
	 * @details 使用RAII模式实现的计算标记，用于多线程环境下防止重复计算
	 */
	typedef struct _CalcFlag
	{
		/// @brief 交换结果
		bool _result;
		/// @brief 原子标志指针
		std::atomic<bool>* _flag;
		/**
		 * @brief 构造函数
		 * @param flag 原子标志指针
		 */
		_CalcFlag(std::atomic<bool>*flag) :_flag(flag) {
			_result = _flag->exchange(true, std::memory_order_acq_rel);
		}

		/**
		 * @brief 析构函数，释放标志
		 */
		~_CalcFlag() {
			if (_flag)
				_flag->exchange(false, std::memory_order_acq_rel);
		}
		/**
		 * @brief bool运算符重载，返回是否已经在计算
		 */
		operator bool() const { return _result; }
	}CalcFlag;
	
	/**
	 * @brief 将手数调整为最小手数的整数倍
	 * @details 根据最小手数要求，将计算出的手数调整为最小手数的整数倍，采用四舍五入的方式
	 * @param hands 原始手数
	 * @param min_hands 最小手数
	 * @return 调整后的手数，为最小手数的整数倍
	 */
	inline int round_hands(double hands, double min_hands)
	{
		return (int)((hands + min_hands / 2) / min_hands) * min_hands;
	}

	/**
	 * @brief 获取最小下单数量
	 * @details 根据合约标的类型和产品类型判断最小下单数量
	 *          对于股票，普通股最小单位为100股，科创板股票为200股
	 *          对于可转债，最小单位为10张
	 * @param stdCode 标准化合约代码
	 * @return 最小下单数量
	 */
	inline double get_minOrderQty(std::string stdCode)
	{
		// 从合约代码中提取股票代码部分
		int code = std::stoi(StrUtil::split(stdCode, ".")[2]);
		bool is_KC = false;
		// 判断是否为科创板股票
		if (code >= 688000)
		{
			is_KC = true;
		}
		// 获取商品信息
		WTSCommodityInfo* comm_info = _ctx->getCommodityInfo(stdCode.c_str());
		double min_order = 1.0;
		// 根据产品类型设置最小下单数量
		if (strcmp(comm_info->getProduct(), cbondStr) == 0)
			min_order = 10.0;  // 可转债最小下单数量为10张
		else if (strcmp(comm_info->getProduct(), stockStr) == 0)
			if (is_KC)
				min_order = 200.0;  // 科创板股票最小下单数量为200股
			else
				min_order = 100.0;  // 普通股票最小下单数量为100股
		// 释放商品信息对象
		if (comm_info)
			comm_info->release();
		return min_order;
	}
};