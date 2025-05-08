/*!
 * \file MQClient.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 消息队列客户端定义
 */
#pragma once
#include "PorterDefs.h"
#include <queue>

#include "../Includes/WTSMarcos.h"
#include "../Includes/FasterDefs.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class MQManager;

/**
 * @brief 消息队列客户端类
 * @details 基于nanomsg实现的消息队列客户端，用于订阅和接收消息
 *          支持按主题订阅和过滤消息，并通过回调函数处理收到的消息
 *          内部实现了连接状态检测和消息分包处理
 */
class MQClient
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建消息队列客户端实例，并分配唯一的客户端 ID
	 * @param mgr 消息队列管理器指针，用于日志记录和客户端管理
	 */
	MQClient(MQManager* mgr);

	/**
	 * @brief 析构函数
	 * @details 清理客户端资源，关闭网络连接并停止接收线程
	 */
	~MQClient();

private:
	/**
	 * @brief 从接收缓冲区提取完整消息包
	 * @details 处理接收缓冲区中的数据，将其分解为多个消息包
	 *          对每个消息包进行主题过滤，并通过回调函数处理符合条件的消息
	 */
	void	extract_buffer();

	/**
	 * @brief 检查指定主题是否允许接收
	 * @details 如果没有设置订阅主题（_topics为空），则接收所有主题
	 *          否则只接收已订阅的主题
	 * @param topic 要检查的消息主题
	 * @return 如果允许接收该主题返回true，否则返回false
	 */
	inline bool	is_allowed(const char* topic)
	{
		if (_topics.empty())
			return true;

		auto it = _topics.find(topic);
		if (it != _topics.end())
			return true;

		return false;
	}

public:
	/**
	 * @brief 获取客户端唯一ID
	 * @details 返回创建客户端时分配的唯一标识符
	 * @return 客户端唯一ID
	 */
	inline uint32_t id() const { return _id; }

	/**
	 * @brief 初始化客户端
	 * @details 创建并配置消息队列套接字，连接到指定的服务地址
	 *          设置消息接收回调函数和缓冲区大小
	 * @param url 服务器地址，格式如"tcp://127.0.0.1:5555"
	 * @param cb 消息接收回调函数，用于处理收到的消息
	 * @return 初始化成功返回true，失败返回false
	 */
	bool	init(const char* url, FuncMQCallback cb);

	/**
	 * @brief 启动客户端
	 * @details 创建并启动接收线程，开始接收消息
	 *          如果客户端已经启动或未初始化，则不会重复启动
	 *          接收线程会持续监听消息并处理连接超时检测
	 */
	void	start();

	/**
	 * @brief 订阅指定主题
	 * @details 将指定的主题添加到订阅列表中
	 *          当没有订阅任何主题时，客户端会接收所有主题的消息
	 *          订阅主题后，只会接收指定主题的消息
	 * @param topic 要订阅的主题名称
	 */
	inline void	sub_topic(const char* topic)
	{
		_topics.insert(topic);
	}

private:
	std::string		m_strURL;
	bool			m_bReady;
	int				_sock;
	MQManager*		_mgr;
	uint32_t		_id;

	StdThreadPtr	m_thrdRecv;
	bool			m_bTerminated;
	int64_t			m_iCheckTime;
	bool			m_bNeedCheck;

	std::string		_buffer;
	FuncMQCallback	_cb_message;

	wt_hashset<std::string> _topics;
	char			_recv_buf[1024 * 1024];
};

NS_WTP_END