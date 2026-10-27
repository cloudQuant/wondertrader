/*!
 * \file WtOrdMon.cpp
 * \brief 订单监控器实现文件
 *
 * 本文件实现了订单监控器类，用于管理和跟踪交易订单的状态
 */
#include "WtOrdMon.h"

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
		OrderPair& ordInfo = _orders[localid];
		ordInfo.first = curTime;
		ordInfo.second = bCanCancel;
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
	if (_orders.empty())
		return;


	StdLocker<StdRecurMutex> lock(_mtx_ords);
	for (auto& m : _orders)
	{
		uint32_t localid = m.first;
		OrderPair& ordInfo = m.second;
		if (!ordInfo.second)	//如果不能撤单，则直接跳过（一般涨跌停价的挂单是不能撤单的）
			continue;

		auto entertm = ordInfo.first;
		if (curTime - entertm < expiresecs * 1000)
			continue;

		callback(m.first);
	}
}

/**
 * @brief 枚举所有订单
 * @details 遍历监控器中的所有订单，并通过回调函数返回订单的详细信息
 * @param cb 处理订单信息的回调函数，接收本地订单ID、下单时间和是否可撤销三个参数
 */
void WtOrdMon::enumOrder(EnumAllOrderCallback cb)
{
	if (_orders.empty())
		return;

	StdLocker<StdRecurMutex> lock(_mtx_ords);
	for (auto& m : _orders)
	{
		uint32_t localid = m.first;
		OrderPair& ordInfo = m.second;
		uint64_t entertm = ordInfo.first;
		bool cancancel = ordInfo.second;
		cb(localid, entertm, cancancel);
	}
}

