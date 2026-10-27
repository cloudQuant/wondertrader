/*!
 * \file HftStraBaseCtx.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 高频交易策略基础上下文头文件
 * \details 本文件定义了WonderTrader高频交易策略的基础上下文类(HftStraBaseCtx)
 *          该类实现了IHftStraCtx和ITrdNotifySink接口，提供了高频交易策略所需的各种功能
 *          包括行情数据处理、交易指令发送、持仓管理、日志记录等
 */
#pragma once

#include "../Includes/FasterDefs.h"
#include "../Includes/IHftStraCtx.h"
#include "../Share/BoostFile.hpp"
#include "../Share/fmtlib.h"

#include <boost/circular_buffer.hpp>

#include "ITrdNotifySink.h"

NS_WTP_BEGIN

class WtHftEngine;
class TraderAdapter;

/**
 * @brief 高频交易策略基础上下文类
 * @details 高频交易策略基础上下文类，继承自IHftStraCtx和ITrdNotifySink接口
 *          提供了高频交易策略开发所需的各种功能，包括：
 *          1. 行情数据处理（Tick、K线、委托队列、委托明细、成交明细等）
 *          2. 交易指令发送（买入、卖出、开多、开空、平多、平空等）
 *          3. 持仓管理和盈亏计算
 *          4. 日志记录和用户数据存储
 *          该类为高频交易策略提供了一个统一的接口，简化了策略开发过程
 */
class HftStraBaseCtx : public IHftStraCtx, public ITrdNotifySink
{
public:
	/**
	 * @brief 高频交易策略基础上下文构造函数
	 * @param engine 高频交易引擎指针
	 * @param name 策略名称
	 * @param bAgent 是否为数据代理模式
	 * @param slippage 滑点设置
	 * @details 初始化高频交易策略基础上下文对象
	 */
	HftStraBaseCtx(WtHftEngine* engine, const char* name, bool bAgent, int32_t slippage);

	/**
	 * @brief 高频交易策略基础上下文析构函数
	 * @details 清理高频交易策略基础上下文对象的资源
	 */
	virtual ~HftStraBaseCtx();

	/**
	 * @brief 设置交易适配器
	 * @param trader 交易适配器指针
	 * @details 将交易适配器关联到策略上下文，使策略可以发送交易指令
	 */
	void setTrader(TraderAdapter* trader);

public:
	//////////////////////////////////////////////////////////////////////////
	//IHftStraCtx 接口
	/**
	 * @brief 获取策略上下文ID
	 * @return 策略上下文ID
	 * @details 返回策略上下文的唯一标识符
	 */
	virtual uint32_t id() override;

	/**
	 * @brief 策略初始化回调
	 * @details 在策略初始化时调用，用于加载用户数据、初始化日志文件等
	 */
	virtual void on_init() override;

	/**
	 * @brief Tick数据回调
	 * @param stdCode 标准化合约代码
	 * @param newTick 新到的Tick数据
	 * @details 当新的Tick数据到达时调用，用于处理行情数据和更新动态盈亏
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

	/**
	 * @brief 委托队列数据回调
	 * @param stdCode 标准化合约代码
	 * @param newOrdQue 新到的委托队列数据
	 * @details 当新的委托队列数据到达时调用，用于分析市场深度
	 */
	virtual void on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override;

	/**
	 * @brief 委托明细数据回调
	 * @param stdCode 标准化合约代码
	 * @param newOrdDtl 新到的委托明细数据
	 * @details 当新的委托明细数据到达时调用，用于分析市场交易行为
	 */
	virtual void on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override;

	/**
	 * @brief 成交明细数据回调
	 * @param stdCode 标准化合约代码
	 * @param newTrans 新到的成交明细数据
	 * @details 当新的成交明细数据到达时调用，用于分析市场成交情况
	 */
	virtual void on_transaction(const char* stdCode, WTSTransData* newTrans) override;

	/**
	 * @brief K线数据回调
	 * @param stdCode 标准化合约代码
	 * @param period 周期标识
	 * @param times 周期倍数
	 * @param newBar 新到的K线数据
	 * @details 当新的K线数据到达时调用，用于技术分析和策略决策
	 */
	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

