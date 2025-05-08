/*!
 * \file MQClient.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 消息队列客户端实现
 */
#include "MQClient.h"
#include "MQManager.h"

#include "../Share/fmtlib.h"
#include "../Share/TimeUtils.hpp"
#include <atomic>

#ifndef NN_STATIC_LIB
#define NN_STATIC_LIB
#endif
#include <nanomsg/nn.h>
#include <nanomsg/pubsub.h>


USING_NS_WTP;

#pragma warning(disable:4200)

#define  RECV_BUF_SIZE  1024*1024

/**
 * @brief 生成唯一的消息队列客户端 ID
 * @details 使用原子操作生成递增的客户端 ID，从5001开始
 * @return 新生成的客户端 ID
 */
inline uint32_t makeMQCientId()
{
	static std::atomic<uint32_t> _auto_client_id{ 5001 };
	return _auto_client_id.fetch_add(1);
}


/**
 * @brief 构造函数实现
 * @details 初始化客户端对象的所有成员变量，并分配唯一的客户端 ID
 *          初始状态下套接字无效，客户端未就绪，未终止，无回调函数
 * @param mgr 消息队列管理器指针，用于日志记录和客户端管理
 */
MQClient::MQClient(MQManager* mgr)
	: _sock(-1)
	, m_bReady(false)
	, _mgr(mgr)
	, m_bTerminated(false)
	, _cb_message(NULL)
	, m_iCheckTime(0)
	, m_bNeedCheck(false)
{
	_id = makeMQCientId();
}

/**
 * @brief 析构函数实现
 * @details 清理客户端资源，包括停止接收线程和关闭网络连接
 *          如果客户端未就绪，则直接返回不进行清理
 *          否则设置终止标志，等待接收线程结束，并关闭套接字
 */
MQClient::~MQClient()
{
	if (!m_bReady)
		return;

	m_bTerminated = true;
	if (m_thrdRecv)
		m_thrdRecv->join();

	if (_sock != 0)
		nn_close(_sock);
}

/**
 * @brief 初始化客户端实现
 * @details 创建并配置消息队列套接字，连接到指定的服务地址
 *          如果客户端已经初始化，则直接返回true
 *          初始化过程包括：设置回调函数、创建套接字、设置订阅选项、设置缓冲区大小、连接服务器
 * @param url 服务器地址，格式如"tcp://127.0.0.1:5555"
 * @param cb 消息接收回调函数，用于处理收到的消息
 * @return 初始化成功返回true，失败返回false
 */
bool MQClient::init(const char* url, FuncMQCallback cb)
{
	if (_sock >= 0)
		return true;

	_cb_message = cb;
	_sock = nn_socket(AF_SP, NN_SUB);
	if (_sock < 0)
	{
		_mgr->log_client(_id, fmtutil::format("MQClient {} has an error {} while initializing", _id, _sock));
		return false;
	}

	nn_setsockopt(_sock, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);

	int bufsize = RECV_BUF_SIZE;
	nn_setsockopt(_sock, NN_SOL_SOCKET, NN_RCVBUF, &bufsize, sizeof(bufsize));

	m_strURL = url;
	if (nn_connect(_sock, url) < 0)
	{
		_mgr->log_client(_id, fmtutil::format("MQClient {} has an error while connecting url {}", _id, url));
		return false;
	}
	else
	{
		_mgr->log_client(_id, fmtutil::format("MQClient {} has connected to {} ", _id, url));
	}

	m_bReady = true;

	_mgr->log_client(_id, fmtutil::format("MQClient {} inited", _id));
	return true;
}

/**
 * @brief 启动客户端实现
 * @details 创建并启动接收线程，开始接收消息
 *          如果客户端已经终止或未初始化，则不会启动
 *          接收线程会持续监听消息，并在收到数据时处理
 *          同时实现了连接超时检测机制，当超过60秒未收到数据时发送超时通知
 */
void MQClient::start()
{
	if (m_bTerminated)
		return;

	if(_sock < 0)
	{
		_mgr->log_client(_id, fmtutil::format("MQClient {} has not been initialized yet", _id));
		return;
	}

	if (m_thrdRecv == NULL)
	{
		m_thrdRecv.reset(new StdThread([this]() {

			while (!m_bTerminated)
			{
				bool hasData = false;
				for(;;)
				{
					int nBytes = nn_recv(_sock, _recv_buf, RECV_BUF_SIZE, NN_DONTWAIT);
					if (nBytes > 0)
					{
						m_iCheckTime = TimeUtils::getLocalTimeNow();
						m_bNeedCheck = true;
						hasData = true;
						_buffer.append(_recv_buf, nBytes);
					}
					else
					{
						break;
					}
				}

				if (hasData)
					extract_buffer();
				else
				{
					if(m_iCheckTime != 0 && m_bNeedCheck)
					{
						int64_t now = TimeUtils::getLocalTimeNow();
						int64_t elapse = now - m_iCheckTime;
						if (elapse >= 60 * 1000)
						{
							//只通知一次，防止重复通知
							_cb_message(_id, "TIMEOUT", "", 0);
							m_bNeedCheck = false;
						}
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				
			}
		}));

		_mgr->log_client(_id, fmtutil::format("MQClient {} has started successfully", _id));
	}
	else
	{
		_mgr->log_client(_id, fmtutil::format("MQClient {} has already started", _id));
	}
	
}

/**
 * @brief 从接收缓冲区提取完整消息包实现
 * @details 处理接收缓冲区中的数据，将其分解为多个消息包
 *          对每个消息包进行长度检查和主题过滤
 *          如果消息主题符合订阅条件，则通过回调函数处理消息
 *          处理完成后，从缓冲区中移除已处理的数据
 */
void MQClient::extract_buffer()
{
	uint32_t proc_len = 0;
	for(;;)
	{
		//先做长度检查
		if (_buffer.length() - proc_len < sizeof(MQPacket))
			break;

		MQPacket* packet = (MQPacket*)(_buffer.data() + proc_len);

		if (_buffer.length() - proc_len < sizeof(MQPacket) + packet->_length)
			break;

		char* data = packet->_data;

		if (is_allowed(packet->_topic))
			_cb_message(_id, packet->_topic, packet->_data, packet->_length);

		proc_len += sizeof(MQPacket) + packet->_length;
	}

	if(proc_len > 0)
		_buffer.erase(0, proc_len);
}