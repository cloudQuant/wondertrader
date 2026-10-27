/*!
 * \file IHftStraCtx.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频策略上下文接口定义
 * \details 本文件定义了WonderTrader框架中高频策略的上下文接口，包括行情推送回调、
 * 		下单操作、持仓查询、行情订阅等功能。高频策略主要特点是对行情的快速响应和精细化交易
 */
#pragma once
#include <stdint.h>
#include <string>
#include "ExecuteDefs.h"

#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSCommodityInfo;
class WTSTickSlice;
class WTSKlineSlice;
class WTSTickData;
struct WTSBarStruct;

/**
 * @brief 高频策略订单标记
 * @details 定义不同类型的订单标记，用于指定下单时的特殊行为
 */
static const int HFT_OrderFlag_Nor = 0;  ///< 普通限价单，不自动撤单

static const int HFT_OrderFlag_FAK = 1;   ///< FAK单(Fill and Kill)，即时执行单，未能成交的部分自动撤单

static const int HFT_OrderFlag_FOK = 2;   ///< FOK单(Fill or Kill)，即时执行单，如果不能全部成交则自动撤单

/**
 * @brief 高频策略上下文接口
 * @details 定义了高频策略的上下文环境，包括高频行情响应、订单簿信息及交易接口
 * @details 高频策略与其他策略的主要区别是能够处理更细粒度的市场数据，如逐笔行情、委托队列、逐笔成交等
 */
class IHftStraCtx
{
public:
	/**
	 * @brief 构造函数
	 * @param name 策略名称
	 */
	IHftStraCtx(const char* name) :_name(name) {}

	/**
	 * @brief 析构函数
	 */
	virtual ~IHftStraCtx() {}

	/**
	 * @brief 获取策略名称
	 * @return 策略名称
	 */
	const char* name() const { return _name.c_str(); }

public:
	/**
	 * @brief 获取策略ID
	 * @return 策略ID
	 */
	virtual uint32_t id() = 0;

	/**
	 * @brief 回调函数组
	 * @details 这些函数由引擎调用，用于通知策略各种事件的发生
	 */

	/**
	 * @brief 策略初始化回调
	 * @details 在策略启动时回调，用于进行策略的初始化工作
	 */
	virtual void on_init() = 0;

	/**
	 * @brief Tick数据推送回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 最新的Tick数据
	 * @details 当新的Tick数据到达时触发
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) = 0;

	/**
	 * @brief 委托队列数据推送回调
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 最新的委托队列数据
	 * @details 当新的委托队列数据到达时触发，可以用于分析市场深度和流动性
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) = 0;

	/**
	 * @brief 逐笔委托数据推送回调
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 最新的逐笔委托数据
	 * @details 当新的逐笔委托数据到达时触发，可以用于精细化分析市场变化
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) = 0;

	/**
	 * @brief 逐笔成交数据推送回调
	 * @param stdCode 标准化合约代码
	 * @param newTrans 最新的逐笔成交数据
	 * @details 当新的逐笔成交数据到达时触发，可以用于分析市场成交情况
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) = 0;

	/**
	 * @brief K线数据推送回调
	 * @param stdCode 标准化合约代码
	 * @param period 周期，如m1/m5/d1等
	 * @param times 周期倍数
	 * @param newBar 最新的K线数据
	 * @details 当新的K线数据完成时触发，在高频策略中较少使用
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) {}

	/**
	 * @brief 交易日开始回调
	 * @param uTDate 交易日，格式为YYYYMMDD
	 * @details 每个交易日开始时触发，用于准备当日交易
	 */
	virtual void on_session_begin(uint32_t uTDate) {}

	/**
	 * @brief 交易日结束回调
	 * @param uTDate 交易日，格式为YYYYMMDD
	 * @details 每个交易日结束时触发，用于清理或汇总当日交易
	 */
	virtual void on_session_end(uint32_t uTDate) {}
	/**
	 * @brief 回测结束事件回调
	 * @details 该回调仅在回测环境下会被触发，用于通知策略回测已结束
	 * @details 可用于在回测结束时进行数据汇总、统计分析或清理资源
	 */
	virtual void on_bactest_end() {};

