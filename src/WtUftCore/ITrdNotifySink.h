/*!
 * \file ITrdNotifySink.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易通知接收器接口定义
 * 
 * 本文件定义了交易通知接收器的接口，用于接收并处理交易系统中的成交和订单回报。
 * 策略模块通过实现该接口来处理交易信息。
 */
#pragma once
#include <stdint.h>
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN

/**
 * @brief 交易通知接收器接口
 * 
 * 定义了交易系统的各类回调接口，包括成交回报、订单回报、交易通道状态等
 * 策略模块需要实现该接口以接收交易相关信息
 */
class ITrdNotifySink
{
public:
	/**
	 * @brief 成交回报回调函数
	 * 
	 * 当有新的成交信息时，交易系统会调用此函数通知策略
	 * 
	 * @param localid 本地订单ID，用于识别订单
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头方向，true为多头，false为空头
	 * @param offset 开平仓标志，参考WTSDefine.h中的WTSOffsetType
	 * @param vol 成交数量
	 * @param price 成交价格
	 */
	virtual void on_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price) = 0;

	/**
	 * @brief 订单回报回调函数
	 * 
	 * 当订单状态发生变化时，交易系统会调用此函数通知策略
	 * 
	 * @param localid 本地订单ID，用于识别订单
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头方向，true为多头，false为空头
	 * @param offset 开平仓标志，参考WTSDefine.h中的WTSOffsetType
	 * @param totalQty 总数量
	 * @param leftQty 剩余未成交数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤单，true表示已撤销，false表示未撤销
	 */
	virtual void on_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled = false) = 0;

	/**
	 * @brief 持仓更新回调函数
	 * 
	 * 当持仓状态发生变化时，交易系统会调用此函数通知策略
	 * 
	 * @param stdCode 标准化合约代码
	 * @param isLong 是否为多头持仓，true为多头，false为空头
	 * @param prevol 先前的持仓总量
	 * @param preavail 先前的可用持仓量
	 * @param newvol 新的持仓总量
	 * @param newavail 新的可用持仓量
	 * @param tradingday 交易日
	 */
	virtual void on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) {}

	/**
	 * @brief 交易通道就绪回调函数
	 * 
	 * 当交易通道连接成功并准备就绪时，交易系统会调用此函数通知策略
	 * 
	 * @param tradingday 当前交易日
	 */
	virtual void on_channel_ready(uint32_t tradingday) = 0;

	/**
	 * @brief 交易通道丢失回调函数
	 * 
	 * 当交易通道连接断开或发生异常时，交易系统会调用此函数通知策略
	 */
	virtual void on_channel_lost() = 0;

	/**
	 * @brief 下单回报回调函数
	 * 
	 * 当下单指令发送后收到回报时，交易系统会调用此函数通知策略
	 * 
	 * @param localid 本地订单ID，用于识别订单
	 * @param stdCode 标准化合约代码
	 * @param bSuccess 下单是否成功，true表示成功，false表示失败
	 * @param message 回报消息，并常包含错误信息（当失败时）
	 */
	virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message){}
};

NS_WTP_END