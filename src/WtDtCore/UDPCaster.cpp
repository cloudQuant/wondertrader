/*!
 * \file UDPCaster.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UDP广播器实现
 * \details 实现了UDP广播器类，用于将行情数据通过UDP协议广播至多个接收端
 *          支持普通广播和组播两种模式，可处理适价行情、委托队列、委托明细、成交数据等类型
 */
#include "UDPCaster.h"
#include "DataManager.h"

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"


/**
 * @brief UDP消息类型定义
 * @details 定义了不同类型的UDP消息码，用于标识推送的不同数据类型
 */
#define UDP_MSG_SUBSCRIBE	0x100  ///< 订阅消息类型，用于客户端订阅特定数据
#define UDP_MSG_PUSHTICK	0x200  ///< 推送适价行情数据类型
#define UDP_MSG_PUSHORDQUE	0x201	///< 推送委托队列数据类型
#define UDP_MSG_PUSHORDDTL	0x202	///< 推送委托明细数据类型
#define UDP_MSG_PUSHTRANS	0x203	///< 推送逐笔成交数据类型

#pragma pack(push,1)
/**
 * @brief UDP请求包结构体
 * @details 定义UDP请求数据的格式，包含请求类型和请求数据
 */
typedef struct _UDPReqPacket
{
	uint32_t		_type;      ///< 请求类型，如UDP_MSG_SUBSCRIBE
	char			_data[1020]; ///< 请求数据内容，如需要订阅的代码列表
} UDPReqPacket;

/**
 * @brief UDP数据包模板结构体
 * @details 模板化的UDP数据包结构，可以存储不同类型的数据
 * @tparam T 具体数据类型，如WTSTickStruct
 */
template <typename T>
struct UDPDataPacket
{
	uint32_t	_type;  ///< 数据类型，如UDP_MSG_PUSHTICK
	T			_data;   ///< 实际的数据内容
};
#pragma pack(pop)

/**
 * @brief 适价数据包类型别名
 */
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;

/**
 * @brief 委托队列数据包类型别名
 */
typedef UDPDataPacket<WTSOrdQueStruct>	UDPOrdQuePacket;

/**
 * @brief 委托明细数据包类型别名
 */
typedef UDPDataPacket<WTSOrdDtlStruct>	UDPOrdDtlPacket;

/**
 * @brief 成交数据包类型别名
 */
typedef UDPDataPacket<WTSTransStruct>	UDPTransPacket;

/**
 * @brief 构造函数
 * @details 初始化UDP广播器对象，设置初始状态和变量
 */
UDPCaster::UDPCaster()
	: m_bTerminated(false)  ///< 初始化终止标志为关闭状态
	, m_bdMgr(NULL)         ///< 初始化基础数据管理器指针为空
	, m_dtMgr(NULL)         ///< 初始化数据管理器指针为空
{
	
}


/**
 * @brief 析构函数
 * @details 清理UDP广播器资源，实际清理工作在stop方法中完成
 */
UDPCaster::~UDPCaster()
{
}

/**
 * @brief 初始化UDP广播器
 * @details 根据配置初始化UDP广播器，设置广播和组播的目标主机和端口
 * @param cfg 配置对象，包含广播器的全部配置信息
 * @param bdMgr 基础数据管理器，用于获取合约信息
 * @param dtMgr 数据管理器，用于获取行情数据
 * @return 初始化是否成功
 */
