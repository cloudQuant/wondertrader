/*!
 * \file WtMsgQue.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 消息队列模块对外接口实现
 * \details 实现了消息队列模块的C接口，通过单例模式的MQManager实现服务端和客户端的管理
 */
#include "WtMsgQue.h"
#include "MQManager.h"

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
#pragma comment(lib, "nanomsg.lib")
#endif

USING_NS_WTP;

/**
 * @brief 获取MQManager单例
 * @details 使用单例模式创建并返回MQManager对象，确保整个应用中只有一个实例
 * @return MQManager单例的引用
 */
MQManager& getMgr()
{
	static MQManager runner;
	return runner;
}

/**
 * @brief 注册日志回调函数实现
 * @details 将日志回调函数传递给MQManager单例
 * @param cbLog 日志回调函数，接收ID、消息和是否为服务端的标志
 */
void regiter_callbacks(FuncLogCallback cbLog)
{
	getMgr().regiter_callbacks(cbLog);
}

/**
 * @brief 创建消息队列服务端实现
 * @details 通过MQManager单例创建一个新的消息队列服务端
 * @param url 服务端绑定的地址，格式如"tcp://0.0.0.0:5555"
 * @param confirm 是否需要消息确认
 * @return 新创建的服务端ID
 */
WtUInt32 create_server(const char* url, bool confirm)
{
	printf("create server\r\n");
	return getMgr().create_server(url, confirm);
}

/**
 * @brief 销毁消息队列服务端实现
 * @details 通过MQManager单例销毁指定的消息队列服务端
 * @param id 要销毁的服务端ID
 */
void destroy_server(WtUInt32 id)
{
	getMgr().destroy_server(id);
}

/**
 * @brief 发布消息实现
 * @details 通过MQManager单例发布消息到指定的主题
 * @param id 服务端ID
 * @param topic 消息主题
 * @param data 消息数据指针
 * @param dataLen 消息数据长度
 */
void publish_message(WtUInt32 id, const char* topic, const char* data, WtUInt32 dataLen)
{
	getMgr().publish_message(id, topic, data, dataLen);
}

/**
 * @brief 创建消息队列客户端实现
 * @details 通过MQManager单例创建一个新的消息队列客户端
 * @param url 要连接的服务器地址，格式如"tcp://127.0.0.1:5555"
 * @param cb 消息接收回调函数，用于处理收到的消息
 * @return 新创建的客户端ID
 */
WtUInt32 create_client(const char* url, FuncMQCallback cb)
{
	return getMgr().create_client(url, cb);
}

/**
 * @brief 销毁消息队列客户端实现
 * @details 通过MQManager单例销毁指定的消息队列客户端
 * @param id 要销毁的客户端ID
 */
void destroy_client(WtUInt32 id)
{
	getMgr().destroy_client(id);
}

/**
 * @brief 订阅主题实现
 * @details 通过MQManager单例为指定的客户端订阅特定的消息主题
 * @param id 客户端ID
 * @param topic 要订阅的主题
 */
void subscribe_topic(WtUInt32 id, const char* topic)
{
	return getMgr().sub_topic(id, topic);
}

/**
 * @brief 启动客户端实现
 * @details 通过MQManager单例启动指定的客户端，开始接收消息
 * @param id 要启动的客户端ID
 */
void start_client(WtUInt32 id)
{
	getMgr().start_client(id);
}
