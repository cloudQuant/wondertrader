/*!
 * \file MQServer.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 消息队列服务端定义
 */
#pragma once

#include <queue>

#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class MQManager;

/**
 * @brief 消息队列服务端类
 * @details 基于nanomsg实现的消息队列服务端，负责消息的发布
 *          采用发布-订阅模式，支持多客户端连接和主题过滤
 */
class MQServer
{
public:
	/**
	 * @brief 构造函数
	 * @param mgr 消息队列管理器指针，用于日志记录和管理
	 */
	MQServer(MQManager* mgr);

	/**
	 * @brief 析构函数
	 * @details 负责清理资源，包括停止发布线程和关闭socket
	 */
	~MQServer();

public:
	/**
	 * @brief 获取服务端ID
	 * @return 服务端唯一标识符
	 */
	inline uint32_t id() const { return _id; }

	/**
	 * @brief 初始化服务端
	 * @param url 绑定的URL地址，格式如"tcp://0.0.0.0:5555"
	 * @param confirm 是否需要确认模式，如果为true，则只有客户端连接时才发送消息
	 * @return 初始化是否成功
	 */
	bool	init(const char* url, bool confirm = false);

	/**
	 * @brief 发布消息
	 * @param topic 消息主题
	 * @param data 消息数据指针
	 * @param dataLen 消息数据长度
	 */
	void	publish(const char* topic, const void* data, uint32_t dataLen);

private:
	std::string		_url;      ///< 服务端绑定的URL地址
	bool			_ready;    ///< 服务端是否已就绪
	int				_sock;     ///< nanomsg套接字句柄
	MQManager*		_mgr;      ///< 消息队列管理器指针
	uint32_t		_id;       ///< 服务端唯一标识符
	bool			_confirm;  ///< 是否需要确认模式

	/// @brief 发布线程指针
	/// @details 负责从队列中取出消息并发送
	StdThreadPtr	m_thrdCast;

	/// @brief 条件变量，用于线程同步
	/// @details 在有新消息入队时通知发布线程
	StdCondVariable	m_condCast;

	/// @brief 互斥锁，用于保护数据队列
	StdUniqueMutex	m_mtxCast;

	/// @brief 线程终止标志
	/// @details 当需要终止发布线程时设置为true
	bool			m_bTerminated;

	/// @brief 等待超时标志
	/// @details 用于判断是否需要发送心跳包
	bool			m_bTimeout;

	/**
	 * @brief 发布数据结构体
	 * @details 存储要发布的消息主题和数据
	 */
	typedef struct _PubData
	{
		std::string	_topic;  ///< 消息主题
		std::string	_data;   ///< 消息数据

		/**
		 * @brief 构造函数
		 * @param topic 消息主题
		 * @param data 消息数据指针
		 * @param dataLen 消息数据长度
		 */
		_PubData(const char* topic, const void* data, uint32_t dataLen)
			: _topic(topic)
		{
			if(data !=  NULL && dataLen != 0)
			{
				_data.append((const char*)data, dataLen);
			}
		}
	} PubData;
	/// @brief 发布数据队列类型定义
	typedef std::queue<PubData> PubDataQue;

	/// @brief 发布数据队列
	/// @details 存储待发送的消息
	PubDataQue		m_dataQue;

	/// @brief 发送缓冲区
	/// @details 用于构建发送数据包，避免重复分配内存
	std::string		m_sendBuf;
};

NS_WTP_END