	/**
	 * @brief 交易日开始回调
	 * @param uTDate 交易日日期
	 * @details 在每个交易日开始时调用，用于重置日内数据和状态
	 */
	virtual void on_session_begin(uint32_t uTDate) override;

	/**
	 * @brief 交易日结束回调
	 * @param uTDate 交易日日期
	 * @details 在每个交易日结束时调用，用于保存用户数据和清理资源
	 */
	virtual void on_session_end(uint32_t uTDate) override;

	/**
	 * @brief 撤单接口
	 * @param localid 本地订单ID
	 * @return 撤单是否成功
	 * @details 根据本地订单ID撤销特定订单
	 */
	virtual bool stra_cancel(uint32_t localid) override;

	/**
	 * @brief 批量撤单接口
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买单
	 * @param qty 数量
	 * @return 被撤销订单的ID列表
	 * @details 根据合约代码、方向和数量批量撤销订单
	 */
	virtual OrderIDs stra_cancel(const char* stdCode, bool isBuy, double qty) override;

	/**
	 * @brief 下单接口: 买入
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0则是市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于跟踪订单
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（剣除单），2-fok（全成全撤单），默认0
	 * @param bForceClose 是否强制平仓，默认false
	 * @return 订单ID列表
	 * @details 发送买入指令，可以是开仓也可以是平仓，系统会自动判断
	 */
	virtual OrderIDs stra_buy(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) override;

	/**
	 * @brief 下单接口: 卖出
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0则是市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于跟踪订单
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（剣除单），2-fok（全成全撤单），默认0
	 * @param bForceClose 是否强制平仓，默认false
	 * @return 订单ID列表
	 * @details 发送卖出指令，可以是开仓也可以是平仓，系统会自动判断
	 */
	virtual OrderIDs stra_sell(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) override;

	/**
	 * @brief 下单接口: 开多
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0则是市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于跟踪订单
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（剣除单），2-fok（全成全撤单），默认0
	 * @return 本地订单ID
	 * @details 发送开多（买入开仓）指令，开仓买入合约
	 */
	virtual uint32_t	stra_enter_long(const char* stdCode, double price, double qty, const char* userTag, int flag = 0) override;

	/**
	 * @brief 下单接口: 开空
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0则是市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于跟踪订单
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（剣除单），2-fok（全成全撤单），默认0
	 * @return 本地订单ID
	 * @details 发送开空（卖出开仓）指令，开仓卖出合约
	 */
	virtual uint32_t	stra_enter_short(const char* stdCode, double price, double qty, const char* userTag, int flag = 0) override;

	/**
	 * @brief 下单接口: 平多
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0则是市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于跟踪订单
	 * @param isToday 是否平今仓，默认false（先平昨仓）
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（剣除单），2-fok（全成全撤单），默认0
	 * @return 本地订单ID
	 * @details 发送平多（卖出平仓）指令，平仓之前的多头仓位
	 */
	virtual uint32_t	stra_exit_long(const char* stdCode, double price, double qty, const char* userTag, bool isToday = false, int flag = 0) override;

	/**
	 * @brief 下单接口: 平空
	 * @param stdCode 标准化合约代码
	 * @param price 下单价格，0则是市价单
	 * @param qty 下单数量
	 * @param userTag 用户自定义标签，用于跟踪订单
	 * @param isToday 是否平今仓，默认false（先平昨仓）
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（剣除单），2-fok（全成全撤单），默认0
	 * @return 本地订单ID
	 * @details 发送平空（买入平仓）指令，平仓之前的空头仓位
	 */
	virtual uint32_t	stra_exit_short(const char* stdCode, double price, double qty, const char* userTag, bool isToday = false, int flag = 0) override;

	/**
	 * @brief 获取合约信息
	 * @param stdCode 标准化合约代码
	 * @return 合约信息指针
	 * @details 获取指定合约的详细信息，包括合约代码、合约名称、交易所、合约乘数等
	 */
	virtual WTSCommodityInfo* stra_get_comminfo(const char* stdCode) override;

	/**
	 * @brief 获取K线数据
	 * @param stdCode 标准化合约代码
	 * @param period 周期标识，如"m1"/"m5"/"d1"等
	 * @param count 请求的K线数量
	 * @return K线数据切片指针
	 * @details 获取指定合约的历史K线数据，用于技术分析和策略计算
	 */
	virtual WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;

