/*!
 * \file EventNotifier.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 事件通知器对象定义
 * 
 * 本文件定义了交易系统中的事件通知器，用于向外部系统广播交易日志、订单和成交信息。
 * 通过消息队列服务实现与外部系统的异步通信，支持多种通知类型。
 */
#pragma once

#include <boost/asio/io_service.hpp>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/StdUtils.hpp"

/**
 * @brief 创建消息队列服务器函数指针类型
 * @param url 消息队列服务器URL
 * @return unsigned long 服务器标识ID
 */
typedef unsigned long(*FuncCreateMQServer)(const char*);

/**
 * @brief 销毁消息队列服务器函数指针类型
 * @param sid 服务器标识ID
 */
typedef void(*FuncDestroyMQServer)(unsigned long);

/**
 * @brief 发布消息函数指针类型
 * @param sid 服务器标识ID
 * @param topic 消息主题
 * @param data 消息数据
 * @param dataLen 消息数据长度
 */
typedef void(*FundPublishMessage)(unsigned long, const char*, const char*, unsigned long);

/**
 * @brief 日志回调函数指针类型
 * @param id 服务器标识ID
 * @param message 日志消息
 * @param bServer 是否为服务器端日志
 */
typedef void(*FuncLogCallback)(unsigned long, const char*, bool);

/**
 * @brief 注册回调函数指针类型
 * @param cbLog 日志回调函数
 */
typedef void(*FuncRegCallbacks)(FuncLogCallback);


NS_WTP_BEGIN
class WTSTradeInfo;
class WTSOrderInfo;
class WTSVariant;

/**
 * @brief 事件通知器类
 * 
 * 负责收集并向外部系统广播交易相关事件，包括交易日志、订单信息、成交信息和系统事件等。
 * 使用消息队列服务实现与外部系统的异步通信，通过动态加载库支持多种消息队列实现。
 */
class EventNotifier
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * 初始化事件通知器对象，设置初始状态
	 */
	EventNotifier();

	/**
	 * @brief 析构函数
	 * 
	 * 清理事件通知器资源，包括关闭消息队列连接并终止工作线程
	 */
	~EventNotifier();

private:
	/**
	 * @brief 将成交信息转换为JSON格式
	 * 
	 * @param trader 交易器名称
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param trdInfo 成交信息对象
	 * @param output 输出的JSON字符串
	 */
	void	tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output);

	/**
	 * @brief 将订单信息转换为JSON格式
	 * 
	 * @param trader 交易器名称
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param ordInfo 订单信息对象
	 * @param output 输出的JSON字符串
	 */
	void	orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output);

public:
	/**
	 * @brief 初始化事件通知器
	 * 
	 * 根据配置加载并初始化消息队列服务器，创建异步处理线程
	 * 
	 * @param cfg 配置对象，包含消息队列服务器URL等信息
	 * @return bool 初始化是否成功
	 */
	bool	init(WTSVariant* cfg);

	/**
	 * @brief 通知成交信息
	 * 
	 * 将成交信息转换为JSON并通过消息队列发送
	 * 
	 * @param trader 交易器名称
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param trdInfo 成交信息对象
	 */
	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);

	/**
	 * @brief 通知订单信息
	 * 
	 * 将订单信息转换为JSON并通过消息队列发送
	 * 
	 * @param trader 交易器名称
	 * @param localid 本地订单ID
	 * @param stdCode 标准化合约代码
	 * @param ordInfo 订单信息对象
	 */
	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);

	/**
	 * @brief 通知交易消息
	 * 
	 * 将交易相关消息转换为JSON并通过消息队列发送
	 * 
	 * @param trader 交易器名称
	 * @param message 消息内容
	 */
	void	notify(const char* trader, const char* message);

	/**
	 * @brief 通知日志信息
	 * 
	 * 将日志信息转换为JSON并通过消息队列发送
	 * 
	 * @param tag 日志标签
	 * @param message 日志内容
	 */
	void	notify_log(const char* tag, const char* message);

	/**
	 * @brief 通知事件信息
	 * 
	 * 将系统事件信息转换为JSON并通过消息队列发送
	 * 
	 * @param message 事件内容
	 */
	void	notify_event(const char* message);

private:
	std::string		_url;			/**< 消息队列服务器URL */
	uint32_t		_mq_sid;		/**< 消息队列服务器标识ID */
	FuncCreateMQServer	_creator;		/**< 创建消息队列服务器函数指针 */
	FuncDestroyMQServer	_remover;		/**< 销毁消息队列服务器函数指针 */
	FundPublishMessage	_publisher;		/**< 发布消息函数指针 */
	FuncRegCallbacks	_register;		/**< 注册回调函数函数指针 */

	bool			_stopped;		/**< 线程停止标志 */
	boost::asio::io_service		_asyncio;		/**< 异步IO服务对象 */
	StdThreadPtr				_worker;		/**< 工作线程指针 */
};

NS_WTP_END