/*!
 * \file WtOrdMon.h
 * \brief 订单监控器头文件
 *
 * 本文件定义了订单监控器类，用于管理和跟踪订单状态
 */
#pragma once
#include <unordered_map>
#include <stdint.h>
#include <functional>

#include "../Share/StdUtils.hpp"

/**
 * @brief 订单回调函数类型
 * @details 用于遍历订单时的回调函数类型，仅提供本地订单ID
 * @param uint32_t 本地订单ID
 */
typedef std::function<void(uint32_t)> EnumOrderCallback;

/**
 * @brief 订单详细信息回调函数类型
 * @details 用于遍历订单时的回调函数类型，提供本地订单ID、下单时间和是否可撤销信息
 * @param uint32_t 本地订单ID
 * @param uint64_t 下单时间
 * @param bool 是否可撤销
 */
typedef std::function<void(uint32_t, uint64_t, bool)> EnumAllOrderCallback;

/**
 * @brief 订单监控器类
 * @details 负责管理和跟踪交易订单的状态，包括添加、移除、查询和超时检查等功能
 */
class WtOrdMon
{
public:
		/**
	 * @brief 添加订单到监控器
	 * @details 将一组订单ID添加到监控器中，记录其下单时间和是否可撤销
	 * @param ids 本地订单ID数组指针
	 * @param cnt 订单数量
	 * @param curTime 当前时间，作为订单下单时间
	 * @param bCanCancel 是否可撤销，true为可撤销（默认），false为不可撤销
	 */
	void push_order(const uint32_t* ids, uint32_t cnt, uint64_t curTime, bool bCanCancel = true);

		/**
	 * @brief 从监控器中移除指定订单
	 * @details 当订单完成成交或撤销时，从监控器中移除该订单
	 * @param localid 本地订单ID
	 */
	void erase_order(uint32_t localid);

		/**
	 * @brief 检查是否有订单
	 * @details 检查监控器中是否有指定订单或任意订单
	 * @param localid 本地订单ID，默认为0。当为0时检查是否有任意订单，不为0时检查是否有指定订单
	 * @return 如果有订单返回true，否则返回false
	 */
	inline bool has_order(uint32_t localid = 0)
	{
		if (localid == 0)
			return !_orders.empty();

		auto it = _orders.find(localid);
		if (it == _orders.end())
			return false;

		return true;
	}

		/**
	 * @brief 检查超时订单
	 * @details 检查监控器中的订单，如果超过指定时间则触发回调函数进行处理
	 * @param expiresecs 订单超时时间（秒）
	 * @param curTime 当前时间
	 * @param callback 处理超时订单的回调函数
	 */
	void check_orders(uint32_t expiresecs, uint64_t curTime, EnumOrderCallback callback);

		/**
	 * @brief 清空所有订单
	 * @details 将监控器中的所有订单清空
	 */
	inline void clear_orders()
	{
		_orders.clear();
	}

		/**
	 * @brief 枚举所有订单
	 * @details 遍历监控器中的所有订单，并通过回调函数返回订单信息
	 * @param cb 处理订单信息的回调函数
	 */
	void enumOrder(EnumAllOrderCallback cb);

private:
	/** @brief 订单信息对，包含下单时间和是否可撤销 */
	typedef std::pair<uint64_t, bool> OrderPair;	//uint64_t - entertime, bool - cancancel
	/** @brief 订单ID映射表，本地订单ID到订单信息的映射 */
	typedef std::unordered_map<uint32_t, OrderPair> IDMap;
	/** @brief 订单集合，存储所有被监控的订单 */
	IDMap			_orders;
	/** @brief 互斥锁，用于保护订单集合的线程安全 */
	StdRecurMutex	_mtx_ords;
};