	/**
	 * @brief 获取Tick数据
	 * @param stdCode 标准化合约代码
	 * @param count 请求的Tick数量
	 * @return Tick数据切片指针
	 * @details 获取指定合约的历史Tick数据，用于微观市场分析
	 */
	virtual WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托明细数据
	 * @param stdCode 标准化合约代码
	 * @param count 请求的委托明细数量
	 * @return 委托明细数据切片指针
	 * @details 获取指定合约的历史委托明细数据，用于分析市场交易行为
	 */
	virtual WTSOrdDtlSlice*	stra_get_order_detail(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取委托队列数据
	 * @param stdCode 标准化合约代码
	 * @param count 请求的委托队列数量
	 * @return 委托队列数据切片指针
	 * @details 获取指定合约的历史委托队列数据，用于分析市场深度
	 */
	virtual WTSOrdQueSlice*	stra_get_order_queue(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取成交明细数据
	 * @param stdCode 标准化合约代码
	 * @param count 请求的成交明细数量
	 * @return 成交明细数据切片指针
	 * @details 获取指定合约的历史成交明细数据，用于分析市场成交情况
	 */
	virtual WTSTransSlice*	stra_get_transaction(const char* stdCode, uint32_t count) override;

	/**
	 * @brief 获取最新Tick数据
	 * @param stdCode 标准化合约代码
	 * @return 最新Tick数据指针
	 * @details 获取指定合约的最新市场行情数据
	 */
	virtual WTSTickData* stra_get_last_tick(const char* stdCode) override;

	/**
	 * @brief 获取分月合约代码
	 * @param stdCode 标准化合约代码
	 * @return 原始分月合约代码
	 * @details 将标准化合约代码转换为原始的分月合约代码，如SHFE.rb.2101
	 */
	virtual std::string		stra_get_rawcode(const char* stdCode) override;

	/**
	 * @brief 记录信息级别日志
	 * @param message 日志消息
	 * @details 记录信息级别的日志消息，用于记录策略的正常运行信息
	 */
	virtual void stra_log_info(const char* message) override;

	/**
	 * @brief 记录调试级别日志
	 * @param message 日志消息
	 * @details 记录调试级别的日志消息，用于记录策略的详细调试信息
	 */
	virtual void stra_log_debug(const char* message) override;

	/**
	 * @brief 记录警告级别日志
	 * @param message 日志消息
	 * @details 记录警告级别的日志消息，用于记录策略的非关键异常情况
	 */
	virtual void stra_log_warn(const char* message) override;

	/**
	 * @brief 记录错误级别日志
	 * @param message 日志消息
	 * @details 记录错误级别的日志消息，用于记录策略的关键错误和异常
	 */
	virtual void stra_log_error(const char* message) override;

	/**
	 * @brief 获取持仓量
	 * @param stdCode 标准化合约代码
	 * @param bOnlyValid 是否只返回可用持仓，默认false
	 * @param flag 持仓方向标志，1-多头，2-空头，3-多空合计，默认3
	 * @return 持仓量
	 * @details 获取指定合约的持仓量，可以指定方向和是否只返回可用持仓
	 */
	virtual double stra_get_position(const char* stdCode, bool bOnlyValid = false, int flag = 3) override;

	/**
	 * @brief 获取持仓均价
	 * @param stdCode 标准化合约代码
	 * @return 持仓均价
	 * @details 获取指定合约的持仓均价，用于计算盈亏
	 */
	virtual double stra_get_position_avgpx(const char* stdCode) override;

	/**
	 * @brief 获取持仓盈亏
	 * @param stdCode 标准化合约代码
	 * @return 持仓盈亏
	 * @details 获取指定合约的持仓盈亏，包括已实现和未实现盈亏
	 */
	virtual double stra_get_position_profit(const char* stdCode) override;

	/**
	 * @brief 获取合约最新价格
	 * @param stdCode 标准化合约代码
	 * @return 最新价格
	 * @details 获取指定合约的最新价格，用于计算盈亏和下单决策
	 */
	virtual double stra_get_price(const char* stdCode) override;

	/**
	 * @brief 获取未完成委托数量
	 * @param stdCode 标准化合约代码
	 * @return 未完成委托数量
	 * @details 获取指定合约的未完成委托数量，用于控制委托量
	 */
	virtual double stra_get_undone(const char* stdCode) override;

	/**
	 * @brief 获取当前交易日期
	 * @return 交易日期，格式为YYYYMMDD
	 * @details 获取当前交易系统的交易日期，用于日期相关的策略计算
	 */
	virtual uint32_t stra_get_date() override;

	/**
	 * @brief 获取当前交易时间
	 * @return 交易时间，格式为HHMMSS
	 * @details 获取当前交易系统的交易时间，用于时间相关的策略计算
	 */
	virtual uint32_t stra_get_time() override;

	/**
	 * @brief 获取当前交易秒数
	 * @return 当前秒数，从0点开始的秒数
	 * @details 获取当前交易系统的秒数，用于高精度时间相关的策略计算
	 */
	virtual uint32_t stra_get_secs() override;

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的Tick数据，订阅后可以接收到on_tick回调
	 */
	virtual void stra_sub_ticks(const char* stdCode) override;

	/**
	 * @brief 订阅委托明细数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的委托明细数据，订阅后可以接收到on_order_detail回调
	 */
	virtual void stra_sub_order_details(const char* stdCode) override;

	/**
	 * @brief 订阅委托队列数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的委托队列数据，订阅后可以接收到on_order_queue回调
	 */
	virtual void stra_sub_order_queues(const char* stdCode) override;

	/**
	 * @brief 订阅成交明细数据
	 * @param stdCode 标准化合约代码
	 * @details 订阅指定合约的成交明细数据，订阅后可以接收到on_transaction回调
	 */
	virtual void stra_sub_transactions(const char* stdCode) override;

	/**
	 * @brief 保存用户数据
	 * @param key 数据键
	 * @param val 数据值
	 * @details 保存用户自定义数据，用于存储策略状态和配置信息
	 */
	virtual void stra_save_user_data(const char* key, const char* val) override;

	/**
	 * @brief 加载用户数据
	 * @param key 数据键
	 * @param defVal 默认值，当数据不存在时返回，默认为空字符串
	 * @return 数据值
	 * @details 加载用户自定义数据，用于恢复策略状态和配置信息
	 */
	virtual const char* stra_load_user_data(const char* key, const char* defVal = "") override;

	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief 成交回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买单
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @details 当订单成交时触发此回调，用于处理成交事件和更新持仓信息
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;

	/**
	 * @brief 订单状态回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param isBuy 是否为买单
	 * @param totalQty 总委托数量
	 * @param leftQty 剩余数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 * @details 当订单状态发生变化时触发此回调，用于跟踪订单状态
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled) override;

	/**
	 * @brief 交易通道就绪回调
	 * @details 当交易通道就绪时触发此回调，表示可以开始交易
	 */
	virtual void on_channel_ready() override;

	/**
	 * @brief 交易通道断开回调
	 * @details 当交易通道断开时触发此回调，表示交易通道已断开，无法交易
	 */
	virtual void on_channel_lost() override;

	/**
	 * @brief 委托回报回调
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托回报消息
	 * @details 当委托发出后收到交易所回报时触发此回调，用于处理委托结果
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;

	/**
	 * @brief 持仓变化回调
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头持仓
	 * @param prevol 之前的持仓量
	 * @param preavail 之前的可用持仓量
	 * @param newvol 新的持仓量
	 * @param newavail 新的可用持仓量
	 * @param tradingday 交易日
	 * @details 当持仓发生变化时触发此回调，用于更新持仓信息
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override;

protected:
	/**
	 * @brief 调试日志模板函数
	 * @tparam Args 参数类型包
	 * @param format 格式化字符串
	 * @param args 参数列表
	 * @details 提供格式化的调试日志记录功能，支持类似 printf 的格式化语法
	 */
	template<typename... Args>
	void log_debug(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_debug(buffer);
	}

	/**
	 * @brief 信息日志模板函数
	 * @tparam Args 参数类型包
	 * @param format 格式化字符串
	 * @param args 参数列表
	 * @details 提供格式化的信息日志记录功能，支持类似 printf 的格式化语法
	 */
	template<typename... Args>
	void log_info(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_info(buffer);
	}

	/**
	 * @brief 错误日志模板函数
	 * @tparam Args 参数类型包
	 * @param format 格式化字符串
	 * @param args 参数列表
	 * @details 提供格式化的错误日志记录功能，支持类似 printf 的格式化语法
	 */
	template<typename... Args>
	void log_error(const char* format, const Args& ...args)
	{
		const char* buffer = fmtutil::format(format, args...);
		stra_log_error(buffer);
	}

protected:
	/**
	 * @brief 获取内部合约代码
	 * @param stdCode 标准化合约代码
	 * @return 内部合约代码
	 * @details 将标准化合约代码转换为内部使用的合约代码
	 */
	const char* get_inner_code(const char* stdCode);

	/**
	 * @brief 加载用户数据
	 * @details 从存储中加载用户自定义数据，用于恢复策略状态
	 */
	void	load_userdata();

	/**
	 * @brief 保存用户数据
	 * @details 将用户自定义数据保存到存储中，用于持久化策略状态
	 */
	void	save_userdata();

	/**
	 * @brief 初始化输出日志
	 * @details 初始化策略的各种日志文件，包括信号日志、平仓日志、交易日志和资金日志
	 */
	void	init_outputs();

	/**
	 * @brief 设置持仓量
	 * @param stdCode 标准化合约代码
	 * @param qty 持仓数量，正数表示多头，负数表示空头
	 * @param price 持仓价格，默认0.0
	 * @param userTag 用户自定义标签，默认为空
	 * @details 设置指定合约的持仓量，用于策略的持仓管理
	 */
	void	do_set_position(const char* stdCode, double qty, double price = 0.0, const char* userTag = "");

	/**
	 * @brief 更新动态盈亏
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 * @details 根据最新的Tick数据更新指定合约的动态盈亏
	 */
	void	update_dyn_profit(const char* stdCode, WTSTickData* newTick);

	/**
	 * @brief 记录交易日志
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头
	 * @param isOpen 是否为开仓
	 * @param curTime 当前时间
	 * @param price 交易价格
	 * @param qty 交易数量
	 * @param fee 交易手续费
	 * @param userTag 用户自定义标签
	 * @details 记录交易日志，包括开仓和平仓交易的详细信息
	 */
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee, const char* userTag);

	/**
	 * @brief 记录平仓日志
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头
	 * @param openTime 开仓时间
	 * @param openpx 开仓价格
	 * @param closeTime 平仓时间
	 * @param closepx 平仓价格
	 * @param qty 交易数量
	 * @param profit 本次交易盈亏
	 * @param maxprofit 最大盈利
	 * @param maxloss 最大亏损
	 * @param totalprofit 总盈亏
	 * @param enterTag 开仓标签
	 * @param exitTag 平仓标签
	 * @details 记录平仓日志，包括开平仓价格、盈亏等详细信息
	 */
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double maxprofit, double maxloss, double totalprofit, const char* enterTag, const char* exitTag);

