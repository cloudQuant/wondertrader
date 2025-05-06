/**
 * @file ITrdNotifySink.h
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 交易通知接收器接口定义
 * @details 定义了交易系统中的各类回报通知接收器接口，包括成交、订单、仓位、交易通道状态等回调
 */
#pragma once
#include <stdint.h>
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN

/**
 * @brief 交易通知接收器接口
 * @details 用于接收各类交易相关通知和回报，实现交易引擎与策略组件的解耦
 */
class ITrdNotifySink
{
public:
	/**
	 * @brief 成交回报通知
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否买入，true为买入，false为卖出
	 * @param vol 成交数量
	 * @param price 成交价格
	 * @details 当委托成交后，交易引擎调用此接口将成交信息通知给相关组件
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) = 0;

	/**
	 * @brief 订单状态回报通知
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否买入，true为买入，false为卖出
	 * @param totalQty 委托总数量
	 * @param leftQty 剩余未成交数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销，true表示已撤销，false表示未撤销
	 * @details 当委托状态发生变化时，交易引擎调用此接口将最新状态通知给相关组件
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) = 0;

	/**
	 * @brief 持仓更新回调
	 * @param stdCode 标准合约代码
	 * @param isLong 是否为多头持仓，true为多头，false为空头
	 * @param prevol 前一交易日持仓量
	 * @param preavail 前一交易日可用持仓量
	 * @param newvol 最新持仓量
	 * @param newavail 最新可用持仓量
	 * @param tradingday 交易日，格式为YYYYMMDD
	 * @details 当持仓状态发生变化时，交易引擎调用此接口将最新持仓信息通知给相关组件
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) {}

	/**
	 * @brief 交易通道就绪通知
	 * @details 当交易通道连接成功并准备就绪可用时，交易引擎调用此接口通知相关组件
	 */
	virtual void on_channel_ready() = 0;

	/**
	 * @brief 交易通道丢失通知
	 * @details 当交易通道断开连接或发生异常时，交易引擎调用此接口通知相关组件
	 */
	virtual void on_channel_lost() = 0;

	/**
	 * @brief 下单回报通知
	 * @param localid 本地委托ID
	 * @param stdCode 标准合约代码
	 * @param bSuccess 是否下单成功，true表示成功，false表示失败
	 * @param message 回报消息，通常在下单失败时包含错误信息
	 * @details 当委托下单后收到确认或拒绝时，交易引擎调用此接口通知相关组件
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message){}

	/**
	 * @brief 资金账户更新回调
	 * @param currency 货币类型，如CNY、USD等
	 * @param prebalance 前一交易日结存
	 * @param balance 静态权益
	 * @param dynbalance 动态权益
	 * @param avaliable 可用资金
	 * @param closeprofit 平仓盈亏
	 * @param dynprofit 浮动盈亏
	 * @param margin 占用保证金
	 * @param fee 手续费
	 * @param deposit 入金
	 * @param withdraw 出金
	 * @details 当账户资金状态发生变化时，交易引擎调用此接口将最新资金信息通知给相关组件
	 */
	virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw){}
};

NS_WTP_END