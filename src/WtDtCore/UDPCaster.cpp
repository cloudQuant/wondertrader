/*!
 * \file UDPCaster.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "UDPCaster.h"
#include "DataManager.h"

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"

#include <cstring>
#include <exception>
#include <limits>


#define UDP_MSG_SUBSCRIBE	0x100
#define UDP_MSG_PUSHTICK	0x200
#define UDP_MSG_PUSHORDQUE	0x201	//委托队列
#define UDP_MSG_PUSHORDDTL	0x202	//委托明细
#define UDP_MSG_PUSHTRANS	0x203	//逐笔成交

namespace {

constexpr int kMaxUdpPort = static_cast<int>(
	std::numeric_limits<boost::asio::ip::port_type>::max());

bool isValidUdpPort(int port)
{
	return port >= 0 && port <= kMaxUdpPort;
}

} // namespace

#pragma pack(push,1)
//UDP请求包
typedef struct _UDPReqPacket
{
	uint32_t		_type;
	char			_data[1020];
} UDPReqPacket;

//UDPTick数据包
template <typename T>
struct UDPDataPacket
{
	uint32_t	_type;
	T			_data;
};
#pragma pack(pop)
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;
typedef UDPDataPacket<WTSOrdQueStruct>	UDPOrdQuePacket;
typedef UDPDataPacket<WTSOrdDtlStruct>	UDPOrdDtlPacket;
typedef UDPDataPacket<WTSTransStruct>	UDPTransPacket;

UDPCaster::UDPCaster()
	: m_bStarted(false)
	, m_bStopping(false)
	, m_bStopped(false)
	, m_bTerminated(false)
	, m_bdMgr(NULL)
	, m_dtMgr(NULL)
{
	
}


UDPCaster::~UDPCaster()
{
	stop();
}

bool UDPCaster::init(WTSVariant* cfg, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)
{
	m_bdMgr = bdMgr;
	m_dtMgr = dtMgr;

	if (!cfg->getBoolean("active"))
		return false;

	WTSVariant* cfgBC = cfg->get("broadcast");
	if (cfgBC)
	{
		for (uint32_t idx = 0; idx < cfgBC->size(); idx++)
		{
			WTSVariant* cfgItem = cfgBC->get(idx);
			addBRecver(cfgItem->getCString("host"), cfgItem->getInt32("port"), cfgItem->getUInt32("type"));
		}
	}

	WTSVariant* cfgMC = cfg->get("multicast");
	if (cfgMC)
	{
		for (uint32_t idx = 0; idx < cfgMC->size(); idx++)
		{
			WTSVariant* cfgItem = cfgMC->get(idx);
			addMRecver(cfgItem->getCString("host"), cfgItem->getInt32("port"), cfgItem->getInt32("sendport"), cfgItem->getUInt32("type"));
		}
	}

	//By Wesley @ 2022.01.11
	//这是订阅端口，但是以前全部用的bport，属于笔误
	//只能写一个兼容了
	int32_t sport = cfg->getInt32("sport");
	if (sport == 0)
		sport = cfg->getInt32("bport");
	return start(sport);
}

bool UDPCaster::start(int sport)
{
	StdUniqueLock lock(m_mtxLifecycle);
	if (m_bTerminated)
		return false;
	if (m_bStarted)
		return true;

	UDPSocketPtr broadcastSocket;
	UDPSocketPtr subscribeSocket;
	auto closeGroups = [](MulticastList& groups)
	{
		for (MulticastPair& item : groups)
		{
			if (item.first)
			{
				boost::system::error_code ec;
				item.first->close(ec);
			}
		}
		groups.clear();
	};

	try
	{
		if (!m_listFlatRecver.empty() || !m_listJsonRecver.empty() || !m_listRawRecver.empty())
		{
			broadcastSocket.reset(new UDPSocket(m_ioservice,
				boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0)));
			boost::asio::socket_base::broadcast option(true);
			broadcastSocket->set_option(option);
		}

		subscribeSocket.reset(new UDPSocket(m_ioservice,
			boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sport)));
	}
	catch (const std::exception& e)
	{
		WTSLogger::error("Failed to create UDP caster socket while starting @ port {}: {}", sport, e.what());
		closeGroups(m_listFlatGroup);
		closeGroups(m_listJsonGroup);
		closeGroups(m_listRawGroup);
		return false;
	}
	catch (...)
	{
		WTSLogger::error("Unknown exception while creating UDP caster socket @ port {}", sport);
		closeGroups(m_listFlatGroup);
		closeGroups(m_listJsonGroup);
		closeGroups(m_listRawGroup);
		return false;
	}

	m_sktBroadcast = broadcastSocket;
	m_sktSubscribe = subscribeSocket;
	do_receive();

	m_thrdIO.reset(new StdThread([this](){
		{
			StdUniqueLock lock(m_mtxLifecycle);
			m_ioThreadId = std::this_thread::get_id();
		}
		try
		{
			m_ioservice.run();
		}
		catch(...)
		{
			m_ioservice.stop();
		}
	}));
	m_bStarted = true;
	return true;
}

void UDPCaster::stop()
{
	StdThreadPtr ioThread;
	StdThreadPtr castThread;
	{
		StdUniqueLock lock(m_mtxLifecycle);
		const std::thread::id currentThread = std::this_thread::get_id();
		const bool isWorkerThread =
			(m_thrdIO && m_ioThreadId == currentThread)
			|| (m_thrdCast && m_castThreadId == currentThread);
		if (isWorkerThread)
		{
			// A worker can request shutdown, but it cannot join itself or destroy its owner.
			m_bTerminated = true;
			m_ioservice.stop();
			lock.unlock();
			m_condCast.notify_all();
			return;
		}

		if (m_bStopped)
			return;
		if (m_bStopping)
		{
			m_condLifecycle.wait(lock, [this]() { return !m_bStopping; });
			return;
		}

		m_bStopping = true;
		m_bTerminated = true;
		m_ioservice.stop();
		ioThread = m_thrdIO;
		castThread = m_thrdCast;
	}

	m_condCast.notify_all();
	if (ioThread && ioThread->joinable())
		ioThread->join();
	if (castThread && castThread->joinable())
		castThread->join();

	{
		StdUniqueLock lock(m_mtxLifecycle);
		{
			StdUniqueLock castLock(m_mtxCast);
			while (!m_dataQue.empty())
				m_dataQue.pop();
		}

		auto closeSocket = [](UDPSocketPtr& socket)
		{
			if (socket)
			{
				boost::system::error_code ec;
				socket->close(ec);
				socket.reset();
			}
		};
		auto closeGroups = [&closeSocket](MulticastList& groups)
		{
			for (MulticastPair& item : groups)
				closeSocket(item.first);
			groups.clear();
		};

		closeGroups(m_listFlatGroup);
		closeGroups(m_listJsonGroup);
		closeGroups(m_listRawGroup);
		closeSocket(m_sktBroadcast);
		closeSocket(m_sktSubscribe);

		m_thrdIO.reset();
		m_thrdCast.reset();
		m_ioThreadId = std::thread::id();
		m_castThreadId = std::thread::id();
		m_bStopped = true;
		m_bStopping = false;
	}
	m_condLifecycle.notify_all();
}

void UDPCaster::do_receive()
{
	if (m_bTerminated || !m_sktSubscribe || !m_sktSubscribe->is_open())
		return;

	m_sktSubscribe->async_receive_from(boost::asio::buffer(m_data, max_length), m_senderEP,
		[this](boost::system::error_code ec, std::size_t bytes_recvd)
	{
		if(ec)
		{
			if (!m_bTerminated)
				do_receive();
			return;
		}

		if (bytes_recvd == sizeof(UDPReqPacket))
		{
			UDPReqPacket* req = (UDPReqPacket*)m_data;

			std::string data;
			//处理请求
			if (req->_type == UDP_MSG_SUBSCRIBE)
			{
				const char* terminator = static_cast<const char*>(
					std::memchr(req->_data, '\0', sizeof(req->_data)));
				if (terminator == NULL)
				{
					do_receive();
					return;
				}

				const std::string subscribeData(req->_data,
					static_cast<std::size_t>(terminator - req->_data));
				const StringVector& ay = StrUtil::split(subscribeData, ",");
				std::string code, exchg;
				for(const std::string& fullcode : ay)
				{
					auto pos = fullcode.find(".");
					if (pos == std::string::npos)
						code = fullcode;
					else
					{
						code = fullcode.substr(pos + 1);
						exchg = fullcode.substr(0, pos);
					}
					WTSContractInfo* ct = m_bdMgr->getContract(code.c_str(), exchg.c_str());
					if (ct == NULL)
						continue;

					WTSTickData* curTick = m_dtMgr->getCurTick(code.c_str(), exchg.c_str());
					if(curTick == NULL)
						continue;

					std::shared_ptr<std::string> data(new std::string());
					data->resize(sizeof(UDPTickPacket), 0);
					UDPTickPacket* pkt = (UDPTickPacket*)data->data();
					pkt->_type = req->_type;
					memcpy(&pkt->_data, &curTick->getTickStruct(), sizeof(WTSTickStruct));
					curTick->release();
					m_sktSubscribe->async_send_to(
						boost::asio::buffer(*data, data->size()), m_senderEP,
						[data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/)
					{
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
			std::shared_ptr<std::string> data(new std::string("Can not indentify the command"));
			m_sktSubscribe->async_send_to(
				boost::asio::buffer(*data, data->size()), m_senderEP,
				[data](const boost::system::error_code& ec, std::size_t /*bytes_sent*/)
			{
				if (ec)
				{
					WTSLogger::error("Sending data on UDP failed: {}", ec.message().c_str());
				}
			});
		}

		do_receive();
	});
}