bool UDPCaster::init(WTSVariant* cfg, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)
{
	// 保存基础数据管理器和数据管理器指针
	m_bdMgr = bdMgr;
	m_dtMgr = dtMgr;

	// 如果广播器未激活，直接返回失败
	if (!cfg->getBoolean("active"))
		return false;

	// 处理普通广播配置
	WTSVariant* cfgBC = cfg->get("broadcast");
	if (cfgBC)
	{
		for (uint32_t idx = 0; idx < cfgBC->size(); idx++)
		{
			WTSVariant* cfgItem = cfgBC->get(idx);
			// 添加广播接收者，包含主机、端口和数据类型
			addBRecver(cfgItem->getCString("host"), cfgItem->getInt32("port"), cfgItem->getUInt32("type"));
		}
	}

	// 处理组播配置
	WTSVariant* cfgMC = cfg->get("multicast");
	if (cfgMC)
	{
		for (uint32_t idx = 0; idx < cfgMC->size(); idx++)
		{
			WTSVariant* cfgItem = cfgMC->get(idx);
			// 添加组播接收者，包含主机、端口、发送端口和数据类型
			addMRecver(cfgItem->getCString("host"), cfgItem->getInt32("port"), cfgItem->getInt32("sendport"), cfgItem->getUInt32("type"));
		}
	}

	//By Wesley @ 2022.01.11
	//这是订阅端口，但是以前全部用的bport，属于笔误
	//只能写一个兼容了
	// 获取订阅端口，兼容旧配置中的bport字段
	int32_t sport = cfg->getInt32("sport");
	if (sport == 0)
		sport = cfg->getInt32("bport");
	// 启动广播器
	start(sport);

	return true;
}

/**
 * @brief 启动UDP广播器
 * @details 初始化广播和订阅套接字，并启动IO线程
 * @param sport 订阅端口，用于接收外部订阅请求
 */
void UDPCaster::start(int sport)
{
	// 如果有任何类型的接收者，初始化广播套接字
	if (!m_listFlatRecver.empty() || !m_listJsonRecver.empty() || !m_listRawRecver.empty())
	{
		// 创建UDP广播套接字，使用随机端口
		m_sktBroadcast.reset(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0)));
		// 允许广播模式
		boost::asio::socket_base::broadcast option(true);
		m_sktBroadcast->set_option(option);
	}

	try
	{
		// 创建UDP订阅套接字，绑定到指定端口
		m_sktSubscribe.reset(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sport)));
	}
	catch(...)
	{
		// 捕获并记录创建订阅套接字时的异常
		WTSLogger::error("Exception raised while start subscribing service @ port {}", sport);
	}

	// 开始异步接收数据
	do_receive();

	// 启动IO服务线程
	m_thrdIO.reset(new StdThread([this](){
		try
		{
			// 运行Boost.Asio事件循环
			m_ioservice.run();
		}
		catch(...)
		{
			// 出现异常时停止IO服务
			m_ioservice.stop();
		}
	}));
}

/**
 * @brief 停止UDP广播器
 * @details 停止IO服务和广播线程，清理资源
 */
void UDPCaster::stop()
{
	// 设置终止标志，让广播循环退出
	m_bTerminated = true;
	// 停止IO服务
	m_ioservice.stop();
	// 等待IO线程结束
	if (m_thrdIO)
		m_thrdIO->join();

	// 唯醒所有等待广播的线程，让它们检测到终止标志
	m_condCast.notify_all();
	// 等待广播线程结束
	if (m_thrdCast)
		m_thrdCast->join();
}

/**
 * @brief 执行异步数据接收
 * @details 针对订阅套接字设置异步接收处理器，处理外部发来的订阅和请求
 */
