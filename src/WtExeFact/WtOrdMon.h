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

/** @brief 超时扫描候选；generation 用于拒绝已删除或被复用 localid 的旧快照 */
struct WtOrdMonTimeoutCandidate
{
	uint32_t localid;
	uint64_t generation;
};

typedef std::function<void(uint32_t, uint64_t)> EnumOrderGenerationCallback;

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
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		if (localid == 0)
			return !_orders.empty();

		auto it = _orders.find(localid);
		if (it == _orders.end())
			return false;

		return true;
	}

		/**
	 * @brief 检查超时订单
	 * @details 超时判断基于加锁时的订单快照，回调在解锁后执行；并发移除订单后，快照中的ID仍可能被回调。
	 * @param expiresecs 订单超时时间（秒）
	 * @param curTime 当前时间
	 * @param callback 处理超时订单的回调函数
	 */
	void check_orders(uint32_t expiresecs, uint64_t curTime, EnumOrderCallback callback);

	/**
	 * @brief 按可控时间生成带代次的超时快照。
	 * @details 回调仍在锁外执行；调用方必须用 mark_unverified_timeout 重新校验，
	 *          不得直接把候选当作仍存活的订单。
	 */
	void check_orders_with_generation(uint32_t expiresecs, uint64_t curTime,
		EnumOrderGenerationCallback callback);

	/**
	 * @brief 仅当候选仍对应同一条可撤订单时，将其转为不可验证超时状态。
	 * @return 只有本次成功完成状态转移时返回 true。
	 */
	bool mark_unverified_timeout(uint32_t localid, uint64_t generation,
		uint32_t expiresecs, uint64_t curTime);

	/** @brief 查询执行单元生命周期内是否发生过无法验证身份的超时；不随 clear_orders 清除。 */
	inline bool has_unverified_timeout()
	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		return _has_unverified_timeout;
	}

		/**
	 * @brief 清空所有订单
	 * @details 将监控器中的所有订单清空
	 */
	inline void clear_orders()
	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		_orders.clear();
	}

		/**
	 * @brief 枚举所有订单
	 * @details 回调按加锁时复制的快照执行，回调期间可以安全地增删订单。
	 * @param cb 处理订单信息的回调函数
	 */
	void enumOrder(EnumAllOrderCallback cb);

private:
	/** @brief 订单信息包含创建时间、撤单能力和本地代次 */
	struct OrderInfo
	{
		uint64_t enterTime{ 0 };
		bool canCancel{ true };
		uint64_t generation{ 0 };
		bool unverifiedTimeout{ false };
	};
	/** @brief 订单ID映射表，本地订单ID到订单信息的映射 */
	typedef std::unordered_map<uint32_t, OrderInfo> IDMap;
	/** @brief 订单集合，存储所有被监控的订单 */
	IDMap			_orders;
	uint64_t		_next_generation{ 1 };
	bool			_has_unverified_timeout{ false };
	/** @brief 互斥锁，用于保护订单集合的线程安全 */
	StdRecurMutex	_mtx_ords;
};

