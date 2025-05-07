/*!
 * \file EventNotifier.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 事件通知器对象定义，用于回测过程中的事件和数据广播
 */
#pragma once

#include <queue>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/StdUtils.hpp"

/**
 * @brief 创建消息队列服务器的函数类型
 * @param 服务器URL地址
 * @param 是否为服务器模式
 * @return 服务器ID
 */
typedef unsigned long(*FuncCreateMQServer)(const char*, bool);

/**
 * @brief 销毁消息队列服务器的函数类型
 * @param 服务器ID
 */
typedef void(*FuncDestroyMQServer)(unsigned long);

/**
 * @brief 发布消息的函数类型
 * @param 服务器ID
 * @param 主题
 * @param 消息内容
 * @param 消息长度
 */
typedef void(*FundPublishMessage)(unsigned long, const char*, const char*, unsigned long);

/**
 * @brief 日志回调函数类型
 * @param 服务器ID
 * @param 日志消息
 * @param 是否为服务器
 */
typedef void(*FuncLogCallback)(unsigned long, const char*, bool);

/**
 * @brief 注册回调函数的函数类型
 * @param 日志回调函数
 */
typedef void(*FuncRegCallbacks)(FuncLogCallback);


NS_WTP_BEGIN
class WTSVariant;

/**
 * @brief 事件通知器类，用于回测过程中的事件、数据和资金信息广播
 * @details 该类封装了消息队列服务器的创建、销毁和消息发布功能，便于其他模块获取回测过程中的信息
 */
class EventNotifier
{
public:
	/**
	 * @brief 构造函数
	 */
	EventNotifier();

	/**
	 * @brief 析构函数
	 */
	~EventNotifier();

public:
	/**
	 * @brief 初始化事件通知器
	 * @param cfg 配置项，包含消息队列服务器的URL等信息
	 * @return 是否初始化成功
	 */
	bool	init(WTSVariant* cfg);

	/**
	 * @brief 通知事件
	 * @param evtType 事件类型
	 */
	void	notifyEvent(const char* evtType);

	/**
	 * @brief 通知数据
	 * @param topic 主题
	 * @param data 数据内容
	 * @param dataLen 数据长度
	 */
	void	notifyData(const char* topic, void* data , uint32_t dataLen);

	/**
	 * @brief 通知资金信息
	 * @param topic 主题
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param total_profit 总盈亏
	 * @param dynprofit 动态盈亏
	 * @param dynbalance 动态余额
	 * @param total_fee 总手续费
	 */
	void	notifyFund(const char* topic, uint32_t uDate, double total_profit, double dynprofit, double dynbalance, double total_fee);
	

private:
	std::string		m_strURL;        ///< 消息队列服务器的URL地址
	uint32_t		_mq_sid;         ///< 消息队列服务器ID
	FuncCreateMQServer	_creator;    ///< 创建消息队列服务器的函数指针
	FuncDestroyMQServer	_remover;    ///< 销毁消息队列服务器的函数指针
	FundPublishMessage	_publisher;  ///< 发布消息的函数指针
	FuncRegCallbacks	_register;   ///< 注册回调函数的函数指针
};

NS_WTP_END