	/**
	 * @brief Tick数据更新回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 更新的Tick数据
	 * @details 与on_tick类似，但一般用于数据更新通知，而非交易信号触发
	 */
	virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) {}

	/**
	 * @brief 委托队列数据更新回调
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 更新的委托队列数据
	 * @details 当委托队列数据更新时触发，一般用于市场数据监控而非交易信号
	 */
	virtual void on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue) {}

	/**
	 * @brief 逐笔委托数据更新回调
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 更新的逐笔委托数据
	 * @details 当逐笔委托数据更新时触发，一般用于市场数据监控而非交易信号
	 */
	virtual void on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl) {}

	/**
	 * @brief 逐笔成交数据更新回调
	 * @param stdCode 标准化合约代码
	 * @param newTrans 更新的逐笔成交数据
	 * @details 当逐笔成交数据更新时触发，一般用于市场数据监控而非交易信号
	 */
	virtual void on_trans_updated(const char* stdCode, WTSTransData* newTrans) {}

	/**
	 * @brief 策略交易接口
	 * @details 高频策略的交易相关接口，包括撤单、下单等操作
	 */

	/**
	 * @brief 根据本地订单ID撤单
	 * @param localid 本地订单ID
	 * @return 撤单是否成功
	 * @details 撤销指定本地ID的订单，通常用于精确撤销已知订单
	 */
	virtual bool		stra_cancel(uint32_t localid) = 0;

	/**
	 * @brief 批量撤单
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买单
	 * @param qty 要撤销的数量，如果为0表示撤销全部
	 * @return 被撤销订单的ID集合
	 * @details 根据合约、方向和数量批量撤单，当需要快速撤销特定合约的所有挂单时非常有用
	 */
	virtual OrderIDs	stra_cancel(const char* stdCode, bool isBuy, double qty) = 0;

	/**
	 * @brief 下单接口: 买入
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param flag 下单标志: 0-普通限价单，1-FAK单，2-FOK单，默认0
	 * @param bForceClose 强平标志，如果为true，则强制优先平仓
	 * @return 下单成功的订单ID集合
	 * @details 发出买入指令，可能是开仓也可能是平仓，取决于当前持仓情况
	 */
	virtual OrderIDs	stra_buy(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) = 0;

	/**
	 * @brief 下单接口: 卖出
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param flag 下单标志: 0-普通限价单，1-FAK单，2-FOK单，默认0
	 * @param bForceClose 强平标志，如果为true，则强制优先平仓
	 * @return 下单成功的订单ID集合
	 * @details 发出卖出指令，可能是开仓也可能是平仓，取决于当前持仓情况
	 */
	virtual OrderIDs	stra_sell(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) = 0;

	/**
	 * @brief 下单接口: 开多(买入开仓)
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param flag 下单标志: 0-普通限价单，1-FAK单，2-FOK单
	 * @return 发出订单的本地ID
	 * @details 发出买入开仓指令，与stra_buy的区别是这个函数明确指定为开仓
	 */
	virtual uint32_t	stra_enter_long(const char* stdCode, double price, double qty, const char* userTag, int flag = 0) { return 0; }

	/**
	 * @brief 下单接口: 开空(卖出开仓)
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param flag 下单标志: 0-普通限价单，1-FAK单，2-FOK单
	 * @return 发出订单的本地ID
	 * @details 发出卖出开仓指令，与stra_sell的区别是这个函数明确指定为开仓
	 */
	virtual uint32_t	stra_enter_short(const char* stdCode, double price, double qty, const char* userTag, int flag = 0) { return 0; }

	/**
	 * @brief 下单接口: 平多(卖出平仓)
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param isToday 是否平今仓，仅对期货有效，默认false
	 * @param flag 下单标志: 0-普通限价单，1-FAK单，2-FOK单，默认0
	 * @return 发出订单的本地ID
	 * @details 发出卖出平多头指令，与stra_sell的区别是这个函数明确指定为平多头仓位
	 */
	virtual uint32_t	stra_exit_long(const char* stdCode, double price, double qty, const char* userTag, bool isToday = false, int flag = 0) { return 0; }

	/**
	 * @brief 下单接口: 平空(买入平仓)
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0表示市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于识别订单来源
	 * @param isToday 是否平今仓，仅对期货有效，默认false
	 * @param flag 下单标志: 0-普通限价单，1-FAK单，2-FOK单，默认0
	 * @return 发出订单的本地ID
	 * @details 发出买入平空头指令，与stra_buy的区别是这个函数明确指定为平空头仓位
	 */
	virtual uint32_t	stra_exit_short(const char* stdCode, double price, double qty, const char* userTag, bool isToday = false, int flag = 0) { return 0; }

	/**
	 * @brief 获取合约品种信息
	 * @param stdCode 标准化合约代码
	 * @return 合约品种信息对象指针
	 * @details 获取合约的基本信息，包括品种、合约乘数、手续费等
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) = 0;

	/**
	 * @brief 获取K线切片数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如m1/m5/d1等
	 * @param count 要获取的K线条数
	 * @return K线切片数据对象指针
	 * @details 获取指定合约的历史K线数据，用于进行技术分析
	 */
	virtual WTSKlineSlice*	stra_get_bars(const char* stdCode, const char* period, uint32_t count) = 0;

	/**
	 * @brief 获取Tick切片数据
	 * @param stdCode 标准化合约代码
	 * @param count 要获取的Tick数据条数
	 * @return Tick切片数据对象指针
	 * @details 获取指定合约的历史Tick数据，用于精细化分析
	 */
	virtual WTSTickSlice*	stra_get_ticks(const char* stdCode, uint32_t count) = 0;

	/**
	 * @brief 获取逐笔委托明细切片数据
	 * @param stdCode 标准化合约代码
	 * @param count 要获取的逐笔委托数据条数
	 * @return 逐笔委托切片数据对象指针
	 * @details 获取指定合约的历史逐笔委托数据，用于分析市场微观结构
	 */
	virtual WTSOrdDtlSlice*	stra_get_order_detail(const char* stdCode, uint32_t count) = 0;

	/**
	 * @brief 获取委托队列切片数据
	 * @param stdCode 标准化合约代码
	 * @param count 要获取的委托队列数据条数
	 * @return 委托队列切片数据对象指针
	 * @details 获取指定合约的历史委托队列数据，用于分析市场深度
	 */
	virtual WTSOrdQueSlice*	stra_get_order_queue(const char* stdCode, uint32_t count) = 0;

	/**
	 * @brief 获取逐笔成交切片数据
	 * @param stdCode 标准化合约代码
	 * @param count 要获取的逐笔成交数据条数
	 * @return 逐笔成交切片数据对象指针
	 * @details 获取指定合约的历史逐笔成交数据，用于分析市场交易行为
	 */
	virtual WTSTransSlice*	stra_get_transaction(const char* stdCode, uint32_t count) = 0;

	/**
	 * @brief 获取最新Tick数据
	 * @param stdCode 标准化合约代码
	 * @return 最新Tick数据对象指针
	 * @details 获取指定合约的最新市场行情数据
	 */
	virtual WTSTickData*	stra_get_last_tick(const char* stdCode) = 0;

	/**
	 * @brief 获取原始合约代码（分月合约代码）
	 * @param stdCode 标准化合约代码
	 * @return 原始合约代码字符串
	 * @details 当使用主力合约或标准化合约代码时，可获取对应的原始分月合约代码
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约的持仓量
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只考虑有效仓位，默认false
	 * @param flag 持仓标志：
	 *            1-开仓
	 *            2-平仓
	 *            3-所有(默认)
	 * @return 持仓量，正数表示多头持仓，负数表示空头持仓
	 * @details 获取当前策略的指定合约持仓情况
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, int flag = 3) = 0;

	/**
	 * @brief 获取指定合约持仓的平均价格
	 * @param stdCode 标准化合约代码
	 * @return 持仓平均价格
	 * @details 获取指定合约持仓的平均成本，用于计算盈亏
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约持仓的浮动盈亏
	 * @param stdCode 标准化合约代码
	 * @return 浮动盈亏
	 * @details 获取指定合约持仓的当前浮动盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约的当前市场价格
	 * @param stdCode 标准化合约代码
	 * @return 当前市场价格
	 * @details 获取指定合约的最新市场价格
	 */
	virtual double stra_get_price(const char* stdCode) = 0;

	/**
	 * @brief 获取指定合约未完成委托量
	 * @param stdCode 标准化合约代码
	 * @return 未完成委托量
	 * @details 获取指定合约的已发出但未成交的委托数量
	 */
	virtual double stra_get_undone(const char* stdCode) = 0;

	/**
	 * @brief 获取当前日期
	 * @return 当前日期，格式为YYYYMMDD
	 * @details 获取当前系统日期，用于时间控制和日志记录
	 */
	virtual uint32_t stra_get_date() = 0;

	/**
	 * @brief 获取当前时间
	 * @return 当前时间，格式为HHMMSS（时分秒）
	 * @details 获取当前系统时间，不包括毫秒
	 */
	virtual uint32_t stra_get_time() = 0;

	/**
	 * @brief 获取当前秒数
	 * @return 当前秒数，一般为当天已过秒数
	 * @details 获取当前系统时间的秒数表示，用于精确时间控制
	 */
	virtual uint32_t stra_get_secs() = 0;

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的Tick数据，成功后会通过on_tick回调推送数据
	 */
	virtual void stra_sub_ticks(const char* stdCode) = 0;

	/**
	 * @brief 订阅委托队列数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的委托队列数据，成功后会通过on_order_queue回调推送数据
	 */
	virtual void stra_sub_order_queues(const char* stdCode) = 0;

	/**
	 * @brief 订阅逐笔委托数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的逐笔委托数据，成功后会通过on_order_detail回调推送数据
	 */
	virtual void stra_sub_order_details(const char* stdCode) = 0;

	/**
	 * @brief 订阅逐笔成交数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的逐笔成交数据，成功后会通过on_transaction回调推送数据
	 */
	virtual void stra_sub_transactions(const char* stdCode) = 0;

	/**
	 * @brief 输出信息级别日志
	 * @param message 日志消息
	 * @details 输出一般信息级别的日志，用于记录策略执行过程中的关键信息
	 */
	virtual void stra_log_info(const char* message) = 0;

	/**
	 * @brief 输出调试级别日志
	 * @param message 日志消息
	 * @details 输出调试级别的日志，用于记录详细的策略执行信息，仅在调试时使用
	 */
	virtual void stra_log_debug(const char* message) = 0;

	/**
	 * @brief 输出错误级别日志
	 * @param message 日志消息
	 * @details 输出错误级别的日志，用于记录策略执行过程中的错误情况
	 */
	virtual void stra_log_error(const char* message) = 0;

	/**
	 * @brief 输出警告级别日志
	 * @param message 日志消息
	 * @details 输出警告级别的日志，用于记录需要注意但不影响策略正常运行的情况
	 */
	virtual void stra_log_warn(const char* message) {}

	/**
	 * @brief 保存用户自定义数据
	 * @param key 数据键
	 * @param val 数据值
	 * @details 将用户自定义数据保存到存储中，可用于记录策略状态或参数，便于策略重启后继续使用
	 */
	virtual void stra_save_user_data(const char* key, const char* val) {}

	/**
	 * @brief 加载用户自定义数据
	 * @param key 数据键
	 * @param defVal 默认值，当数据不存在时返回该值
	 * @return 数据值或默认值
	 * @details 从存储中加载用户自定义数据，用于恢复策略状态或参数
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") { return defVal; }

protected:
	/**
	 * @brief 策略名称
	 * @details 存储当前高频策略的名称，用于标识和日志输出
	 */
	std::string _name;
};

NS_WTP_END