	/**
	 * @brief 获取订单标签
	 * @param localid 本地订单ID
	 * @return 订单标签字符串
	 * @details 根据本地订单ID获取用户自定义标签，用于跟踪订单的目的和来源
	 */
	inline const char* getOrderTag(uint32_t localid)
	{
		thread_local static OrderTag oTag;
		oTag._localid = localid;
		auto it = std::lower_bound(_orders.begin(), _orders.end(), oTag, [](const OrderTag& a, const OrderTag& b) {
			return a._localid < b._localid;
		});

		if (it == _orders.end())
			return "";

		return (*it)._usertag;
	}


	/**
	 * @brief 设置订单用户标签
	 * @param localid 本地订单ID
	 * @param usertag 用户自定义标签
	 * @details 为指定的订单设置用户自定义标签，用于跟踪订单的目的和来源
	 */
	inline void setUserTag(uint32_t localid, const char* usertag)
	{
		_orders.push_back({ localid, usertag });
	}

	/**
	 * @brief 删除订单标签
	 * @param localid 本地订单ID
	 * @details 删除指定订单的用户自定义标签，通常在订单完成或撤销后调用
	 */
	inline void eraseOrderTag(uint32_t localid)
	{
		thread_local static OrderTag oTag;
		oTag._localid = localid;
		auto it = std::lower_bound(_orders.begin(), _orders.end(), oTag, [](const OrderTag& a, const OrderTag& b) {
			return a._localid < b._localid;
		});

		if (it == _orders.end())
			return;

		_orders.erase(it);
	}

protected:
	uint32_t		_context_id;
	WtHftEngine*	_engine;
	TraderAdapter*	_trader;
	int32_t			_slippage;

