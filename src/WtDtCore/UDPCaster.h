/*!
 * \file UDPCaster.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UDP广播对象定义
 * \details 该文件定义了基于UDP协议的数据广播器，用于向外部接收者广播行情数据
 *          支持普通广播和组播两种模式，可以将行情数据以不同格式(Flat/JSON/Raw)发送给接收方
 *          实现了IDataCaster接口，提供了统一的数据广播功能
 */
#pragma once
#include "IDataCaster.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/StdUtils.hpp"

#include <boost/asio.hpp>
#include <queue>

NS_WTP_BEGIN
	class WTSVariant;
NS_WTP_END

USING_NS_WTP;

class WTSBaseDataMgr;
class DataManager;

/**
 * @brief UDP数据广播器类
 * @details 用于将交易系统的行情数据通过UDP协议广播到外部接收者
 *          类实现了IDataCaster接口，支持普通广播和组播两种模式
 *          可将不同类型的广播数据（预设格式、JSON格式、原始格式）发送给不同类型的接收者
 */
class UDPCaster : public IDataCaster
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化UDP广播器对象，初始化内部变量和状态
	 */
	UDPCaster();

	/**
	 * @brief 析构函数
	 * @details 清理UDP广播器资源，包括关闭线程和释放内部对象
	 */
	~UDPCaster();

	/**
	 * @brief UDP端点类型别名
	 * @details 封装boost::asio::ip::udp::endpoint作为端点类型，用于表示网络端点（IP地址和端口）
	 */
	typedef boost::asio::ip::udp::endpoint EndPoint;

	/**
	 * @brief UDP接收者结构体
	 * @details 定义UDP数据接收者的信息，包括端点和数据类型
	 */
	typedef struct tagUDPReceiver
	{
		EndPoint	_ep;    ///< 端点信息，包含IP地址和端口
		uint32_t	_type;  ///< 数据类型，用于标识不同格式的数据(0:Flat, 1:JSON, 2:Raw)

		/**
		 * @brief 构造函数
		 * @param ep 端点信息
		 * @param t 数据类型
		 */
		tagUDPReceiver(EndPoint ep, uint32_t t)
		{
			_ep = ep;
			_type = t;
		}

	} UDPReceiver;

	/**
	 * @brief UDP接收者的智能指针类型
	 */
	typedef std::shared_ptr<UDPReceiver>	UDPReceiverPtr;

	/**
	 * @brief UDP接收者列表类型
	 */
	typedef std::vector<UDPReceiverPtr>		ReceiverList;

private:
	/**
	 * @brief 处理普通广播发送完成后的回调
	 * @param ep 目标端点
	 * @param error 发送过程中的错误信息
	 * @param bytes_transferred 实际发送的字节数
	 */
	void	handle_send_broad(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred); 

	/**
	 * @brief 处理组播发送完成后的回调
	 * @param ep 目标端点
	 * @param error 发送过程中的错误信息
	 * @param bytes_transferred 实际发送的字节数
	 */
	void	handle_send_multi(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred); 

	/**
	 * @brief 执行数据接收操作
	 * @details 接收外部的UDP数据，用于双向通信场景或反馈处理
	 */
	void	do_receive();

	/**
	 * @brief 执行数据广播操作
	 * @details 将指定数据对象广播到已注册的接收者
	 * @param data 要广播的数据对象
	 * @param dataType 数据类型，用于确定数据处理方式
	 */
	void	do_broadcast(WTSObject* data, uint32_t dataType);

public:
	/**
	 * @brief 初始化UDP广播器
	 * @details 根据给定的配置初始化UDP广播器，设置必要的内部参数
	 * @param cfg 配置对象，包含广播器的配置信息
	 * @param bdMgr 基础数据管理器指针
	 * @param dtMgr 数据管理器指针
	 * @return bool 初始化是否成功
	 */
	bool	init(WTSVariant* cfg, WTSBaseDataMgr* bdMgr, DataManager* dtMgr);

	/**
	 * @brief 启动UDP广播器
	 * @details 启动广播线程和IO线程，准备数据广播
	 * @param bport 广播端口
	 */
	void	start(int bport);

	/**
	 * @brief 停止UDP广播器
	 * @details 停止广播线程和IO线程，清理资源
	 */
	void	stop();

	/**
	 * @brief 添加普通广播接收者
	 * @details 添加一个接收普通广播数据的端点
	 * @param remote 远程主机地址或域名
	 * @param port 远程端口
	 * @param type 数据类型（0: 预设格式, 1: JSON格式, 2: 原始格式）
	 * @return bool 添加是否成功
	 */
	bool	addBRecver(const char* remote, int port, int type = 0);

	/**
	 * @brief 添加组播接收者
	 * @details 添加一个接收组播数据的端点
	 * @param remote 远程主机地址或组播地址
	 * @param port 远程端口
	 * @param sendport 发送组播的本地端口
	 * @param type 数据类型（0: 预设格式, 1: JSON格式, 2: 原始格式）
	 * @return bool 添加是否成功
	 */
	bool	addMRecver(const char* remote, int port, int sendport, int type = 0);

