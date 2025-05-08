/*!
 * \file StatHelper.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据统计辅助类
 * \details 提供数据统计功能，用于跟踪和统计广播数据的接收包数、发送包数和发送字节数
 *          采用单例模式实现，支持多类型统计和线程安全的数据更新和获取
 */
#pragma once
#include <boost/atomic.hpp>
#include <boost/interprocess/detail/atomic.hpp>

/**
 * @brief 数据统计辅助类
 * @details 实现数据的统计功能，包括接收包数、发送包数和发送字节数的统计
 *          采用单例模式设计，确保全局唯一的统计实例
 *          支持多种类型的统计数据，并使用读写锁保证线程安全
 */
class StatHelper
{
public:
	/**
	 * @brief 获取StatHelper的单例对象
	 * @details 采用单例模式，确保全局唯一的统计实例
	 *          通过这个方法获取类实例，而不是直接创建对象
	 * @return StatHelper& 返回单例对象的引用
	 */
	static StatHelper& one()
	{
		static StatHelper only; // 内部静态对象，确保对象的全局唯一性
		return only;
	}

public:
	/**
	 * @brief 统计信息结构体
	 * @details 存储广播数据的接收包数、发送包数和发送字节数等统计信息
	 */
	typedef struct _StatInfo
	{
		uint32_t	_recv_packs;  ///< 接收数据包数量
		uint32_t	_send_packs;  ///< 发送数据包数量
		uint64_t	_send_bytes;  ///< 发送数据字节数

		/**
		 * @brief 结构体构造函数
		 * @details 初始化统计信息，将所有计数器设置为0
		 */
		_StatInfo()
		{
			_recv_packs = 0;  // 初始化接收包数为0
			_send_bytes = 0;  // 初始化发送字节数为0
			_send_packs = 0;  // 初始化发送包数为0
		}
	} StatInfo;

	/**
	 * @brief 统计类型枚举
	 * @details 定义不同的统计类型，当前仅支持广播数据的统计
	 */
	typedef enum
	{
		ST_BROADCAST ///< 广播数据的统计类型
	} StatType;

	/**
	 * @brief 更新标志枚举
	 * @details 用于标记哪些统计数据被更新，可以通过按位或操作组合多个标志
	 */
	typedef enum 
	{
		UF_Recv		= 0x0001, ///< 接收数据更新标志，值为1
		UF_Send		= 0x0002  ///< 发送数据更新标志，值为2
	} UpdateFlag;

public:
	/**
	 * @brief 更新统计信息
	 * @details 线程安全地更新指定类型的统计信息，包括接收包数、发送包数和发送字节数
	 *          使用写锁保证线程安全，并处理可能的数据溢出情况
	 * @param sType 统计类型，指定要更新的统计数据类型
	 * @param recvPacks 新接收的数据包数量
	 * @param sendPacks 新发送的数据包数量
	 * @param sendBytes 新发送的数据字节数
	 */
	void updateStatInfo(StatType sType, uint32_t recvPacks, uint32_t sendPacks, uint64_t sendBytes)
	{
		BoostWriteLock lock(_mutexes[sType]); // 获取写锁，确保线程安全
		StatInfo& sInfo = _stats[sType]; // 获取指定类型的统计信息
		sInfo._recv_packs += recvPacks; // 累加接收包数
		sInfo._send_packs += sendPacks; // 累加发送包数
		// 处理字节数累加可能导致的溢出
		if(UINT64_MAX - sInfo._send_bytes < sendBytes) // 检查是否会溢出
			sInfo._send_bytes = sendBytes; // 如果会溢出，直接设置为新值
		else
			sInfo._send_bytes += sendBytes; // 如果不会溢出，正常累加
		
		// 设置更新标志
		uint32_t flag = 0;
		if (recvPacks > 0)
			flag |= UF_Recv; // 标记接收数据被更新
		if (sendPacks > 0)
			flag |= UF_Send; // 标记发送数据被更新
	}
	
	/**
	 * @brief 获取统计信息
	 * @details 线程安全地获取指定类型的统计信息
	 *          使用读锁保证线程安全，允许多个线程同时读取
	 * @param sType 统计类型，指定要获取的统计数据类型
	 * @return StatInfo 返回指定类型的统计信息副本
	 */
	StatInfo getStatInfo(StatType sType)
	{
		BoostReadLock lock(_mutexes[sType]); // 获取读锁，确保线程安全
		return _stats[sType]; // 返回统计信息的副本
	}

private:
	StatInfo		_stats[5];     ///< 统计信息数组，每个下标对应一种统计类型，最多支持5种类型
	BoostRWMutex	_mutexes[5];   ///< 读写锁数组，每种统计类型对应一个读写锁，用于线程同步
};