void UDPCaster::do_receive()
{
	// 开始异步接收数据，并指定接收完成后的处理函数
	m_sktSubscribe->async_receive_from(boost::asio::buffer(m_data, max_length), m_senderEP,
		[this](boost::system::error_code ec, std::size_t bytes_recvd)
	{
		// 如果发生错误，重新开始接收
		if(ec)
		{
			do_receive();
			return;
		}

		// 检查接收的数据是否为完整的请求包
		if (bytes_recvd == sizeof(UDPReqPacket))
		{
			// 将接收的数据转换为请求包格式
			UDPReqPacket* req = (UDPReqPacket*)m_data;

			std::string data;
			// 处理订阅类型的请求
			if (req->_type == UDP_MSG_SUBSCRIBE)
			{
				// 使用逗号分割多个代码
				const StringVector& ay = StrUtil::split(req->_data, ",");
				std::string code, exchg;
				// 逐个处理代码
				for(const std::string& fullcode : ay)
				{
					// 处理带有交易所前缀的完整代码格式 (exchg.code)
					auto pos = fullcode.find(".");
					if (pos == std::string::npos)
						code = fullcode;  // 如果没有点，则全部作为代码
					else
					{
						// 如果有点，则分离交易所和代码
						code = fullcode.substr(pos + 1);
						exchg = fullcode.substr(0, pos);
					}
					// 根据代码和交易所获取合约信息
					WTSContractInfo* ct = m_bdMgr->getContract(code.c_str(), exchg.c_str());
					if (ct == NULL)
						continue;  // 无效合约，跳过

					// 获取当前诸如数据
					WTSTickData* curTick = m_dtMgr->getCurTick(code.c_str(), exchg.c_str());
					if(curTick == NULL)
						continue;  // 无效诸如数据，跳过

					// 创建并填充回复数据包
					std::string* data = new std::string();
					data->resize(sizeof(UDPTickPacket), 0);
					UDPTickPacket* pkt = (UDPTickPacket*)data->data();
					pkt->_type = req->_type;
					// 将诸如数据结构体复制到数据包中
					memcpy(&pkt->_data, &curTick->getTickStruct(), sizeof(WTSTickStruct));
					// 释放诸如数据对象
					curTick->release();
					// 异步发送回复数据
					m_sktSubscribe->async_send_to(
						boost::asio::buffer(*data, data->size()), m_senderEP,
						[this, data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/)
					{
						// 发送完成后释放数据
						delete data;
						// 如果发送发生错误则记录
						if (ec)
						{
							WTSLogger::error("Sending data on UDP failed: {}", ec.message().c_str());
						}
					});
				}
			}			
		}
		else
		{
			// 如果数据不是预期的标准请求包格式，发送错误信息
			std::string* data = new std::string("Can not indentify the command");
			// 异步发送错误消息
			m_sktSubscribe->async_send_to(
				boost::asio::buffer(*data, data->size()), m_senderEP,
				[this, data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/)
			{
				// 发送完成后释放数据
				delete data;
				// 如果发送发生错误则记录
				if (ec)
				{
					WTSLogger::error("Sending data on UDP failed: {}", ec.message().c_str());
				}
			});
		}

		// 继续接收下一个数据包
		do_receive();
	});
}

/**
 * @brief 添加普通广播接收者
 * @details 添加一个接收普通广播数据的端点，根据数据类型添加到相应的接收者列表中
 * @param remote 远程主机地址或域名
 * @param port 远程端口
 * @param type 数据类型（0: 预设格式，1: JSON格式，2: 原始格式）
 * @return 是否添加成功
 */
bool UDPCaster::addBRecver(const char* remote, int port, int type /* = 0 */)
{
	try
	{
		// 从字符串解析IP地址
		boost::asio::ip::address_v4 addr = boost::asio::ip::address_v4::from_string(remote);
		// 创建UDP接收者对象
		UDPReceiverPtr item(new UDPReceiver(EndPoint(addr, port), type));
		// 根据数据类型添加到不同的列表中
		if(type == 0)
			m_listFlatRecver.emplace_back(item);  // 预设格式接收者
		else if(type == 1)
			m_listJsonRecver.emplace_back(item);  // JSON格式接收者
		else if(type == 2)
			m_listRawRecver.emplace_back(item);   // 原始格式接收者
	}
	catch(...)
	{
		// 处理所有异常，比如IP地址解析错误
		return false;
	}

	return true;
}


/**
 * @brief 添加组播接收者
 * @details 添加一个接收组播数据的端点，同时创建一个组播套接字并加入组
 * @param remote 远程主机地址或组播地址
 * @param port 组播端口
 * @param sendport 发送组播数据的本地端口
 * @param type 数据类型（0: 预设格式，1: JSON格式，2: 原始格式）
 * @return 是否添加成功
 */
