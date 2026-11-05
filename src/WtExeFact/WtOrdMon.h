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
 * @brief WtOrdMon 生命周期内的版本化 opaque 订单身份。
 * @details 同一 WtOrdMon 实现模块中的实例使用不同命名空间，token 在该模块生命周期内不复用。
 *          该命名空间不持久化，也不跨模块统一；不能据此断言跨模块或跨进程唯一。
 *          调用方可以复制和保存身份，但不能构造有效身份。
 */
class WtOrdMonOrderIdentity
{
public:
	WtOrdMonOrderIdentity() = default; // 无效身份
	bool valid() const { return _version != 0 && _instance_id != 0 && _sequence != 0; }

private:
	friend class WtOrdMon;
	enum : uint32_t { kCurrentVersion = 1 };
	WtOrdMonOrderIdentity(uint32_t localid, uint32_t version, uint64_t instanceId, uint64_t sequence)
		: _localid(localid), _version(version), _instance_id(instanceId), _sequence(sequence) {}

	uint32_t _localid{ 0 };
	uint32_t _version{ 0 };
	uint64_t _instance_id{ 0 };
	uint64_t _sequence{ 0 };
};

typedef std::function<void(uint32_t, WtOrdMonOrderIdentity)> EnumOrderIdentityCallback;

/**
 * @brief 订单监控器类
 * @details 负责管理和跟踪交易订单的状态，包括添加、移除、查询和超时检查等功能
 */
class WtOrdMon
{
public:
	WtOrdMon();
	WtOrdMon(const WtOrdMon&) = delete;
	WtOrdMon& operator=(const WtOrdMon&) = delete;
	WtOrdMon(WtOrdMon&&) = delete;
	WtOrdMon& operator=(WtOrdMon&&) = delete;

		/**
	 * @brief 添加订单到监控器
	 * @details 将一组订单ID添加到监控器中，记录其下单时间和是否可撤销
	 * @param ids 本地订单ID数组指针
	 * @param cnt 订单数量
	 * @param curTime 当前时间，作为订单下单时间
	 * @param bCanCancel 是否可撤销，true为可撤销（默认），false为不可撤销
	 */
	void push_order(const uint32_t* ids, uint32_t cnt, uint64_t curTime, bool bCanCancel = true);

	/** @brief 原子登记一笔订单并返回其身份，避免登记后再按 localid 查询产生歧义。 */
	WtOrdMonOrderIdentity push_order_with_identity(uint32_t localid, uint64_t curTime,
		bool bCanCancel = true);

		/**
	 * @brief 从监控器中移除指定订单
	 * @details 当订单完成成交或撤销时，从监控器中移除该订单
	 * @param localid 本地订单ID
	 */
	void erase_order(uint32_t localid);

	/**
	 * @brief 保留订单关联记录，但将终态订单从超时撤单候选中移除。
	 * @return 仅当本次把一条存活订单首次转为终态时返回 true；订单不存在或已是终态
	 *         （重复的撤单/全成回报）时返回 false，调用方据此只消费一次终态回报。
	 */
	bool mark_order_terminal(uint32_t localid);

	/** @brief 仅当身份仍匹配当前订单时删除；供持有身份的迟到回调安全收敛。 */
	bool erase_order(const WtOrdMonOrderIdentity& identity);

	/** @brief 读取当前订单身份；订单不存在或身份已不可验证时返回 false。 */
	bool get_order_identity(uint32_t localid, WtOrdMonOrderIdentity& identity);

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
	 * @brief 生成带版本化身份的超时快照。
	 * @details 快照在锁内生成，callback 在锁外执行；迟到回调须使用传入身份调用身份化操作。
	 */
	void check_orders_with_identity(uint32_t expiresecs, uint64_t curTime,
		EnumOrderIdentityCallback callback);

	/**
	 * @brief 仅当候选仍对应同一条可撤订单时，将其转为不可验证超时状态。
	 * @return 只有本次成功完成状态转移时返回 true。
	 */
	bool mark_unverified_timeout(uint32_t localid, uint64_t generation,
		uint32_t expiresecs, uint64_t curTime);

	/** @brief 仅当身份仍匹配同一条可撤订单时，将其转为不可验证超时状态。 */
	bool mark_unverified_timeout(const WtOrdMonOrderIdentity& identity,
		uint32_t expiresecs, uint64_t curTime);

	/**
	 * @brief 宿主身份核验超时撤单的认领（迭代03 卡5 / N05-003）。
	 * @details 仅当候选身份仍对应一条已超时、可撤、未被认领的订单时认领成功；认领期间该单
	 *          不再出现在超时扫描中，直到终态回报删除或 fail_timeout_cancel 转为隔离。
	 */
	bool claim_timeout_cancel(uint32_t localid, uint64_t generation, uint32_t expiresecs, uint64_t curTime);

	/** @brief 宿主拒绝核验撤单：把认领转为持久的不可验证超时隔离（fail closed）。 */
	bool fail_timeout_cancel(uint32_t localid, uint64_t generation);

	/**
	 * @brief 认领、锁外调用 cancel(localid)、失败即隔离的完整流程。
	 * @return 1 已经宿主核验并发出撤单；0 候选已失效（终态/复用/已认领）；-1 宿主无法核验身份，已隔离。
	 */
	template <typename CancelFn>
	int cancel_timeout_verified(uint32_t localid, uint64_t generation, uint32_t expiresecs,
		uint64_t curTime, CancelFn&& cancel)
	{
		if (!claim_timeout_cancel(localid, generation, expiresecs, curTime))
			return 0;
		bool sent = false;
		try
		{
			sent = cancel(localid);
		}
		catch (...)
		{
			(void)fail_timeout_cancel(localid, generation);
			throw;
		}
		if (sent)
			return 1;
		(void)fail_timeout_cancel(localid, generation);
		return -1;
	}

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
		WtOrdMonOrderIdentity identity;
		bool unverifiedTimeout{ false };
		bool cancelClaimed{ false };	///< 宿主核验撤单已发出，等待终态
		bool terminal{ false };	///< 已收到终态回报（BUG-R17-001：重复终态回报只生效一次）
	};
	WtOrdMonOrderIdentity push_order_locked(uint32_t localid, uint64_t curTime, bool bCanCancel);
	/** @brief 订单ID映射表，本地订单ID到订单信息的映射 */
	typedef std::unordered_map<uint32_t, OrderInfo> IDMap;
	/** @brief 订单集合，存储所有被监控的订单 */
	IDMap			_orders;
	uint64_t		_monitor_instance_id{ 0 };
	uint64_t		_next_identity_sequence{ 1 };
	bool			_identity_sequence_exhausted{ false };
	bool			_has_unverified_timeout{ false };
	/** @brief 互斥锁，用于保护订单集合的线程安全 */
	StdRecurMutex	_mtx_ords;
};

/**
 * @brief Run an internal automatic action only if no timeout latch is observed.
 * @details The monitor lock is released before the callback begins. This helper
 *          does not make the check and action atomic across threads, and does not
 *          prove that other orders have verifiable identities. Production callers
 *          rely on per-unit callback serialization with timeout updates; the lock
 *          is not held across provider calls.
 */
template <typename Callback>
inline bool wtRunIfNoUnverifiedTimeout(WtOrdMon& ordersMon, Callback&& callback)
{
	if (ordersMon.has_unverified_timeout())
		return false;
	callback();
	return true;
}