bool UDPCaster::addBRecver(const char* remote, int port, int type /* = 0 */)
{
	if (!isValidUdpPort(port))
	{
		WTSLogger::error("Invalid UDP receiver port: {}", port);
		return false;
	}

	if (type < 0 || type > 2)
	{
		WTSLogger::error("Unsupported UDP receiver type: {}", type);
		return false;
	}

	try
	{
		boost::asio::ip::address_v4 addr = boost::asio::ip::make_address_v4(remote);
		UDPReceiverPtr item(new UDPReceiver(EndPoint(addr, port), type));
		if(type == 0)
			m_listFlatRecver.emplace_back(item);
		else if(type == 1)
			m_listJsonRecver.emplace_back(item);
		else if(type == 2)
			m_listRawRecver.emplace_back(item);
	}
	catch(...)
	{
		return false;
	}

	return true;
}


bool UDPCaster::addMRecver(const char* remote, int port, int sendport, int type /* = 0 */)
{
	if (!isValidUdpPort(port) || !isValidUdpPort(sendport))
	{
		WTSLogger::error("Invalid UDP multicast receiver ports: port={}, sendport={}", port, sendport);
		return false;
	}

	if (type < 0 || type > 2)
	{
		WTSLogger::error("Unsupported UDP receiver type: {}", type);
		return false;
	}

	try
	{
		boost::asio::ip::address_v4 addr = boost::asio::ip::make_address_v4(remote);
		UDPReceiverPtr item(new UDPReceiver(EndPoint(addr, port), type));
		UDPSocketPtr sock(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sendport)));
		boost::asio::ip::multicast::join_group option(item->_ep.address());
		sock->set_option(option);
		if(type == 0)
			m_listFlatGroup.emplace_back(std::make_pair(sock, item));
		else if(type == 1)
			m_listJsonGroup.emplace_back(std::make_pair(sock, item));
		else if(type == 2)
			m_listRawGroup.emplace_back(std::make_pair(sock, item));
	}
	catch(...)
	{
		return false;
	}

	return true;
}