bool UDPCaster::addMRecver(const char* remote, int port, int sendport, int type /* = 0 */)
{
	try
	{
		// 从字符串解析IP地址
		boost::asio::ip::address_v4 addr = boost::asio::ip::address_v4::from_string(remote);
		// 创建UDP接收者对象
		UDPReceiverPtr item(new UDPReceiver(EndPoint(addr, port), type));
		// 创建组播套接字，绑定到指定的发送端口
		UDPSocketPtr sock(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sendport)));
		// 将套接字加入指定的组播组
		boost::asio::ip::multicast::join_group option(item->_ep.address());
		sock->set_option(option);
		// 根据数据类型添加到不同的组播组列表中
		if(type == 0)
			m_listFlatGroup.emplace_back(std::make_pair(sock, item));  // 预设格式组播组
		else if(type == 1)
			m_listJsonGroup.emplace_back(std::make_pair(sock, item));  // JSON格式组播组
		else if(type == 2)
			m_listRawGroup.emplace_back(std::make_pair(sock, item));   // 原始格式组播组
	}
	catch(...)
	{
		// 处理所有异常，比如IP地址解析错误或端口绑定错误
		return false;
	}

	return true;
}

/**
 * @brief 广播适价行情数据
 * @details 将适价行情数据通过UDP广播给所有已注册的接收者
 * @param curTick 当前适价数据对象
 */
void UDPCaster::broadcast(WTSTickData* curTick)
{
	do_broadcast(curTick, UDP_MSG_PUSHTICK);
}

/**
 * @brief 广播委托明细数据
 * @details 将委托明细数据通过UDP广播给所有已注册的接收者
 * @param curOrdDtl 当前委托明细数据对象
 */
void UDPCaster::broadcast(WTSOrdDtlData* curOrdDtl)
{
	do_broadcast(curOrdDtl, UDP_MSG_PUSHORDDTL);
}

/**
 * @brief 广播委托队列数据
 * @details 将委托队列数据通过UDP广播给所有已注册的接收者
 * @param curOrdQue 当前委托队列数据对象
 */
void UDPCaster::broadcast(WTSOrdQueData* curOrdQue)
{
	do_broadcast(curOrdQue, UDP_MSG_PUSHORDQUE);
}

/**
 * @brief 广播成交数据
 * @details 将成交数据通过UDP广播给所有已注册的接收者
 * @param curTrans 当前成交数据对象
 */
void UDPCaster::broadcast(WTSTransData* curTrans)
{
	do_broadcast(curTrans, UDP_MSG_PUSHTRANS);
}

/**
 * @brief 执行数据广播操作
 * @details 将指定数据对象推送到广播队列，启动或唤醒广播线程进行处理
 * @param data 要广播的数据对象
 * @param dataType 数据类型，如UDP_MSG_PUSHTICK
 */
