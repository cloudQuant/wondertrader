/*!
 * \file WtOrdMon.cpp
 * \brief 订单监控器实现文件
 *
 * 本文件实现了订单监控器类，用于管理和跟踪交易订单的状态
 */
#include "WtOrdMon.h"

#include <tuple>
#include <vector>

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
	{
		uint32_t localid = ids[idx];
		OrderInfo& ordInfo = _orders[localid];
		ordInfo.enterTime = curTime;
		ordInfo.canCancel = bCanCancel;
		ordInfo.generation = _next_generation++;
		if (_next_generation == 0)
			_next_generation = 1;
		ordInfo.unverifiedTimeout = false;
	}
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
	std::vector<WtOrdMonTimeoutCandidate> expired;
	const uint64_t expireMs = static_cast<uint64_t>(expiresecs) * 1000ULL;
	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		for (const auto& m : _orders)
		{
			const OrderInfo& ordInfo = m.second;
			if (!ordInfo.canCancel || ordInfo.unverifiedTimeout)	// 不可撤或已经进入 fail-closed 状态
				continue;

			if (curTime < ordInfo.enterTime || curTime - ordInfo.enterTime < expireMs)
				continue;

			expired.push_back({ m.first, ordInfo.generation });
		}
	}

	for (const WtOrdMonTimeoutCandidate& candidate : expired)
	{
		callback(candidate.localid, candidate.generation);
	}
}

bool WtOrdMon::mark_unverified_timeout(uint32_t localid, uint64_t generation,
	uint32_t expiresecs, uint64_t curTime)
{
	StdLocker<StdRecurMutex> lock(_mtx_ords);
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	OrderInfo& order = it->second;
	if (order.generation != generation || !order.canCancel || order.unverifiedTimeout)
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

