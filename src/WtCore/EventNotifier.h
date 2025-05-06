/**
 * @file EventNotifier.h
 * @project WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 事件通知器定义
 * @details 定义了事件通知器类，用于发送各类交易相关的事件和日志
 *          通过消息队列将事件异步通知给客户端
 */
#pragma once

#include <boost/asio/io_service.hpp>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/StdUtils.hpp"

/**
 * @brief 创建消息队列服务器函数指针类型
 * @param url 服务器URL
 * @return 服务器ID
 */
typedef unsigned long(*FuncCreateMQServer)(const char*);
/**
 * @brief 销毁消息队列服务器函数指针类型
 * @param sid 服务器ID
 */
typedef void(*FuncDestroyMQServer)(unsigned long);
/**
 * @brief 发布消息函数指针类型
 * @param sid 服务器ID
 * @param topic 消息主题
 * @param data 消息数据
 * @param dataLen 数据长度
 */
typedef void(*FundPublishMessage)(unsigned long, const char*, const char*, unsigned long);

/**
 * @brief 日志回调函数指针类型
 * @param id 服务器ID
 * @param message 日志消息
 * @param bServer 是否服务器日志
 */
typedef void(*FuncLogCallback)(unsigned long, const char*, bool);
/**
 * @brief 注册回调函数指针类型
 * @param cbLogger 日志回调函数
 */
typedef void(*FuncRegCallbacks)(FuncLogCallback);


NS_WTP_BEGIN
class WTSTradeInfo;
class WTSOrderInfo;
class WTSVariant;

/**
 * @brief 事件通知器类
 * @details 负责处理并发送各类交易相关的事件和日志
 *          如订单事件、成交事件、图表标记、日志等
 *          通过消息队列将这些事件异步通知给客户端
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


private:
	/**
	 * @brief 将成交信息转换为JSON格式
	 * @param trader 交易者标识
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param trdInfo 成交信息对象
	 * @param output 输出的JSON字符串
	 */
	void	tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output);

	/**
	 * @brief 将订单信息转换为JSON格式
	 * @param trader 交易者标识
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param ordInfo 订单信息对象
	 * @param output 输出的JSON字符串
	 */
	void	orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output);

public:
	/**
	 * @brief 初始化事件通知器
	 * @param cfg 配置对象
	 * @return 是否初始化成功
	 */
	bool	init(WTSVariant* cfg);

	/**
	 * @brief 发送成交事件通知
	 * @param trader 交易者标识
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param trdInfo 成交信息对象
	 */
	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);

	/**
	 * @brief 发送订单事件通知
	 * @param trader 交易者标识
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param ordInfo 订单信息对象
	 */
	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);

	/**
	 * @brief 发送交易者消息通知
	 * @param trader 交易者标识
	 * @param message 消息内容
	 */
	void	notify(const char* trader, const char* message);

	/**
	 * @brief 发送日志通知
	 * @param tag 日志标签
	 * @param message 日志消息
	 */
	void	notify_log(const char* tag, const char* message);

	/**
	 * @brief 发送事件通知
	 * @param message 事件消息
	 */
	void	notify_event(const char* message);

	/**
	 * @brief 发送图表标记通知
	 * @param time 时间戳
	 * @param straId 策略ID
	 * @param price 价格
	 * @param icon 图标
	 * @param tag 标签
	 */
	void	notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag);

	/**
	 * @brief 发送图表指标通知
	 * @param time 时间戳
	 * @param straId 策略ID
	 * @param idxName 指标名称
	 * @param lineName 线条名称
	 * @param val 指标值
	 */
	void	notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val);

	/**
	 * @brief 发送策略交易信号通知
	 * @param straId 策略ID
	 * @param stdCode 标准合约代码
	 * @param isLong 是否做多
	 * @param isOpen 是否开仓
	 * @param curTime 当前时间戳
	 * @param price 交易价格
	 * @param userTag 用户标记
	 */
	void	notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag);

private:
	std::string		_url;				//!< 消息队列服务器URL
	uint32_t		_mq_sid;			//!< 消息队列服务器ID
	FuncCreateMQServer	_creator;		//!< 创建服务器函数指针
	FuncDestroyMQServer	_remover;		//!< 销毁服务器函数指针
	FundPublishMessage	_publisher;		//!< 发布消息函数指针
	FuncRegCallbacks	_register;		//!< 注册回调函数指针

	bool			_stopped;			//!< 是否停止工作线程
	boost::asio::io_service		_asyncio;	//!< asio异步IO服务
	StdThreadPtr				_worker;	//!< 工作线程指针
};

NS_WTP_END