public:
	/**
	 * @brief 广播适价行情数据
	 * @details 将适价行情数据通过UDP广播给所有已注册的接收者
	 * @param curTick 当前适价数据对象
	 */
	virtual void	broadcast(WTSTickData* curTick) override;

	/**
	 * @brief 广播委托队列数据
	 * @details 将委托队列数据通过UDP广播给所有已注册的接收者
	 * @param curOrdQue 当前委托队列数据对象
	 */
	virtual void	broadcast(WTSOrdQueData* curOrdQue) override;

	/**
	 * @brief 广播委托明细数据
	 * @details 将委托明细数据通过UDP广播给所有已注册的接收者
	 * @param curOrdDtl 当前委托明细数据对象
	 */
	virtual void	broadcast(WTSOrdDtlData* curOrdDtl) override;

	/**
	 * @brief 广播成交数据
	 * @details 将成交数据通过UDP广播给所有已注册的接收者
	 * @param curTrans 当前成交数据对象
	 */
	virtual void	broadcast(WTSTransData* curTrans) override;

private:
	/**
	 * @brief UDP套接字类型
	 * @details 封装boost::asio::ip::udp::socket作为UDP套接字类型
	 */
	typedef boost::asio::ip::udp::socket	UDPSocket;

	/**
	 * @brief UDP套接字的智能指针类型
	 */
	typedef std::shared_ptr<UDPSocket>		UDPSocketPtr;

	/**
	 * @brief UDP缓冲区最大长度枚举
	 */
	enum 
	{ 
		max_length = 2048 ///< 缓冲区最大长度，2048字节
	};

	boost::asio::ip::udp::endpoint	m_senderEP;   ///< 发送者端点，用于标识数据发送方
	char			m_data[max_length];             ///< 数据缓冲区，用于存储临时数据

	// 广播相关成员
	ReceiverList	m_listFlatRecver;    ///< 预设格式广播接收者列表
	ReceiverList	m_listJsonRecver;    ///< JSON格式广播接收者列表
	ReceiverList	m_listRawRecver;     ///< 原始格式广播接收者列表
	UDPSocketPtr	m_sktBroadcast;      ///< 广播用的UDP套接字
	UDPSocketPtr	m_sktSubscribe;      ///< 订阅用的UDP套接字，用于接收数据

	/**
	 * @brief 组播套接字和接收者配对类型
	 * @details 一个组播套接字和其相关的接收者信息组成的数据结构
	 */
	typedef std::pair<UDPSocketPtr,UDPReceiverPtr>	MulticastPair;

	/**
	 * @brief 组播对列表类型
	 */
	typedef std::vector<MulticastPair>	MulticastList;

	MulticastList	m_listFlatGroup;    ///< 预设格式组播组列表
	MulticastList	m_listJsonGroup;    ///< JSON格式组播组列表
	MulticastList	m_listRawGroup;     ///< 原始格式组播组列表
	boost::asio::io_service		m_ioservice;  ///< Boost.Asio IO服务对象，用于处理异步IO
	StdThreadPtr	m_thrdIO;            ///< IO处理线程，用于运行Boost.Asio服务

	StdThreadPtr	m_thrdCast;          ///< 广播处理线程，用于处理数据广播
	StdCondVariable	m_condCast;       ///< 广播线程条件变量，用于线程同步
	StdUniqueMutex	m_mtxCast;         ///< 广播线程互斥锁，用于线程同步
	bool			m_bTerminated;         ///< 终止标志，用于控制线程终止

	WTSBaseDataMgr*	m_bdMgr;         ///< 基础数据管理器指针
	DataManager*	m_dtMgr;             ///< 数据管理器指针

	/**
	 * @brief 广播数据结构体
	 * @details 存储广播的数据对象和类型信息，用于广播队列
	 *          封装了应用于引用计数的内存管理机制
	 */
	typedef struct _CastData
	{
		uint32_t	_datatype;  ///< 数据类型，标识不同类型的广播数据
		WTSObject*	_data;      ///< 数据对象指针，指向要广播的数据

		/**
		 * @brief 构造函数
		 * @param obj 数据对象指针
		 * @param dataType 数据类型
		 */
		_CastData(WTSObject* obj = NULL, uint32_t dataType = 0)
			: _data(obj), _datatype(dataType)
		{
			if (_data)
				_data->retain();  // 增加引用计数，防止数据被提前释放
		}

		/**
		 * @brief 拷贝构造函数
		 * @param data 源数据对象
		 */
		_CastData(const _CastData& data)
			: _data(data._data), _datatype(data._datatype)
		{
			if (_data)
				_data->retain();  // 增加引用计数，防止数据被提前释放
		}

		/**
		 * @brief 析构函数
		 * @details 释放数据对象的引用，减少引用计数
		 */
		~_CastData()
		{
			if (_data)
			{
				_data->release();  // 释放引用计数，当计数为0时对象会被销毁
				_data = NULL;
			}
		}
	} CastData;

	std::queue<CastData>		m_dataQue;  ///< 广播数据队列，存储等待广播的数据
};