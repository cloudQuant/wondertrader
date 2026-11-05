/*!
 * \file WtOrdMon.cpp
 * \brief 订单监控器实现文件
 *
 * 本文件实现了订单监控器类，用于管理和跟踪交易订单的状态
 */
#include "WtOrdMon.h"

#include <atomic>
#include <limits>
#include <tuple>
#include <vector>

namespace
{
// 0 is the permanent exhausted sentinel; issuing UINT64_MAX transitions directly to 0.
uint64_t allocateMonitorInstanceId()
{
	static std::atomic<uint64_t> nextId{ 1 };
	uint64_t current = nextId.load(std::memory_order_relaxed);
	while (current != 0)
	{
		const uint64_t next = current == std::numeric_limits<uint64_t>::max() ? 0 : current + 1;
		if (nextId.compare_exchange_weak(current, next,
			std::memory_order_relaxed, std::memory_order_relaxed))
			return current;
	}
	return 0;
}
}

WtOrdMon::WtOrdMon()
	: _monitor_instance_id(allocateMonitorInstanceId()),
	  _identity_sequence_exhausted(_monitor_instance_id == 0)
{
}

/**
 * @brief 添加订单到监控器
 * @details 将一组订单ID添加到监控器中，并记录其下单时间和是否可撤销
 * @param ids 本地订单ID数组指针
 * @param cnt 订单数量
 * @param curTime 当前时间，作为订单下单时间
 * @param bCanCancel 是否可撤销，true为可撤销（默认），false为不可撤销
 */
void WtOrdMon::push_order(const uint32_t* ids, uint32_t cnt, uint64_t curTime, bool bCanCancel /* = true */)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	for (uint32_t idx = 0; idx < cnt; idx++)
		(void)push_order_locked(ids[idx], curTime, bCanCancel);
}

WtOrdMonOrderIdentity WtOrdMon::push_order_with_identity(uint32_t localid, uint64_t curTime,
	bool bCanCancel)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	return push_order_locked(localid, curTime, bCanCancel);
}

WtOrdMonOrderIdentity WtOrdMon::push_order_locked(uint32_t localid, uint64_t curTime, bool bCanCancel)
{
	OrderInfo& ordInfo = _orders[localid];
	ordInfo.enterTime = curTime;
	ordInfo.unverifiedTimeout = false;
	ordInfo.cancelClaimed = false;
	if (_identity_sequence_exhausted)
	{
		// 身份空间耗尽后拒绝继续发出可超时撤单的无身份记录。
		ordInfo.identity = WtOrdMonOrderIdentity();
		ordInfo.canCancel = false;
		ordInfo.unverifiedTimeout = bCanCancel;
		if (bCanCancel)
			_has_unverified_timeout = true;
		return ordInfo.identity;
	}

	const uint64_t sequence = _next_identity_sequence;
	ordInfo.identity = WtOrdMonOrderIdentity(localid,
		WtOrdMonOrderIdentity::kCurrentVersion, _monitor_instance_id, sequence);
	ordInfo.canCancel = bCanCancel;
	if (sequence == std::numeric_limits<uint64_t>::max())
		_identity_sequence_exhausted = true;
	else
		++_next_identity_sequence;
	return ordInfo.identity;
}

/**
 * @brief 从监控器中移除指定订单
 * @details 当订单完成成交或撤销时，从监控器中移除该订单
 * @param localid 本地订单ID
 */
void WtOrdMon::erase_order(uint32_t localid)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return;

	_orders.erase(it);
}

bool WtOrdMon::mark_order_terminal(uint32_t localid)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	// Keep the local-id association so late trade callbacks can still be applied,
	// but never consider a terminal order eligible for an expiry cancellation.
	it->second.canCancel = false;
	return true;
}

bool WtOrdMon::erase_order(const WtOrdMonOrderIdentity& identity)
{
	if (!identity.valid())
		return false;

	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(identity._localid);
	if (it == _orders.end())
		return false;

	const WtOrdMonOrderIdentity& current = it->second.identity;
	if (!current.valid() || current._version != identity._version ||
		current._instance_id != identity._instance_id ||
		current._sequence != identity._sequence)
		return false;

	_orders.erase(it);
	return true;
}

bool WtOrdMon::get_order_identity(uint32_t localid, WtOrdMonOrderIdentity& identity)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end() || !it->second.identity.valid())
	{
		identity = WtOrdMonOrderIdentity();
		return false;
	}

	identity = it->second.identity;
	return true;
}

/**
 * @brief 检查超时订单
 * @details 检查监控器中的订单，如果超过指定时间则触发回调函数进行处理
 * @param expiresecs 订单超时时间（秒）
 * @param curTime 当前时间（毫秒）
 * @param callback 处理超时订单的回调函数
 */
