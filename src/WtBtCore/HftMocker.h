/*!
 * \file HftMocker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易策略回测模拟器头文件
 * \details 该文件定义了高频交易策略在回测环境下的模拟执行器，用于模拟高频交易策略的行为。
 *          主要功能包括：模拟订单执行、处理回测行情数据、计算策略绩效和管理策略状态。
 *          它提供了与实盘交易一致的接口，帮助策略开发者在回测环境中验证高频交易策略的有效性。
 */
#pragma once
#include <queue>
#include <sstream>

#include "HisDataReplayer.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IHftStraCtx.h"
#include "../Includes/HftStrategyDefs.h"

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/fmtlib.h"

class HisDataReplayer;

/**
 * @brief 高频策略回测模拟器类
 * @details 实现高频策略的回测模拟，继承自IDataSink（数据接收器）和IHftStraCtx（高频策略上下文）接口
 *          主要负责：
 *          1. 接收并处理各类行情数据（Tick、K线、委托队列等）
 *          2. 模拟订单执行和成交流程
 *          3. 记录交易记录和统计策略绩效
 *          4. 管理策略状态和资金情况
 *          5. 提供策略所需的各类接口和数据查询功能
 */
class HftMocker : public IDataSink, public IHftStraCtx
{
public:
	/**
	 * @brief 高频策略模拟器构造函数
	 * @param replayer 历史数据回放器指针，提供行情数据和回测环境
	 * @param name 策略名称，用于标识策略实例
	 * @details 初始化高频策略模拟器，建立与历史数据回放器的连接，初始化各类状态和数据结构
	 */
	HftMocker(HisDataReplayer* replayer, const char* name);

	/**
	 * @brief 高频策略模拟器析构函数
	 * @details 清理模拟器资源，释放策略实例和相关数据结构，确保内存安全释放
	 */
	virtual ~HftMocker();

private:
	/**
	 * @brief 记录调试级别日志
	 * @tparam Args 参数包类型，支持可变参数模板
	 * @param format 日志格式化字符串，类似printf格式
	 * @param args 可变参数列表，与format中的占位符对应
	 * @details 使用fmtlib格式化日志内容，并调用stra_log_debug将调试级别日志写入日志系统
	 *          该函数用于记录策略运行中的详细调试信息，便于排错和分析
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * @brief 记录信息级别日志
	 * @tparam Args 参数包类型，支持可变参数模板
	 * @param format 日志格式化字符串，类似printf格式
	 * @param args 可变参数列表，与format中的占位符对应
	 * @details 使用fmtlib格式化日志内容，并调用stra_log_info将信息级别日志写入日志系统
	 *          该函数用于记录策略运行中的一般信息，如定期的状态更新等
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * @brief 记录错误级别日志
	 * @tparam Args 参数包类型，支持可变参数模板
	 * @param format 日志格式化字符串，类似printf格式
	 * @param args 可变参数列表，与format中的占位符对应
	 * @details 使用fmtlib格式化日志内容，并调用stra_log_error将错误级别日志写入日志系统
	 *          该函数用于记录策略运行中的错误情况，如订单失败、数据异常等
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataSink
	/**
	 * @brief 处理Tick行情数据
	 * @param stdCode 标准化的合约代码
	 * @param curTick 当前的Tick数据指针
	 * @param pxType 价格类型
	 * @details 实现IDataSink接口的Tick数据处理方法，接收并处理实时Tick行情数据
	 *          在回测中模拟实时行情数据的处理通道，将数据转发给策略实现
	 */
	virtual void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override;
	/**
	 * @brief 处理委托队列数据
	 * @param stdCode 标准化的合约代码
	 * @param curOrdQue 当前的委托队列数据指针
	 * @details 实现IDataSink接口的委托队列数据处理方法，接收并处理实时委托队列数据
	 *          在高频交易回测中，委托队列数据用于分析市场深度和流动性
	 */
	virtual void handle_order_queue(const char* stdCode, WTSOrdQueData* curOrdQue) override;
	/**
	 * @brief 处理逐笔委托数据
	 * @param stdCode 标准化的合约代码
	 * @param curOrdDtl 当前的逐笔委托数据指针
	 * @details 实现IDataSink接口的逐笔委托数据处理方法，接收并处理实时逐笔委托数据
	 *          逐笔委托数据提供了市场上各个委托的详细信息，在高频交易中用于细粒度分析
	 */
	virtual void handle_order_detail(const char* stdCode, WTSOrdDtlData* curOrdDtl) override;
	/**
	 * @brief 处理逐笔成交数据
	 * @param stdCode 标准化的合约代码
	 * @param curTrans 当前的逐笔成交数据指针
	 * @details 实现IDataSink接口的逐笔成交数据处理方法，接收并处理实时逐笔成交数据
	 *          逐笔成交数据前市场上的实际成交情况，在高频交易中用于分析市场成交活跃度
	 */
	virtual void handle_transaction(const char* stdCode, WTSTransData* curTrans) override;

	/**
	 * @brief 处理K线周期数据收盘
	 * @param stdCode 标准化的合约代码
	 * @param period 周期标识，如m1（分钟）、d1（日线）等
	 * @param times 周期倒数，例如对于分钟线，1表示1分钟，5表示5分钟
	 * @param newBar 新收盘的K线数据指针
	 * @details 实现IDataSink接口的K线收盘数据处理方法，接收并处理周期性K线收盘数据
	 *          当一个周期结束时，将新的K线数据提供给策略进行处理
	 */
	virtual void handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
	/**
	 * @brief 处理定时任务
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式HHMMSSmmm，其中mmm为毫秒
	 * @details 实现IDataSink接口的定时任务处理方法，在回测中按照设定的时间间隔触发
	 *          可以用于定时调用策略的on_schedule方法，执行策略定时任务
	 */
	virtual void handle_schedule(uint32_t uDate, uint32_t uTime) override;

