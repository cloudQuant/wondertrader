/*!
 * \file WtMsgQue.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 消息队列模块对外接口定义
 * \details 定义了消息队列模块的C接口，包括服务端和客户端的创建、销毁、消息发布和订阅等功能
 */
#pragma once
#include "PorterDefs.h"

#ifdef __cplusplus
extern "C"
{
#endif
	/**
	 * @brief 注册日志回调函数
	 * @details 设置用于处理日志的回调函数，用于记录服务端和客户端的日志信息
	 * @param cbLog 日志回调函数，接收ID、消息和是否为服务端的标志
	 */
	EXPORT_FLAG void		regiter_callbacks(FuncLogCallback cbLog);

	/**
	 * @brief 创建消息队列服务端
	 * @details 创建并初始化一个新的消息队列服务端实例
	 * @param url 服务端绑定的地址，格式如"tcp://0.0.0.0:5555"
	 * @param confirm 是否需要消息确认
	 * @return 新创建的服务端ID
	 */
	EXPORT_FLAG WtUInt32	create_server(const char* url, bool confirm);
	/**
	 * @brief 销毁消息队列服务端
	 * @details 销毁指定的消息队列服务端实例，释放相关资源
	 * @param id 要销毁的服务端ID
	 */
	EXPORT_FLAG void		destroy_server(WtUInt32 id);
	/**
	 * @brief 发布消息
	 * @details 通过指定的服务端发布消息到指定的主题
	 * @param id 服务端ID
	 * @param topic 消息主题
	 * @param data 消息数据指针
	 * @param dataLen 消息数据长度
	 */
	EXPORT_FLAG void		publish_message(WtUInt32 id, const char* topic, const char* data, WtUInt32 dataLen);

	/**
	 * @brief 创建消息队列客户端
	 * @details 创建并初始化一个新的消息队列客户端实例
	 * @param url 要连接的服务器地址，格式如"tcp://127.0.0.1:5555"
	 * @param cb 消息接收回调函数，用于处理收到的消息
	 * @return 新创建的客户端ID
	 */
	EXPORT_FLAG WtUInt32	create_client(const char* url, FuncMQCallback cb);

	/**
	 * @brief 销毁消息队列客户端
	 * @details 销毁指定的消息队列客户端实例，释放相关资源
	 * @param id 要销毁的客户端ID
	 */
	EXPORT_FLAG void		destroy_client(WtUInt32 id);

	/**
	 * @brief 订阅主题
	 * @details 为指定的客户端订阅特定的消息主题
	 * @param id 客户端ID
	 * @param topic 要订阅的主题
	 */
	EXPORT_FLAG void		subscribe_topic(WtUInt32 id, const char* topic);

	/**
	 * @brief 启动客户端
	 * @details 启动指定的客户端，开始接收消息
	 * @param id 要启动的客户端ID
	 */
	EXPORT_FLAG void		start_client(WtUInt32 id);
#ifdef __cplusplus
}
#endif