void WtOrdMon::check_orders(uint32_t expiresecs, uint64_t curTime, EnumOrderCallback callback)
{
	check_orders_with_generation(expiresecs, curTime,
		[callback](uint32_t localid, uint64_t) {
			callback(localid);
		});
}

void WtOrdMon::check_orders_with_generation(uint32_t expiresecs, uint64_t curTime,
	EnumOrderGenerationCallback callback)
{
	check_orders_with_identity(expiresecs, curTime,
		[callback](uint32_t localid, WtOrdMonOrderIdentity identity) {
			callback(localid, identity._sequence);
		});
}

void WtOrdMon::check_orders_with_identity(uint32_t expiresecs, uint64_t curTime,
	EnumOrderIdentityCallback callback)
{
	std::vector<std::pair<uint32_t, WtOrdMonOrderIdentity>> expired;
	const uint64_t expireMs = static_cast<uint64_t>(expiresecs) * 1000ULL;
	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		for (const auto& m : _orders)
		{
			const OrderInfo& ordInfo = m.second;
			if (!ordInfo.canCancel || ordInfo.unverifiedTimeout || ordInfo.cancelClaimed || !ordInfo.identity.valid())	// 不可撤、无可验证身份、已发核验撤单或已进入 fail-closed 状态
				continue;

			if (curTime < ordInfo.enterTime || curTime - ordInfo.enterTime < expireMs)
				continue;

			expired.emplace_back(m.first, ordInfo.identity);
		}
	}

	for (const auto& candidate : expired)
	{
		callback(candidate.first, candidate.second);
	}
}

bool WtOrdMon::claim_timeout_cancel(uint32_t localid, uint64_t generation,
	uint32_t expiresecs, uint64_t curTime)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	OrderInfo& order = it->second;
	if (!order.identity.valid() || order.identity._version != WtOrdMonOrderIdentity::kCurrentVersion ||
		order.identity._instance_id != _monitor_instance_id ||
		order.identity._sequence != generation || !order.canCancel || order.unverifiedTimeout || order.cancelClaimed)
		return false;

	const uint64_t expireMs = static_cast<uint64_t>(expiresecs) * 1000ULL;
	if (curTime < order.enterTime || curTime - order.enterTime < expireMs)
		return false;

	order.cancelClaimed = true;
	return true;
}

bool WtOrdMon::fail_timeout_cancel(uint32_t localid, uint64_t generation)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	OrderInfo& order = it->second;
	if (!order.identity.valid() || order.identity._sequence != generation || !order.cancelClaimed)
		return false;

	order.cancelClaimed = false;
	order.canCancel = false;
	order.unverifiedTimeout = true;
	_has_unverified_timeout = true;
	return true;
}

bool WtOrdMon::mark_unverified_timeout(uint32_t localid, uint64_t generation,
	uint32_t expiresecs, uint64_t curTime)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	OrderInfo& order = it->second;
	if (!order.identity.valid() || order.identity._version != WtOrdMonOrderIdentity::kCurrentVersion ||
		order.identity._instance_id != _monitor_instance_id ||
		order.identity._sequence != generation || !order.canCancel || order.unverifiedTimeout)
		return false;

	const uint64_t expireMs = static_cast<uint64_t>(expiresecs) * 1000ULL;
	if (curTime < order.enterTime || curTime - order.enterTime < expireMs)
		return false;

	order.canCancel = false;
	order.unverifiedTimeout = true;
	_has_unverified_timeout = true;
	return true;
}

bool WtOrdMon::mark_unverified_timeout(const WtOrdMonOrderIdentity& identity,
	uint32_t expiresecs, uint64_t curTime)
{
	if (!identity.valid())
		return false;

	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(identity._localid);
	if (it == _orders.end())
		return false;

	OrderInfo& order = it->second;
	if (order.identity._version != identity._version ||
		order.identity._instance_id != identity._instance_id ||
		order.identity._sequence != identity._sequence || !order.canCancel || order.unverifiedTimeout)
		return false;

	const uint64_t expireMs = static_cast<uint64_t>(expiresecs) * 1000ULL;
	if (curTime < order.enterTime || curTime - order.enterTime < expireMs)
		return false;

	order.canCancel = false;
	order.unverifiedTimeout = true;
	_has_unverified_timeout = true;
	return true;
}

/**
 * @brief 枚举所有订单
 * @details 遍历监控器中的所有订单，并通过回调函数返回订单的详细信息
 * @param cb 处理订单信息的回调函数，接收本地订单ID、下单时间和是否可撤销三个参数
 */
void WtOrdMon::enumOrder(EnumAllOrderCallback cb)
{
	std::vector<std::tuple<uint32_t, uint64_t, bool>> orders;
	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		orders.reserve(_orders.size());
		for (const auto& m : _orders)
			orders.emplace_back(m.first, m.second.enterTime, m.second.canCancel);
	}

	for (const auto& order : orders)
	{
		cb(std::get<0>(order), std::get<1>(order), std::get<2>(order));
	}
}

