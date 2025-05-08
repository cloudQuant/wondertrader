/*!
 * \file MQManager.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 消息队列管理器实现
 */
#include "MQManager.h"

#include <spdlog/fmt/fmt.h>

USING_NS_WTP;

/**
 * @brief 创建消息队列服务端实现
 * @details 创建并初始化一个新的消息队列服务端实例
 *          将创建的服务端实例存储到服务端映射中
 * @param url 服务端绑定的地址，格式如"tcp://0.0.0.0:5555"
 * @param confirm 是否需要消息确认
 * @return 新创建的服务端ID
 */
WtUInt32 MQManager::create_server(const char* url, bool confirm)
{
	MQServerPtr server(new MQServer(this));

	printf("init server\r\n");
	server->init(url, confirm);

	auto id = server->id();

	_servers[id] = server;
	return id;
}

/**
 * @brief 销毁消息队列服务端实现
 * @details 销毁指定的消息队列服务端实例，释放相关资源
 *          如果服务端不存在，则记录错误日志并返回
 * @param id 要销毁的服务端ID
 */
void MQManager::destroy_server(WtUInt32 id)
{
	auto it = _servers.find(id);
	if(it == _servers.end())
	{
		log_server(id, fmt::format("MQServer {} not exists", id).c_str());
		return;
	}

	_servers.erase(it);
	log_server(id, fmt::format("MQServer {} has been destroyed", id).c_str());
}

/**
 * @brief 发布消息实现
 * @details 通过指定的服务端发布消息到指定的主题
 *          如果服务端不存在，则记录错误日志并返回
 * @param id 服务端ID
 * @param topic 消息主题
 * @param data 消息数据指针
 * @param dataLen 消息数据长度
 */
void MQManager::publish_message(WtUInt32 id, const char* topic, const void* data, WtUInt32 dataLen)
{
	auto it = _servers.find(id);
	if (it == _servers.end())
	{
		log_server(id, fmt::format("MQServer {} not exists", id).c_str());
		return;
	}

	MQServerPtr& server = (MQServerPtr&)it->second;
	server->publish(topic, data, dataLen);
}

/**
 * @brief 记录服务端日志实现
 * @details 调用日志回调函数记录服务端相关的日志信息
 *          如果日志回调函数未设置，则不进行日志记录
 * @param id 服务端ID
 * @param message 日志消息内容
 */
void MQManager::log_server(WtUInt32 id, const char* message)
{
	if (_cb_log)
		_cb_log(id, message, true);
}

/**
 * @brief 记录客户端日志实现
 * @details 调用日志回调函数记录客户端相关的日志信息
 *          如果日志回调函数未设置，则不进行日志记录
 * @param id 客户端ID
 * @param message 日志消息内容
 */
void MQManager::log_client(WtUInt32 id, const char* message)
{
	if (_cb_log)
		_cb_log(id, message, false);
}

/**
 * @brief 创建消息队列客户端实现
 * @details 创建并初始化一个新的消息队列客户端实例
 *          将创建的客户端实例存储到客户端映射中
 * @param url 要连接的服务器地址，格式如"tcp://127.0.0.1:5555"
 * @param cb 消息接收回调函数，用于处理收到的消息
 * @return 新创建的客户端ID
 */
WtUInt32 MQManager::create_client(const char* url, FuncMQCallback cb)
{
	MQClientPtr client(new MQClient(this));
	client->init(url, cb);

	auto id = client->id();

	_clients[id] = client;
	return id;
}

/**
 * @brief 销毁消息队列客户端实现
 * @details 销毁指定的消息队列客户端实例，释放相关资源
 *          如果客户端不存在，则记录错误日志并返回
 * @param id 要销毁的客户端ID
 */
void MQManager::destroy_client(WtUInt32 id)
{
	auto it = _clients.find(id);
	if (it == _clients.end())
	{
		log_client(id, fmt::format("MQClient {} not exists", id).c_str());
		return;
	}

	_clients.erase(it);
	log_client(id, fmt::format("MQClient {} has been destroyed", id).c_str());
}

/**
 * @brief 订阅主题实现
 * @details 为指定的客户端订阅特定的消息主题
 *          如果客户端不存在，则记录错误日志并返回
 * @param id 客户端ID
 * @param topic 要订阅的主题
 */
void MQManager::sub_topic(WtUInt32 id, const char* topic)
{
	auto it = _clients.find(id);
	if (it == _clients.end())
	{
		log_client(id, fmt::format("MQClient {} not exists", id).c_str());
		return;
	}

	MQClientPtr& client = (MQClientPtr&)it->second;
	client->sub_topic(topic);
}

/**
 * @brief 启动客户端实现
 * @details 启动指定的客户端，开始接收消息
 *          如果客户端不存在，则记录错误日志并返回
 * @param id 要启动的客户端ID
 */
void MQManager::start_client(WtUInt32 id)
{
	auto it = _clients.find(id);
	if (it == _clients.end())
	{
		log_client(id, fmt::format("MQClient {} not exists", id).c_str());
		return;
	}

	MQClientPtr& client = (MQClientPtr&)it->second;
	client->start();
}