	wt_hashmap<std::string, std::string> _code_map;

	BoostFilePtr	_sig_logs;
	BoostFilePtr	_close_logs;
	BoostFilePtr	_trade_logs;
	BoostFilePtr	_fund_logs;

	//用户数据
	typedef wt_hashmap<std::string, std::string> StringHashMap;
	StringHashMap	_user_datas;
	bool			_ud_modified;

	bool			_data_agent;	//数据托管

	//tick订阅列表
	wt_hashset<std::string> _tick_subs;

private:
	/**
	 * @brief 仓位明细信息结构体
	 * @details 存储交易明细信息，包括方向、价格、数量、时间和盈亏等
	 */
	typedef struct _DetailInfo
	{
		bool		_long;      ///< 是否为多头仓位
		double		_price;     ///< 开仓价格
		double		_volume;    ///< 仓位数量
		uint64_t	_opentime;  ///< 开仓时间
		uint32_t	_opentdate; ///< 开仓交易日
		double		_max_profit; ///< 最大盈利
		double		_max_loss;   ///< 最大亏损
		double		_profit;     ///< 当前盈亏
		char		_usertag[32]; ///< 用户自定义标签

		/**
		 * @brief 构造函数
		 * @details 初始化所有成员变量为0
		 */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
	 * @brief 持仓信息结构体
	 * @details 存储合约的持仓信息，包括持仓量、平仓盈亏、动态盈亏和明细信息
	 */
	typedef struct _PosInfo
	{
		double		_volume;      ///< 持仓量
		double		_closeprofit; ///< 平仓盈亏（已实现盈亏）
		double		_dynprofit;   ///< 动态盈亏（未实现盈亏）

		std::vector<DetailInfo> _details; ///< 持仓明细列表

		/**
		 * @brief 构造函数
		 * @details 初始化所有成员变量为0
		 */
		_PosInfo()
		{
			_volume = 0;
			_closeprofit = 0;
			_dynprofit = 0;
		}
	} PosInfo;
	typedef wt_hashmap<std::string, PosInfo> PositionMap;
	PositionMap		_pos_map;

	/**
	 * @brief 订单标签结构体
	 * @details 存储订单的标签信息，包括订单ID和用户自定义标签
	 */
	typedef struct _OrderTag
	{
		uint32_t	_localid;   ///< 本地订单ID
		char		_usertag[64] = { 0 }; ///< 用户自定义标签

		/**
		 * @brief 默认构造函数
		 */
		_OrderTag(){}

		/**
		 * @brief 构造函数
		 * @param localid 本地订单ID
		 * @param usertag 用户自定义标签
		 * @details 初始化订单标签结构体
		 */
		_OrderTag(uint32_t localid, const char* usertag)
		{
			_localid = localid;
			wt_strcpy(_usertag, usertag);
		}
	} OrderTag;
	//typedef wt_hashmap<uint32_t, std::string> OrderMap;
	//OrderMap		_orders;
	boost::circular_buffer<OrderTag> _orders;

	/**
	 * @brief 策略资金信息结构体
	 * @details 存储策略的资金信息，包括总盈亏、动态盈亏和手续费
	 */
	typedef struct _StraFundInfo
	{
		double	_total_profit;    ///< 总盈亏（已实现）
		double	_total_dynprofit; ///< 总动态盈亏（未实现）
		double	_total_fees;      ///< 总手续费

		/**
		 * @brief 构造函数
		 * @details 初始化所有成员变量为0
		 */
		_StraFundInfo()
		{
			memset(this, 0, sizeof(_StraFundInfo));
		}
	} StraFundInfo;

	StraFundInfo		_fund_info;

	typedef wt_hashmap<std::string, double> PriceMap;
	PriceMap		_price_map;
};

NS_WTP_END