	/**
	 * @brief 处理模拟器初始化
	 * @details 实现IDataSink接口的初始化方法，在回测开始前调用
	 *          用于初始化模拟器的各种资源和数据结构，并找到对应策略并调用其on_init方法
	 */
	virtual void	handle_init() override;
	/**
	 * @brief 处理交易日开始
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @details 实现IDataSink接口的交易日开始方法，在每个交易日开始时调用
	 *          用于处理交易日开始时的各种初始化和清理工作，并调用策略的on_session_begin方法
	 */
	virtual void	handle_session_begin(uint32_t curTDate) override;
	/**
	 * @brief 处理交易日结束
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @details 实现IDataSink接口的交易日结束方法，在每个交易日结束时调用
	 *          用于处理交易日结束时的清算和统计工作，如计算每日盈亏和清算账户，并调用策略的on_session_end方法
	 */
	virtual void	handle_session_end(uint32_t curTDate) override;

	/**
	 * @brief 处理回放结束
	 * @details 实现IDataSink接口的回放结束方法，在整个回测完成后调用
	 *          用于最终清理工作，如输出策略回测结果、生成统计报告等
	 */
	virtual void	handle_replay_done() override;

	/**
	 * @brief 处理Tick数据更新
	 * @param stdCode 标准化的合约代码
	 * @param newTick 新的Tick数据指针
	 * @details 实现IDataSink接口的Tick数据更新方法，在Tick数据更新时调用
	 *          与handle_tick不同，该方法负责处理更新的内部逻辑，如更新日盈亏、海面表数据等
	 */
	virtual void	on_tick_updated(const char* stdCode, WTSTickData* newTick) override;
	/**
	 * @brief 处理委托队列数据更新
	 * @param stdCode 标准化的合约代码
	 * @param newOrdQue 新的委托队列数据指针
	 * @details 实现IDataSink接口的委托队列数据更新方法，在委托队列数据更新时调用
	 *          用于处理委托队列数据更新后的内部逻辑
	 */
	virtual void	on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue) override;
	/**
	 * @brief 处理逐笔委托数据更新
	 * @param stdCode 标准化的合约代码
	 * @param newOrdDtl 新的逐笔委托数据指针
	 * @details 实现IDataSink接口的逐笔委托数据更新方法，在逐笔委托数据更新时调用
	 *          用于处理逐笔委托数据更新后的内部逻辑
	 */
	virtual void	on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;
	/**
	 * @brief 处理逐笔成交数据更新
	 * @param stdCode 标准化的合约代码
	 * @param newTrans 新的逐笔成交数据指针
	 * @details 实现IDataSink接口的逐笔成交数据更新方法，在逐笔成交数据更新时调用
	 *          用于处理逐笔成交数据更新后的内部逻辑
	 */
	virtual void	on_trans_updated(const char* stdCode, WTSTransData* newTrans) override;

	//////////////////////////////////////////////////////////////////////////
	//IHftStraCtx
	/**
	 * @brief 实现策略收到Tick数据的回调
	 * @param stdCode 标准化的合约代码
	 * @param newTick 新的Tick数据指针
	 * @details 实现IHftStraCtx接口的Tick数据回调方法，在策略订阅的合约收到新Tick数据时触发
	 *          调用到策略实现的on_tick方法，允许策略根据Tick数据做出交易决策
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief 实现策略收到委托队列数据的回调
	 * @param stdCode 标准化的合约代码
	 * @param newOrdQue 新的委托队列数据指针
	 * @details 实现IHftStraCtx接口的委托队列数据回调方法，在策略订阅的合约收到新委托队列数据时触发
	 *          调用到策略实现的on_order_queue方法，允许策略根据委托队列信息分析市场深度
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 实现策略收到逐笔委托数据的回调
	 * @param stdCode 标准化的合约代码
	 * @param newOrdDtl 新的逐笔委托数据指针
	 * @details 实现IHftStraCtx接口的逐笔委托数据回调方法，在策略订阅的合约收到新逐笔委托数据时触发
	 *          调用到策略实现的on_order_detail方法，允许策略根据逐笔委托进行更精细的市场分析
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;

	/**
	 * @brief 实现策略收到逐笔成交数据的回调
	 * @param stdCode 标准化的合约代码
	 * @param newTrans 新的逐笔成交数据指针
	 * @details 实现IHftStraCtx接口的逐笔成交数据回调方法，在策略订阅的合约收到新逐笔成交数据时触发
	 *          调用到策略实现的on_transaction方法，允许策略根据成交信息分析市场活跃度
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief 获取策略ID
	 * @return 策略唯一标识ID
	 * @details 实现IHftStraCtx接口的id方法，返回策略实例的唯一标识
	 *          在系统中用于标识不同的策略实例，特别是在有多个策略实例运行时
	 */
	virtual uint32_t id() override;

	/**
	 * @brief 策略初始化回调
	 * @details 实现IHftStraCtx接口的on_init方法，在策略初始化时调用
	 *          用于调用策略实现的on_init函数，完成策略的初始化操作，如订阅数据、设置参数等
	 */
	virtual void on_init() override;

	/**
	 * @brief K线数据回调
	 * @param stdCode 标准化的合约代码
	 * @param period 周期标识，如m1（分钟）、d1（日线）等
	 * @param times 周期倒数，例如对于分钟线，1表示1分钟，5表示5分钟
	 * @param newBar 新的K线数据指针
	 * @details 实现IHftStraCtx接口的on_bar方法，在K线数据更新时调用
	 *          用于调用策略实现的on_bar方法，允许策略根据K线数据做出交易决策
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 交易日开始回调
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @details 实现IHftStraCtx接口的on_session_begin方法，在每个交易日开始时调用
	 *          用于调用策略实现的on_session_begin方法，让策略有机会在新交易日开始时进行必要的初始化
	 */
	virtual void on_session_begin(uint32_t curTDate) override;

	/**
	 * @brief 交易日结束回调
	 * @param curTDate 当前交易日，格式为YYYYMMDD
	 * @details 实现IHftStraCtx接口的on_session_end方法，在每个交易日结束时调用
	 *          用于调用策略实现的on_session_end方法，让策略有机会在交易日结束时进行清算和统计工作
	 */
	virtual void on_session_end(uint32_t curTDate) override;

	/**
	 * @brief 根据本地订单ID撤单
	 * @param localid 订单的本地ID
	 * @return 撤单是否成功
	 * @details 实现IHftStraCtx接口的撤单方法，根据订单的本地ID发送撤单请求
	 *          在回测中模拟实际交易中的撤单操作，将订单状态标记为已撤销
	 */
	virtual bool stra_cancel(uint32_t localid) override;

	/**
	 * @brief 根据合约和方向批量撤单
	 * @param stdCode 标准化的合约代码
	 * @param isBuy 是否为买单，true为买单，false为卖单
	 * @param qty 需要撤销的数量，默认为0表示撤销所有指定方向的未成交订单
	 * @return 撤销的订单ID列表
	 * @details 实现IHftStraCtx接口的批量撤单方法，根据合约代码、交易方向和数量撤销对应的订单
	 *          允许策略同时撤销多个符合条件的未成交订单，提高策略的灵活性
	 */
	virtual OrderIDs stra_cancel(const char* stdCode, bool isBuy, double qty = 0) override;

	/**
	 * @brief 发送买入订单
	 * @param stdCode 标准化的合约代码
	 * @param price 买入价格
	 * @param qty 买入数量
	 * @param userTag 用户标签，用于订单跟踪和识别
	 * @param flag 标志，用于指定下单类型，默认为0
	 * @param bForceClose 是否强制平仓，默认为false
	 * @return 新创建的订单ID列表
	 * @details 实现IHftStraCtx接口的买入方法，发送买入订单并返回订单ID
	 *          在回测中模拟下单流程，创建新订单并根据回测环境中的行情数据进行撤单成交处理
	 */
	virtual OrderIDs stra_buy(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) override;

	/**
	 * @brief 发送卖出订单
	 * @param stdCode 标准化的合约代码
	 * @param price 卖出价格
	 * @param qty 卖出数量
	 * @param userTag 用户标签，用于订单跟踪和识别
	 * @param flag 标志，用于指定下单类型，默认为0
	 * @param bForceClose 是否强制平仓，默认为false
	 * @return 新创建的订单ID列表
	 * @details 实现IHftStraCtx接口的卖出方法，发送卖出订单并返回订单ID
	 *          在回测中模拟下单流程，创建新订单并根据回测环境中的行情数据进行撤单成交处理
	 */
	virtual OrderIDs stra_sell(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) override;

	/**
	 * @brief 获取合约信息
	 * @param stdCode 标准化的合约代码
	 * @return 合约信息指针
	 * @details 实现IHftStraCtx接口的获取合约信息方法，返回指定合约的详细信息
	 *          包括合约的交易单位、最小变动单位、手续费率等相关信息
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;

	/**
	 * @brief 获取K线序列数据
	 * @param stdCode 标准化的合约代码
	 * @param period 周期标识，如m1（分钟）、d1（日线）等
	 * @param count 要请求的K线数量
	 * @return K线序列数据指针
	 * @details 实现IHftStraCtx接口的获取K线序列方法，返回指定合约和周期的历史K线数据
	 *          并根据请求的数量返回最近的count条K线数据记录
	 */
	virtual WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;

	/**
	 * @brief 获取Tick序列数据
	 * @param stdCode 标准化的合约代码
	 * @param count 要请求的Tick数量
	 * @return Tick序列数据指针
	 * @details 实现IHftStraCtx接口的获取Tick序列方法，返回指定合约的历史Tick数据
	 *          并根据请求的数量返回最近的count条Tick数据记录
	 */
	virtual WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取逐笔委托序列数据
	 * @param stdCode 标准化的合约代码
	 * @param count 要请求的逐笔委托数量
	 * @return 逐笔委托序列数据指针
	 * @details 实现IHftStraCtx接口的获取逐笔委托序列方法，返回指定合约的历史逐笔委托数据
	 *          并根据请求的数量返回最近的count条逐笔委托数据记录
	 */
	virtual WTSOrdDtlSlice*	stra_get_order_detail(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托队列序列数据
	 * @param stdCode 标准化的合约代码
	 * @param count 要请求的委托队列数量
	 * @return 委托队列序列数据指针
	 * @details 实现IHftStraCtx接口的获取委托队列序列方法，返回指定合约的历史委托队列数据
	 *          并根据请求的数量返回最近的count条委托队列数据记录
	 */
	virtual WTSOrdQueSlice*	stra_get_order_queue(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取逐笔成交序列数据
	 * @param stdCode 标准化的合约代码
	 * @param count 要请求的逐笔成交数量
	 * @return 逐笔成交序列数据指针
	 * @details 实现IHftStraCtx接口的获取逐笔成交序列方法，返回指定合约的历史逐笔成交数据
	 *          并根据请求的数量返回最近的count条逐笔成交数据记录
	 */
	virtual WTSTransSlice*	stra_get_transaction(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 标准化的合约代码
	 * @return 最新Tick数据指针
	 * @details 实现IHftStraCtx接口的获取最新Tick方法，返回指定合约的最新一条Tick数据
	 *          该方法常用于获取当前市场价格、成交量等实时信息
	 */
	virtual WTSTickData* stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 获取分月合约代码
	 * @param stdCode 标准化的合约代码
	 * @return 原始分月合约代码
	 * @details 实现IHftStraCtx接口的获取原始合约代码方法，将标准化后的代码转换回原始的分月合约代码
	 *          如将SHFE.rb.HOT转换为SHFE.rb2101等实际的合约月份
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	/**
	 * @brief 获取合约持仓量
	 * @param stdCode 标准化的合约代码
	 * @param bOnlyValid 是否只返回有效持仓，默认为false
	 * @param flag 持仓类型标志，3表示全部，1表示多头，2表示空头，默认为3
	 * @return 持仓量，正数表示多头，负数表示空头
	 * @details 实现IHftStraCtx接口的获取持仓量方法，返回指定合约的当前持仓量
	 *          可以指定只查询有效持仓（即实际可用于交易的持仓）或查询指定方向的持仓
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, int flag = 3) override;

	/**
	 * @brief 获取合约持仓均价
	 * @param stdCode 标准化的合约代码
	 * @return 持仓均价
	 * @details 实现IHftStraCtx接口的获取持仓均价方法，返回指定合约的当前持仓均价
	 *          该均价用于计算持仓盈亏和平仓盈亏
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) override;

	/**
	 * @brief 获取合约持仓盈亏
	 * @param stdCode 标准化的合约代码
	 * @return 持仓浮动盈亏
	 * @details 实现IHftStraCtx接口的获取持仓盈亏方法，返回指定合约的当前持仓浮动盈亏
	 *          根据当前市场价格和持仓均价计算浮动盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) override;

	/**
	 * @brief 获取合约未完成数量
	 * @param stdCode 标准化的合约代码
	 * @return 未完成委托数量，正数表示买入未完成，负数表示卖出未完成
	 * @details 实现IHftStraCtx接口的获取未完成数量方法，返回指定合约的未完成订单数量
	 *          未完成委托包括未成交和部分成交的订单剩余数量
	 */
	virtual double stra_get_undone(const char* stdCode) override;

	/**
	 * @brief 获取合约当前价格
	 * @param stdCode 标准化的合约代码
	 * @return 当前市场价格
	 * @details 实现IHftStraCtx接口的获取当前价格方法，返回指定合约的当前最新成交价格
	 *          通常使用最新Tick数据中的最新成交价格作为当前价格
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取当前交易日期
	 * @return 当前交易日期，格式为YYYYMMDD
	 * @details 实现IHftStraCtx接口的获取当前日期方法，返回当前回测系统中的交易日期
	 *          该日期在每个交易日开始时更新
	 */
	virtual uint32_t stra_get_date() override;

	/**
	 * @brief 获取当前交易时间
	 * @return 当前交易时间，格式为HHMMSS（小时分秒）
	 * @details 实现IHftStraCtx接口的获取当前时间方法，返回当前回测系统中的交易时间
	 *          该时间根据数据回放的进度实时更新
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取当前交易时间（秒）
	 * @return 当前时间秒数，格式为从0点开始的秒数
	 * @details 实现IHftStraCtx接口的获取当前时间秒数方法，返回当前回测系统中的时间秒数
	 *          可以用来计算时间差或更精细的时间判断
	 */
	virtual uint32_t stra_get_secs() override;

	/**
	 * @brief 订阅Tick行情数据
	 * @param stdCode 标准化的合约代码
	 * @details 实现IHftStraCtx接口的订阅Tick数据方法，订阅指定合约的Tick行情数据
	 *          订阅后，当该合约有新的Tick数据到来时，会触发on_tick回调
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * @brief 订阅委托队列数据
	 * @param stdCode 标准化的合约代码
	 * @details 实现IHftStraCtx接口的订阅委托队列数据方法，订阅指定合约的委托队列数据
	 *          订阅后，当该合约有新的委托队列数据到来时，会触发on_order_queue回调
	 */
	virtual void stra_sub_order_queues(const char* stdCode) override;

	/**
	 * @brief 订阅逐笔委托数据
	 * @param stdCode 标准化的合约代码
	 * @details 实现IHftStraCtx接口的订阅逐笔委托数据方法，订阅指定合约的逐笔委托数据
	 *          订阅后，当该合约有新的逐笔委托数据到来时，会触发on_order_detail回调
	 */
	virtual void stra_sub_order_details(const char* stdCode) override;

	/**
	 * @brief 订阅逐笔成交数据
	 * @param stdCode 标准化的合约代码
	 * @details 实现IHftStraCtx接口的订阅逐笔成交数据方法，订阅指定合约的逐笔成交数据
	 *          订阅后，当该合约有新的逐笔成交数据到来时，会触发on_transaction回调
	 */
	virtual void stra_sub_transactions(const char* stdCode) override;

	/**
	 * @brief 记录信息级别日志
	 * @param message 日志消息
	 * @details 实现IHftStraCtx接口的信息日志记录方法，记录并输出策略的信息级别日志
	 *          用于策略中记录一般性的信息，如交易信号、状态变化等
	 */
	virtual void stra_log_info(const char* message) override;
	/**
	 * @brief 记录调试级别日志
	 * @param message 日志消息
	 * @details 实现IHftStraCtx接口的调试日志记录方法，记录并输出策略的调试级别日志
	 *          用于策略中记录详细的调试信息，如数据处理过程、中间计算结果等
	 */
	virtual void stra_log_debug(const char* message) override;
	/**
	 * @brief 记录警告级别日志
	 * @param message 日志消息
	 * @details 实现IHftStraCtx接口的警告日志记录方法，记录并输出策略的警告级别日志
	 *          用于策略中记录需要关注的警告信息，如可能导致问题的异常状况
	 */
	virtual void stra_log_warn(const char* message) override;
	/**
	 * @brief 记录错误级别日志
	 * @param message 日志消息
	 * @details 实现IHftStraCtx接口的错误日志记录方法，记录并输出策略的错误级别日志
	 *          用于策略中记录严重的错误情况，如交易失败、数据异常等
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * @brief 保存用户自定义数据
	 * @param key 数据键名
	 * @param val 数据值
	 * @details 实现IHftStraCtx接口的保存用户数据方法，存储策略自定义的数据
	 *          允许策略保存状态或配置信息，为键值对形式，可在后续使用load_user_data读取
	 */
	virtual void stra_save_user_data(const char* key, const char* val) override;

	/**
	 * @brief 读取用户自定义数据
	 * @param key 数据键名
	 * @param defVal 默认值，当指定键不存在时返回的值，默认为空字符串
	 * @return 键对应的值，如果键不存在则返回默认值
	 * @details 实现IHftStraCtx接口的读取用户数据方法，读取先前通过save_user_data保存的数据
	 *          用于策略中恢复保存的状态、配置或其他定制数据
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief 成交回调
	 * @param localid 订单的本地ID
	 * @param stdCode 标准化的合约代码
	 * @param isBuy 是否为买入成交，true为买入，false为卖出
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @param userTag 用户标签，用于订单跟踪和识别
	 * @details 订单成交时的回调方法，当订单在回测中被操作成交时触发
	 *          用于处理订单成交后的操作，如更新持仓、记录成交详情等
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag);

	/**
	 * @brief 订单状态回调
	 * @param localid 订单的本地ID
	 * @param stdCode 标准化的合约代码
	 * @param isBuy 是否为买入订单，true为买入，false为卖出
	 * @param totalQty 订单总数量
	 * @param leftQty 剩余未成交数量
	 * @param price 订单价格
	 * @param isCanceled 订单是否已撤销
	 * @param userTag 用户标签，用于订单跟踪和识别
	 * @details 订单状态变化时的回调方法，当订单状态发生变化时触发
	 *          如订单提交、部分成交、全部成交或被撤销时都会触发此回调
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag);

	/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道就绪可用时的回调方法
	 *          通常用于通知策略交易系统已经准备就绪，可以开始发送交易指令
	 */
	virtual void on_channel_ready();

	/**
	 * @brief 委托回报回调
	 * @param localid 订单的本地ID
	 * @param stdCode 标准化的合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托返回的消息，通常在失败时包含错误信息
	 * @param userTag 用户标签，用于订单跟踪和识别
	 * @details 委托返回时的回调方法，当委托被提交后收到交易系统的回报时触发
	 *          通知策略委托是否被交易系统接受，如果失败则提供错误原因
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag);

public:
	/**
	 * @brief 初始化高频策略工厂
	 * @param cfg 策略配置对象指针
	 * @return 初始化是否成功
	 * @details 初始化高频策略工厂，加载策略模块并创建策略实例
	 *          根据配置加载指定的策略动态链接库，并创建策略工厂和策略实例
	 */
	bool	init_hft_factory(WTSVariant* cfg);
	/**
	 * @brief 安装钩子函数
	 * @details 安装策略实例的钩子函数，将需要策略回调的函数挂载到策略实例上
	 *          包括实现交易回调、成交回调等与策略相关的钩子
	 */
	void	install_hook();
	/**
	 * @brief 启用或禁用钩子函数
	 * @param bEnabled 是否启用钩子，默认为true
	 * @details 对挂载到策略实例上的钩子函数进行启用或禁用操作
	 *          可以用于暂时暂停策略的回调执行，如在特定情况下防止策略交易
	 */
	void	enable_hook(bool bEnabled = true);
	/**
	 * @brief 处理下一个Tick
	 * @details 在回测过程中处理下一个Tick数据的方法
	 *          用于逐个处理Tick数据，并调用相应的策略回调，通常用于逻速式回测
	 */
	void	step_tick();

private:
	/**
	 * @brief 任务函数类型定义
	 * @details 定义一个无参数、无返回值的函数对象类型，用于定义可排队执行的任务
	 */
	typedef std::function<void()> Task;

	/**
	 * @brief 将任务添加到任务队列
	 * @param task 要添加的任务函数
	 * @details 将任务加入到任务队列中，等待处理线程执行
	 *          用于实现线程安全的任务处理，尤其是当任务来自不同线程时
	 */
	void	postTask(Task task);

	/**
	 * @brief 处理任务队列中的任务
	 * @details 处理任务队列中的所有任务，按照先进先出的顺序执行
	 *          通常在主线程中调用，以确保任务在正确的线程上下文中执行
	 */
	void	procTask();

	/**
	 * @brief 处理订单
	 * @param localid 订单的本地ID
	 * @return 处理是否成功
	 * @details 根据本地订单ID处理订单，包括订单的撤销和成交模拟
	 *          基于当前的市场数据和订单信息判断订单是否应该成交或撤销
	 */
	bool	procOrder(uint32_t localid);

	/**
	 * @brief 设置持仓量
	 * @param stdCode 标准化的合约代码
	 * @param qty 持仓数量，正数为多头，负数为空头
	 * @param price 持仓价格，默认为0.0
	 * @param userTag 用户标签，默认为空字符串
	 * @details 直接设置指定合约的持仓量，不通过交易而是直接修改持仓数据
	 *          通常用于以特定持仓量和价格初始化策略的持仓
	 */
	void	do_set_position(const char* stdCode, double qty, double price = 0.0, const char* userTag = "");
	/**
	 * @brief 更新动态持仓盈亏
	 * @param stdCode 标准化的合约代码
	 * @param newTick 最新的Tick数据指针
	 * @details 根据最新的价格数据更新指定合约的浮动盈亏
	 *          当收到新的Tick数据时，使用最新价格重新计算当前持仓的浮动盈亏
	 */
	void	update_dyn_profit(const char* stdCode, WTSTickData* newTick);

	/**
	 * @brief 输出回测结果
	 * @details 将当前回测的结果数据输出到指定文件或控制台
	 *          包括交易记录、收益统计、策略表现细节等信息
	 *          通常在回测结束时调用
	 */
	void	dump_outputs();
	/**
	 * @brief 记录交易日志
	 * @param stdCode 标准化的合约代码
	 * @param isLong 是否为多头交易，true为多头，false为空头
	 * @param isOpen 是否为开仓交易，true为开仓，false为平仓
	 * @param curTime 当前交易时间戳
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param fee 交易手续费
	 * @param userTag 用户标签
	 * @details 记录每笔交易的详细信息，便于后续分析和统计
	 *          将交易信息格式化并保存到交易日志中
	 */
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee, const char* userTag);
	/**
	 * @brief 记录平仓日志
	 * @param stdCode 标准化的合约代码
	 * @param isLong 是否为平多仓位，true为平多，false为平空
	 * @param openTime 开仓时间戳
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间戳
	 * @param closepx 平仓价格
	 * @param qty 交易数量
	 * @param profit 当前平仓盈亏
	 * @param maxprofit 最大浮盈
	 * @param maxloss 最大浮亏
	 * @param totalprofit 累计盈亏
	 * @param enterTag 开仓标签
	 * @param exitTag 平仓标签
	 * @details 记录平仓操作的详细信息，包括开平仓时间、价格、数量及交易盈亏等
	 *          这些信息在回测结束后将被用于统计和分析策略性能
	 */
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double maxprofit, double maxloss, double totalprofit, const char* enterTag, const char* exitTag);

private:
	/**
	 * @brief 历史数据回放器指针
	 * @details 用于提供回测所需的历史行情数据，包括各种Tick数据、K线数据等
	 *          并负责数据的时间控制和回放过程的管理
	 */
	HisDataReplayer*	_replayer;

	/**
	 * @brief 是否使用新价格
	 * @details 控制订单成交是否使用最新的价格进行模拟成交
	 */
	bool			_use_newpx;

	/**
	 * @brief 错误率
	 * @details 模拟交易中的错误概率，用于模拟实际交易中可能出现的失败情况
	 */
	uint32_t		_error_rate;

	/**
	 * @brief 是否在当前Tick撤单
	 * @details 控制订单模拟撤单是否在当前Tick数据到来时立即执行
	 */
	bool			_match_this_tick;	//是否在当前tick撮合

	/**
	 * @brief 价格映射类型定义
	 * @details 定义一个哈希表，用于存储合约代码到其当前价格的映射
	 */
	typedef wt_hashmap<std::string, double> PriceMap;

	/**
	 * @brief 合约价格映射表
	 * @details 存储各个合约当前的价格信息，键为合约代码，值为当前价格
	 */
	PriceMap		_price_map;


	/**
	 * @brief 策略工厂信息结构
	 * @details 存储策略动态链接库和策略工厂相关的信息
	 */
	typedef struct _StraFactInfo
	{
		/**
		 * @brief 模块路径
		 * @details 存储策略动态链接库的文件路径
		 */
		std::string		_module_path;

		/**
		 * @brief 动态链接库句柄
		 * @details 动态链接库的句柄，用于访问和控制已加载的库
		 */
		DllHandle		_module_inst;

		/**
		 * @brief 高频策略工厂指针
		 * @details 指向高频策略工厂的指针，负责创建和管理策略实例
		 */
		IHftStrategyFact*	_fact;

		/**
		 * @brief 创建策略工厂的函数指针
		 * @details 指向动态链接库中创建策略工厂的函数
		 */
		FuncCreateHftStraFact	_creator;

		/**
		 * @brief 删除策略工厂的函数指针
		 * @details 指向动态链接库中删除策略工厂的函数
		 */
		FuncDeleteHftStraFact	_remover;

		/**
		 * @brief 构造函数
		 * @details 初始化策略工厂信息结构
		 */
		_StraFactInfo()
		{
			_module_inst = NULL;
			_fact = NULL;
		}

		/**
		 * @brief 析构函数
		 * @details 释放策略工厂相关资源
		 */
		~_StraFactInfo()
		{
			if (_fact)
				_remover(_fact);
		}
	} StraFactInfo;
	/**
	 * @brief 策略工厂信息
	 * @details 存储当前使用的策略工厂相关信息
	 */
	StraFactInfo	_factory;

	/**
	 * @brief 高频策略实例指针
	 * @details 指向当前使用的高频策略实例，由策略工厂创建
	 */
	HftStrategy*	_strategy;

	/**
	 * @brief 任务队列互斥锁
	 * @details 保护任务队列的互斥锁，确保多线程环境下对任务队列的安全访问
	 */
	StdUniqueMutex		_mtx;

	/**
	 * @brief 任务队列
	 * @details 存储等待执行的任务，这些任务通过postTask添加，由procTask处理
	 */
	std::queue<Task>	_tasks;

	/**
	 * @brief 控制互斥锁
	 * @details 用于保护对策略控制相关操作的线程安全性
	 */
	StdRecurMutex		_mtx_control;

	/**
	 * @brief 订单信息结构体
	 * @details 存储订单的各项基本信息，包括方向、价格、数量等
	 */
	typedef struct _OrderInfo
	{
		/**
		 * @brief 是否为买入订单
		 * @details true表示买入订单，false表示卖出订单
		 */
		bool	_isBuy;

		/**
		 * @brief 合约代码
		 * @details 订单对应的合约代码
		 */
		char	_code[32];

		/**
		 * @brief 订单价格
		 * @details 下单时指定的委托价格
		 */
		double	_price;

		/**
		 * @brief 订单总数量
		 * @details 订单的总交易数量
		 */
		double	_total;

		/**
		 * @brief 剩余未成交数量
		 * @details 订单当前未成交的数量
		 */
		double	_left;

		/**
		 * @brief 用户自定义标签
		 * @details 用户自定义的订单标签，用于识别和跟踪订单
		 */
		char	_usertag[32];
		
		/**
		 * @brief 订单本地ID
		 * @details 订单的本地标识ID，用于内部跟踪和操作订单
		 */
		uint32_t	_localid;

		/**
		 * @brief 下单后是否已处理
		 * @details 标记订单在下单后是否已经进行过处理
		 */
		bool	_proced_after_placed;	//下单后是否处理过			

		/**
		 * @brief 默认构造函数
		 * @details 初始化订单信息结构，将所有成员设置为0或空
		 */
		_OrderInfo()
		{
			memset(this, 0, sizeof(_OrderInfo));
		}

		/**
		 * @brief 拷贝构造函数
		 * @param rhs 源订单信息对象
		 * @details 从其他订单信息对象创建新实例
		 */
		_OrderInfo(const struct _OrderInfo& rhs)
		{
			memcpy(this, &rhs, sizeof(_OrderInfo));
		}

		/**
		 * @brief 赋值运算符重载
		 * @param rhs 源订单信息对象
		 * @return 当前对象的引用
		 * @details 实现订单信息对象的赋值操作
		 */
		_OrderInfo& operator =(const struct _OrderInfo& rhs)
		{
			memcpy(this, &rhs, sizeof(_OrderInfo));
			return *this;
		}

	} OrderInfo;
	/**
	 * @brief 订单信息指针类型
	 * @details 定义一个智能指针类型，指向订单信息结构，实现自动内存管理
	 */
	typedef std::shared_ptr<OrderInfo> OrderInfoPtr;

	/**
	 * @brief 订单容器类型
	 * @details 定义一个哈希表类型，用于存储本地订单ID到订单信息的映射
	 */
	typedef wt_hashmap<uint32_t, OrderInfoPtr> Orders;

	/**
	 * @brief 订单管理互斥锁
	 * @details 保护订单管理相关操作的互斥锁，确保多线程环境下对订单的安全访问
	 */
	StdRecurMutex	_mtx_ords;

	/**
	 * @brief 订单容器
	 * @details 存储所有当前活跃的订单，使用本地订单ID作为键
	 */
	Orders			_orders;

	/**
	 * @brief 商品映射类型
	 * @details 定义商品映射类型，用于存储和检索商品信息
	 */
	typedef WTSHashMap<std::string> CommodityMap;

	/**
	 * @brief 商品映射对象
	 * @details 存储所有可交易商品的信息，包括合约规格、手续费等
	 */
	CommodityMap*	_commodities;

	//用户数据
	/**
	 * @brief 字符串映射类型
	 * @details 定义一个字符串哈希表类型，用于存储键值对的字符串数据
	 */
	typedef wt_hashmap<std::string, std::string> StringHashMap;

	/**
	 * @brief 用户自定义数据存储
	 * @details 存储策略自定义的数据，允许策略在运行过程中保存状态信息
	 */
	StringHashMap	_user_datas;
	bool			_ud_modified;

	/**
	 * @brief 持仓明细结构体
	 * @details 记录单个持仓明细的详细信息，包括方向、价格、数量及开仓时间等
	 *          用于跟踪每个仓位的盈亏状况和历史记录
	 */
	typedef struct _DetailInfo
	{
		/**
		 * @brief 是否为多头
		 * @details true表示多头，false表示空头
		 */
		bool		_long;

		/**
		 * @brief 开仓价格
		 * @details 开仓时的价格
		 */
		double		_price;

		/**
		 * @brief 仓位数量
		 * @details 该仓位的具体数量
		 */
		double		_volume;

		/**
		 * @brief 开仓时间戳
		 * @details 开仓时的时间戳，用于计算持仓时间和时间分析
		 */
		uint64_t	_opentime;

		/**
		 * @brief 开仓交易日
		 * @details 开仓时的交易日，格式为YYYYMMDD
		 */
		uint32_t	_opentdate;

		/**
		 * @brief 最大浮盈
		 * @details 该仓位历史上的最大浮动盈利
		 */
		double		_max_profit;

		/**
		 * @brief 最大浮亏
		 * @details 该仓位历史上的最大浮动亏损
		 */
		double		_max_loss;

		/**
		 * @brief 当前盈亏
		 * @details 该仓位的当前盈亏状况
		 */
		double		_profit;

		/**
		 * @brief 用户标签
		 * @details 开仓时的用户自定义标签，用于识别和跟踪交易
		 */
		char		_usertag[32];

		/**
		 * @brief 构造函数
		 * @details 初始化持仓明细结构，将所有成员变量初始化为0
		 */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * @brief 持仓信息结构体
	 * @details 存储特定合约的总体持仓情况，包括总持仓量、盈亏和明细列表
	 *          用于统一管理同一合约的所有仓位和计算盈亏
	 */
	typedef struct _PosInfo
	{
		/**
		 * @brief 总持仓量
		 * @details 当前合约的总持仓量，正数表示多头，负数表示空头
		 */
		double		_volume;

		/**
		 * @brief 平仓盈亏
		 * @details 已平仓部分的实现盈亏
		 */
		double		_closeprofit;

		/**
		 * @brief 浮动盈亏
		 * @details 当前持仓部分的浮动盈亏，基于最新市场价格计算
		 */
		double		_dynprofit;

		/**
		 * @brief 冻结仓位
		 * @details 当前被冻结的仓位数量，如已提交平仓指令但尚未成交的部分
		 */
		double		_frozen;

		/**
		 * @brief 明细列表
		 * @details 存储每笔持仓的详细信息，包括开仓价、时间等
		 */
		std::vector<DetailInfo> _details;

		/**
		 * @brief 构造函数
		 * @details 初始化持仓信息结构，将数值成员初始化为0
		 */
		_PosInfo()
		{
			_volume = 0;
			_closeprofit = 0;
			_dynprofit = 0;
			_frozen = 0;
		}

		/**
		 * @brief 获取有效持仓量
		 * @return 当前可用于交易的持仓量
		 * @details 计算当前可供交易的有效持仓量，即总持仓量减去冻结量
		 */
		inline double valid() const { return _volume - _frozen; }
	} PosInfo;
	/**
	 * @brief 持仓映射容器类型
	 * @details 定义一个从合约代码到持仓信息的映射类型
	 */
	typedef wt_hashmap<std::string, PosInfo> PositionMap;

	/**
	 * @brief 持仓映射容器
	 * @details 存储所有合约的持仓信息，键为合约代码，值为相应的持仓信息
	 */
	PositionMap		_pos_map;

	/**
	 * @brief 交易日志流
	 * @details 记录所有交易操作的日志流，包括开仓和平仓操作
	 */
	std::stringstream	_trade_logs;

	/**
	 * @brief 平仓日志流
	 * @details 记录所有平仓操作的详细日志流，包括每笔平仓的具体信息
	 */
	std::stringstream	_close_logs;

	/**
	 * @brief 资金日志流
	 * @details 记录资金变动情况的日志流，包括盈亏、手续费等
	 */
	std::stringstream	_fund_logs;

	/**
	 * @brief 信号日志流
	 * @details 记录交易信号相关的日志流，用于分析信号质量
	 */
	std::stringstream	_sig_logs;

	/**
	 * @brief 持仓日志流
	 * @details 记录持仓变动情况的日志流，用于跟踪持仓变化
	 */
	std::stringstream	_pos_logs;

	/**
	 * @brief 策略资金信息结构体
	 * @details 记录策略的整体资金状况，包括总盈亏、浮动盈亏和手续费等
	 *          用于策略性能的统计和评估
	 */
	typedef struct _StraFundInfo
	{
		/**
		 * @brief 总盈亏
		 * @details 策略的已实现总盈亏，即已平仓部分的盈亏累计
		 */
		double	_total_profit;

		/**
		 * @brief 总浮动盈亏
		 * @details 策略的当前浮动盈亏，即未平仓部分基于当前价格的盈亏
		 */
		double	_total_dynprofit;

		/**
		 * @brief 总手续费
		 * @details 策略交易产生的总手续费支出
		 */
		double	_total_fees;

		/**
		 * @brief 构造函数
		 * @details 初始化资金信息结构，将所有成员初始化为0
		 */
		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	/**
	 * @brief 策略资金信息
	 * @details 存储当前策略的资金状况，用于跟踪和计算策略绩效
	 */
	StraFundInfo		_fund_info;

protected:
	/**
	 * @brief 上下文ID
	 * @details 策略实例的唯一标识ID，用于区分不同的策略实例
	 */
	uint32_t		_context_id;

	/**
	 * @brief 计算互斥锁
	 * @details 保护计算相关操作的互斥锁，确保多线程环境下计算的线程安全
	 */
	StdUniqueMutex	_mtx_calc;

	/**
	 * @brief 计算条件变量
	 * @details 用于线程同步的条件变量，协调计算线程的等待和唇醒
	 */
	StdCondVariable	_cond_calc;

	/**
	 * @brief 是否启用钩子
	 * @details 人为控制是否启用策略钩子函数，可通过enable_hook方法设置
	 */
	bool			_has_hook;		//这是人为控制是否启用钩子

	/**
	 * @brief 钩子是否有效
	 * @details 根据是否是异步回测模式自动确定钩子是否可用
	 */
	bool			_hook_valid;	//这是根据是否是异步回测模式而确定钩子是否可用

	/**
	 * @brief 是否已恢复标志
	 * @details 原子变量，标记策略是否已从暂停状态恢复，用于控制状态转换
	 */
	std::atomic<bool>	_resumed;	//临时变量，用于控制状态

	//tick订阅列表
	/**
	 * @brief Tick数据订阅列表
	 * @details 存储当前策略订阅的所有合约代码，用于管理订阅关系
	 */
	wt_hashset<std::string> _tick_subs;

	/**
	 * @brief Tick缓存类型
	 * @details 定义Tick数据缓存的映射类型
	 */
	typedef WTSHashMap<std::string>	TickCache;

	/**
	 * @brief Tick缓存
	 * @details 存储最新的Tick数据缓存，提供快速访问
	 */
	TickCache*	_ticks;
};