void UDPCaster::do_broadcast(WTSObject* data, uint32_t dataType)
{
	// 初始条件检查，确保广播套接字和数据对象存在，且终止标志未设置
	if(m_sktBroadcast == NULL || data == NULL || m_bTerminated)
		return;

	{
		// 加锁后将数据压入队列
		StdUniqueLock lock(m_mtxCast);
		m_dataQue.push(CastData(data, dataType));
	}

	// 如果广播线程尚未创建，则创建新线程
	if(m_thrdCast == NULL)
	{
		m_thrdCast.reset(new StdThread([this](){

			// 广播线程主循环
			while (!m_bTerminated)
			{
				// 如果队列为空，则等待条件变量通知
				if(m_dataQue.empty())
				{
					StdUniqueLock lock(m_mtxCast);
					m_condCast.wait(lock);
					continue;
				}	

				// 创建临时队列并交换数据，减少锁的持有时间
				std::queue<CastData> tmpQue;
				{
					StdUniqueLock lock(m_mtxCast);
					tmpQue.swap(m_dataQue);
				}
				
				// 处理临时队列中的所有数据
				while(!tmpQue.empty())
				{
					// 获取队列首部数据
					const CastData& castData = tmpQue.front();

					// 检查数据有效性
					if (castData._data == NULL)
						break;

					// 如果有原始格式的接收者或组播组，则处理原始格式广播
					if (!m_listRawGroup.empty() || !m_listRawRecver.empty())
					{
						// 初始化数据缓冲区
						std::string buf_raw;
						// 根据数据类型选择不同的处理方式
						if (castData._datatype == UDP_MSG_PUSHTICK)
						{
							// 准备适价数据包
							buf_raw.resize(sizeof(UDPTickPacket));
							UDPTickPacket* pack = (UDPTickPacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSTickData* curObj = (WTSTickData*)castData._data;
							memcpy(&pack->_data, &curObj->getTickStruct(), sizeof(WTSTickStruct));
						}
						else if (castData._datatype == UDP_MSG_PUSHORDDTL)
						{
							// 准备委托明细数据包
							buf_raw.resize(sizeof(UDPOrdDtlPacket));
							UDPOrdDtlPacket* pack = (UDPOrdDtlPacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSOrdDtlData* curObj = (WTSOrdDtlData*)castData._data;
							memcpy(&pack->_data, &curObj->getOrdDtlStruct(), sizeof(WTSOrdDtlStruct));
						}
						else if (castData._datatype == UDP_MSG_PUSHORDQUE)
						{
							// 准备委托队列数据包
							buf_raw.resize(sizeof(UDPOrdQuePacket));
							UDPOrdQuePacket* pack = (UDPOrdQuePacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSOrdQueData* curObj = (WTSOrdQueData*)castData._data;
							memcpy(&pack->_data, &curObj->getOrdQueStruct(), sizeof(WTSOrdQueStruct));
						}
						else if (castData._datatype == UDP_MSG_PUSHTRANS)
						{
							// 准备成交数据包
							buf_raw.resize(sizeof(UDPTransPacket));
							UDPTransPacket* pack = (UDPTransPacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSTransData* curObj = (WTSTransData*)castData._data;
							memcpy(&pack->_data, &curObj->getTransStruct(), sizeof(WTSTransStruct));
						}
						else
						{
							// 不支持的数据类型，跳过处理
							break;
						}

						// 向普通广播接收者发送数据
						boost::system::error_code ec;
						for (auto it = m_listRawRecver.begin(); it != m_listRawRecver.end(); it++)
						{
							const UDPReceiverPtr& receiver = (*it);
							m_sktBroadcast->send_to(boost::asio::buffer(buf_raw), receiver->_ep, 0, ec);
							// 出错时记录错误日志
							if (ec)
							{
								WTSLogger::error("Error occured while sending to ({}:{}): {}({})", 
									receiver->_ep.address().to_string(), receiver->_ep.port(), ec.value(), ec.message());
							}
						}

						// 向组播组发送数据
						for (auto it = m_listRawGroup.begin(); it != m_listRawGroup.end(); it++)
						{
							const MulticastPair& item = *it;
							it->first->send_to(boost::asio::buffer(buf_raw), item.second->_ep, 0, ec);
							// 出错时记录错误日志
							if (ec)
							{
								WTSLogger::error("Error occured while sending to ({}:{}): {}({})",
									item.second->_ep.address().to_string(), item.second->_ep.port(), ec.value(), ec.message());
							}
						}
					}

					// 移除已处理的数据
					tmpQue.pop();
				} 
			}
		}));
	}
	else
	{
		// 如果广播线程已存在，则唯醒它处理新数据
		m_condCast.notify_all();
	}
}

/**
 * @brief 处理普通广播发送完成后的回调
 * @details 异步发送普通广播数据完成后的回调函数，主要用于错误处理
 * @param ep 目标端点，包含IP地址和端口
 * @param error 发送过程中的错误码
 * @param bytes_transferred 实际发送的字节数
 */
void UDPCaster::handle_send_broad(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred)
{
	// 如果发送过程中出现错误，记录错误日志
	if(error)
	{
		WTSLogger::error("Broadcasting of market data failed, remote addr: {}, error message: {}", ep.address().to_string().c_str(), error.message().c_str());
	}
}

/**
 * @brief 处理组播发送完成后的回调
 * @details 异步发送组播数据完成后的回调函数，主要用于错误处理
 * @param ep 目标组播端点，包含组播地址和端口
 * @param error 发送过程中的错误码
 * @param bytes_transferred 实际发送的字节数
 */
void UDPCaster::handle_send_multi(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred)
{
	// 如果发送过程中出现错误，记录错误日志
	if(error)
	{
		WTSLogger::error("Multicasting of market data failed, remote addr: {}, error message: {}", ep.address().to_string().c_str(), error.message().c_str());
	}
}

