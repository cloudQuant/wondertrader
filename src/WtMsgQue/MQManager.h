/*!
 * \file MQManager.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 消息队列管理器定义
 */
#pragma once
#include "PorterDefs.h"
#include "MQServer.h"
#include "MQClient.h"

#include "../Includes/FasterDefs.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN

#pragma warning(disable:4200)

#pragma pack(push,1)
/**
 * @brief 消息队列数据包结构
 * @details 定义了消息队列传输的数据包格式，包含主题、数据长度和可变长数据
 *          采用紧凑打包方式，确保内存布局的紧凑性
 */
typedef struct _MQPacket
{
	char			_topic[32];    ///< 消息主题，最长32字节
	uint32_t		_length;     ///< 数据长度
	char			_data[0];     ///< 可变长数据区域
} MQPacket;
#pragma pack(pop)

typedef std::shared_ptr<MQServer> MQServerPtr;
typedef std::shared_ptr<MQClient> MQClientPtr;

/**
 * @brief 消息队列管理器类
 * @details 管理消息队列的服务端和客户端实例
 *          提供创建、销毁、发布消息、订阅主题等功能
 *          充当服务端和客户端的统一管理入口
 */
class MQManager
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化消息队列管理器，将日志回调函数设置为空
	 */
	MQManager() : _cb_log(NULL) {}

public:
	/**
	 * @brief 注册日志回调函数
	 * @details 设置用于处理日志的回调函数，用于记录服务端和客户端的日志信息
	 * @param cbLog 日志回调函数，接收ID、消息和是否为服务端的标志
	 */
	inline void		regiter_callbacks(FuncLogCallback cbLog) { _cb_log = cbLog; }

	/**
	 * @brief 创建消息队列服务端
	 * @details 创建并初始化一个新的消息队列服务端实例
	 * @param url 服务端绑定的地址，格式如"tcp://0.0.0.0:5555"
	 * @param confirm 是否需要消息确认
	 * @return 新创建的服务端ID
	 */
	WtUInt32	create_server(const char* url, bool confirm);

	/**
	 * @brief 销毁消息队列服务端
	 * @details 销毁指定的消息队列服务端实例，释放相关资源
	 * @param id 要销毁的服务端ID
	 */
	void		destroy_server(WtUInt32 id);

	/**
	 * @brief 发布消息
	 * @details 通过指定的服务端发布消息到指定的主题
	 * @param id 服务端ID
	 * @param topic 消息主题
	 * @param data 消息数据指针
	 * @param dataLen 消息数据长度
	 */
	void		publish_message(WtUInt32 id, const char* topic, const void* data, WtUInt32 dataLen);

	/**
	 * @brief 创建消息队列客户端
	 * @details 创建并初始化一个新的消息队列客户端实例
	 * @param url 要连接的服务器地址，格式如"tcp://127.0.0.1:5555"
	 * @param cb 消息接收回调函数，用于处理收到的消息
	 * @return 新创建的客户端ID
	 */
	WtUInt32	create_client(const char* url, FuncMQCallback cb);

	/**
	 * @brief 销毁消息队列客户端
	 * @details 销毁指定的消息队列客户端实例，释放相关资源
	 * @param id 要销毁的客户端ID
	 */
	void		destroy_client(WtUInt32 id);

	/**
	 * @brief 订阅主题
	 * @details 为指定的客户端订阅特定的消息主题
	 * @param id 客户端ID
	 * @param topic 要订阅的主题
	 */
	void		sub_topic(WtUInt32 id, const char* topic);

	/**
	 * @brief 启动客户端
	 * @details 启动指定的客户端，开始接收消息
	 * @param id 要启动的客户端ID
	 */
	void		start_client(WtUInt32 id);

	/**
	 * @brief 记录服务端日志
	 * @details 调用日志回调函数记录服务端相关的日志信息
	 * @param id 服务端ID
	 * @param message 日志消息内容
	 */
	void		log_server(WtUInt32 id, const char* message);

	/**
	 * @brief 记录客户端日志
	 * @details 调用日志回调函数记录客户端相关的日志信息
	 * @param id 客户端ID
	 * @param message 日志消息内容
	 */
	void		log_client(WtUInt32 id, const char* message);

private:
	/// @brief 服务端映射类型定义
	/// @details 定义了以服务端ID为键，服务端指针为值的映射类型
	typedef wt_hashmap<uint32_t, MQServerPtr> ServerMap;

	/// @brief 服务端实例映射
	/// @details 存储所有已创建的服务端实例，以ID为键
	ServerMap	_servers;

	/// @brief 客户端映射类型定义
	/// @details 定义了以客户端ID为键，客户端指针为值的映射类型
	typedef wt_hashmap<uint32_t, MQClientPtr> ClientMap;

	/// @brief 客户端实例映射
	/// @details 存储所有已创建的客户端实例，以ID为键
	ClientMap	_clients;

	/// @brief 日志回调函数
	/// @details 用于记录服务端和客户端的日志信息
	FuncLogCallback	_cb_log;
};

NS_WTP_END