void UDPCaster::broadcast(WTSTickData* curTick)
{
	do_broadcast(curTick, UDP_MSG_PUSHTICK);
}

void UDPCaster::broadcast(WTSOrdDtlData* curOrdDtl)
{
	do_broadcast(curOrdDtl, UDP_MSG_PUSHORDDTL);
}

void UDPCaster::broadcast(WTSOrdQueData* curOrdQue)
{
	do_broadcast(curOrdQue, UDP_MSG_PUSHORDQUE);
}

void UDPCaster::broadcast(WTSTransData* curTrans)
{
	do_broadcast(curTrans, UDP_MSG_PUSHTRANS);
}

void UDPCaster::do_broadcast(WTSObject* data, uint32_t dataType)
{
	if (data == NULL)
		return;

	StdUniqueLock lifecycleLock(m_mtxLifecycle);
	if (m_sktBroadcast == NULL || m_bTerminated || m_bStopping || m_bStopped)
		return;

	{
		StdUniqueLock lock(m_mtxCast);
		m_dataQue.push(CastData(data, dataType));
	}

	if(m_thrdCast == NULL)
	{
		m_thrdCast.reset(new StdThread([this](){
			{
				StdUniqueLock lock(m_mtxLifecycle);
				m_castThreadId = std::this_thread::get_id();
			}

			while (true)
			{
				std::queue<CastData> tmpQue;
				{
					StdUniqueLock lock(m_mtxCast);
					m_condCast.wait(lock, [this]() {
						return m_bTerminated || !m_dataQue.empty();
					});
					if (m_bTerminated)
						return;
					tmpQue.swap(m_dataQue);
				}
				
				while(!tmpQue.empty())
				{
					const CastData& castData = tmpQue.front();

					if (castData._data == NULL)
						break;

					//直接广播
					if (!m_listRawGroup.empty() || !m_listRawRecver.empty())
					{
						std::string buf_raw;
						if (castData._datatype == UDP_MSG_PUSHTICK)
						{
							buf_raw.resize(sizeof(UDPTickPacket));
							UDPTickPacket* pack = (UDPTickPacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSTickData* curObj = (WTSTickData*)castData._data;
							memcpy(&pack->_data, &curObj->getTickStruct(), sizeof(WTSTickStruct));
						}
						else if (castData._datatype == UDP_MSG_PUSHORDDTL)
						{
							buf_raw.resize(sizeof(UDPOrdDtlPacket));
							UDPOrdDtlPacket* pack = (UDPOrdDtlPacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSOrdDtlData* curObj = (WTSOrdDtlData*)castData._data;
							memcpy(&pack->_data, &curObj->getOrdDtlStruct(), sizeof(WTSOrdDtlStruct));
						}
						else if (castData._datatype == UDP_MSG_PUSHORDQUE)
						{
							buf_raw.resize(sizeof(UDPOrdQuePacket));
							UDPOrdQuePacket* pack = (UDPOrdQuePacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSOrdQueData* curObj = (WTSOrdQueData*)castData._data;
							memcpy(&pack->_data, &curObj->getOrdQueStruct(), sizeof(WTSOrdQueStruct));
						}
						else if (castData._datatype == UDP_MSG_PUSHTRANS)
						{
							buf_raw.resize(sizeof(UDPTransPacket));
							UDPTransPacket* pack = (UDPTransPacket*)buf_raw.data();
							pack->_type = castData._datatype;
							WTSTransData* curObj = (WTSTransData*)castData._data;
							memcpy(&pack->_data, &curObj->getTransStruct(), sizeof(WTSTransStruct));
						}
						else
						{
							break;
						}

						//广播
						boost::system::error_code ec;
						for (auto it = m_listRawRecver.begin(); it != m_listRawRecver.end(); it++)
						{
							const UDPReceiverPtr& receiver = (*it);
							m_sktBroadcast->send_to(boost::asio::buffer(buf_raw), receiver->_ep, 0, ec);
							if (ec)
							{
								WTSLogger::error("Error occured while sending to ({}:{}): {}({})", 
									receiver->_ep.address().to_string(), receiver->_ep.port(), ec.value(), ec.message());
							}
						}

						//组播
						for (auto it = m_listRawGroup.begin(); it != m_listRawGroup.end(); it++)
						{
							const MulticastPair& item = *it;
							it->first->send_to(boost::asio::buffer(buf_raw), item.second->_ep, 0, ec);
							if (ec)
							{
								WTSLogger::error("Error occured while sending to ({}:{}): {}({})",
									item.second->_ep.address().to_string(), item.second->_ep.port(), ec.value(), ec.message());
							}
						}
					}

					tmpQue.pop();
				} 
			}
		}));
	}
	m_condCast.notify_all();
}

void UDPCaster::handle_send_broad(const EndPoint& ep, const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
{
	if(error)
	{
		WTSLogger::error("Broadcasting of market data failed, remote addr: {}, error message: {}", ep.address().to_string().c_str(), error.message().c_str());
	}
}

void UDPCaster::handle_send_multi(const EndPoint& ep, const boost::system::error_code& error, std::size_t /*bytes_transferred*/)
{
	if(error)
	{
		WTSLogger::error("Multicasting of market data failed, remote addr: {}, error message: {}", ep.address().to_string().c_str(), error.message().c_str());
	}
}

