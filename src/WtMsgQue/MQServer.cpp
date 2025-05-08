/*!
 * \file MQServer.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 消息队列服务端实现
 */
#include "MQServer.h"
#include "MQManager.h"

#include "../Share/StrUtil.hpp"

#include <spdlog/fmt/fmt.h>
#include <atomic>


#ifndef NN_STATIC_LIB
#define NN_STATIC_LIB
#endif
#include <nanomsg/nn.h>
#include <nanomsg/pubsub.h>


USING_NS_WTP;


/**
 * @brief 生成唯一的消息队列服务端ID
 * @details 使用原子操作生成递增的服务端ID，从1001开始
 * @return 新生成的服务端ID
 */
inline uint32_t makeMQSvrId()
{
	static std::atomic<uint32_t> _auto_server_id{ 1001 };
	return _auto_server_id.fetch_add(1);
}


/**
 * @brief 构造函数实现
 * @details 初始化服务端对象，设置初始状态并生成唯一ID
 * @param mgr 消息队列管理器指针
 */
MQServer::MQServer(MQManager* mgr)
	: _sock(-1)           // 初始化socket为无效值
	, _ready(false)       // 初始化服务端为未就绪状态
	, _mgr(mgr)           // 设置管理器指针
	, _confirm(false)     // 默认不使用确认模式
	, m_bTerminated(false) // 初始化终止标志为false
	, m_bTimeout(false)    // 初始化超时标志为false
{
	_id = makeMQSvrId(); // 生成唯一的服务端ID
}

/**
 * @brief 析构函数实现
 * @details 清理服务端资源，停止发布线程
 *          注意：当前实现中并未关闭socket，这可能是一个遗漏
 */
MQServer::~MQServer()
{
	if (!_ready) // 如果服务端未就绪，直接返回
		return;

	m_bTerminated = true; // 设置终止标志
	m_condCast.notify_all(); // 通知发布线程退出
	if (m_thrdCast)
		m_thrdCast->join(); // 等待发布线程结束

	//if (_sock >= 0)
	//	nn_close(_sock); // 注释掉的关闭socket代码，可能存在资源泄漏
}

/**
 * @brief 初始化服务端实现
 * @details 创建并初始化nanomsg套接字，绑定到指定的URL地址
 *          设置发送缓冲区大小并将服务端设置为就绪状态
 * @param url 绑定的URL地址，格式如"tcp://0.0.0.0:5555"
 * @param confirm 是否需要确认模式
 * @return 初始化是否成功
 */
bool MQServer::init(const char* url, bool confirm /* = false */)
{
	// 如果已经初始化过，直接返回成功
	if (_sock >= 0)
		return true;

	_confirm = confirm;

	// 创建nanomsg PUB类型的套接字
	_sock = nn_socket(AF_SP, NN_PUB);
	if(_sock < 0)
	{
		_mgr->log_server(_id, fmt::format("MQServer {} has an error {} while initializing", _id, _sock).c_str());
		return false;
	}

	// 设置发送缓冲区大小为8MB
	int bufsize = 8 * 1024 * 1024;
	nn_setsockopt(_sock, NN_SOL_SOCKET, NN_SNDBUF, &bufsize, sizeof(bufsize));

	_url = url;
	// 绑定套接字到指定的URL
	if(nn_bind(_sock, url) < 0)
	{
		_mgr->log_server(_id, fmt::format("MQServer {} has an error while binding url {}", _id, url).c_str());
		return false;
	}
	else
	{
		_mgr->log_server(_id, fmt::format("MQServer {} has binded to {} ", _id, url).c_str());
	}

	// 设置服务端为就绪状态
	_ready = true;

	_mgr->log_server(_id, fmt::format("MQServer {} ready", _id).c_str());
	return true;
}

/**
 * @brief 发布消息实现
 * @details 将消息添加到发布队列中，并启动或通知发布线程
 *          发布线程会处理队列中的消息并发送给客户端
 * @param topic 消息主题
 * @param data 消息数据指针
 * @param dataLen 消息数据长度
 */
void MQServer::publish(const char* topic, const void* data, uint32_t dataLen)
{
	// 检查服务端是否已初始化
	if(_sock < 0)
	{
		_mgr->log_server(_id, fmt::format("MQServer {} has not been initialized yet", _id).c_str());
		return;
	}

	// 检查数据是否有效及线程是否已终止
	if(data == NULL || dataLen == 0 || m_bTerminated)
		return;

	{
		// 加锁并将消息添加到队列
		StdUniqueLock lock(m_mtxCast);
		m_dataQue.push(PubData(topic, data, dataLen));
		m_bTimeout = false; // 重置超时标志
	}

	// 如果发布线程不存在，创建新的发布线程
	if(m_thrdCast == NULL)
	{
		m_thrdCast.reset(new StdThread([this](){

			// 初始化发送缓冲区
			if (m_sendBuf.empty())
				m_sendBuf.resize(1024 * 1024, 0); // 初始化为1MB
			
			// 主循环，直到终止标志被设置
			while (!m_bTerminated)
			{
				// 获取当前连接的客户端数量
				int cnt = (int)nn_get_statistic(_sock, NN_STAT_CURRENT_CONNECTIONS);
				
				// 如果队列为空或者在确认模式下没有客户端连接，等待
				if(m_dataQue.empty() || (cnt == 0 && _confirm))
				{
					StdUniqueLock lock(m_mtxCast);
					m_bTimeout = true;
					// 等待60秒或者直到被通知
					m_condCast.wait_for(lock, std::chrono::seconds(60));
					//如果有新的数据进来，timeout会被改为false
					//如果没有新的数据进来，timeout会保持为true
					if (m_bTimeout)
					{
						//等待超时以后，广播心跳包
						m_dataQue.push(PubData("HEARTBEAT", "", 0));
					}
					else
					{
						continue; // 被通知有新消息，继续循环
					}
				}	

				// 创建临时队列并交换数据，减少锁的持有时间
				PubDataQue tmpQue;
				{
					StdUniqueLock lock(m_mtxCast);
					tmpQue.swap(m_dataQue);
				}
				
				// 处理队列中的每个消息
				while(!tmpQue.empty())
				{
					const PubData& pubData = tmpQue.front();

					// 只发送非空消息（心跳包可能是空消息）
					if (!pubData._data.empty())
					{
						// 计算消息包总长度
						std::size_t len = sizeof(MQPacket) + pubData._data.size();
						
						// 如果缓冲区不足，扩大缓冲区
						if (m_sendBuf.size() < len)
							m_sendBuf.resize(m_sendBuf.size() * 2);
						
						// 构建消息包
						MQPacket* pack = (MQPacket*)m_sendBuf.data();
						strncpy(pack->_topic, pubData._topic.c_str(), 32);
						pack->_length = (uint32_t)pubData._data.size();
						memcpy(&pack->_data, pubData._data.data(), pubData._data.size());
						
						// 发送消息，确保完整发送
						int bytes_snd = 0;
						for(;;)
						{
							int bytes = nn_send(_sock, m_sendBuf.data() + bytes_snd, len - bytes_snd, 0);
							if (bytes >= 0)
							{
								bytes_snd += bytes;
								if(bytes_snd == len) // 全部发送完成
									break;
							}
							else
								// 发送失败，等待一小段时间后重试
								std::this_thread::sleep_for(std::chrono::milliseconds(1));
						}
						
					}
					tmpQue.pop(); // 移除已处理的消息
				} 
			}
		}));
	}
	else
	{
		// 如果发布线程已存在，通知它处理新消息
		m_condCast.notify_all();
	}
}