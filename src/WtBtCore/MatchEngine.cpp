/**
 * @file MatchEngine.cpp
 * @brief 回测撮合引擎实现文件
 * @details 该文件实现了回测过程中的订单撮合功能，模拟真实市场中的订单撮合机制
 */

#include "MatchEngine.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/decimal.h"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#define PRICE_DOUBLE_TO_INT_P(x) ((int32_t)((x)*10000.0 + 0.5))
#define PRICE_DOUBLE_TO_INT_N(x) ((int32_t)((x)*10000.0 - 0.5))
#define PRICE_DOUBLE_TO_INT(x) (((x)==DBL_MAX)?0:((x)>0?PRICE_DOUBLE_TO_INT_P(x):PRICE_DOUBLE_TO_INT_N(x)))

extern uint32_t makeLocalOrderID();

/**
 * @brief 初始化撮合引擎
 * @details 从配置中读取撮合引擎的相关参数，如撤单率等
 * 
 * @param cfg 配置项指针，包含撮合引擎的配置参数
 */
void MatchEngine::init(WTSVariant* cfg)
{
	if (cfg == NULL)
		return;

	_cancelrate = cfg->getDouble("cancelrate");
}

/**
 * @brief 清空撮合引擎中的所有订单
 * @details 清除撮合引擎中的所有订单数据，通常在重置回测环境或结束回测时调用
 */
void MatchEngine::clear()
{
	_orders.clear();
}

uint64_t MatchEngine::next_order_sequence()
{
	if (_next_order_sequence == std::numeric_limits<uint64_t>::max())
	{
		// Rebase live orders before the per-engine counter wraps. Local IDs are
		// generated globally and can wrap independently, so they cannot serve as
		// the stable tie-breaker for orders created at the same timestamp.
		std::vector<std::pair<uint64_t, uint32_t>> existing;
		existing.reserve(_orders.size());
		for (const auto& entry : _orders)
			existing.emplace_back(entry.second._creation_sequence, entry.first);

		std::sort(existing.begin(), existing.end());
		if (existing.size() >= std::numeric_limits<uint64_t>::max())
			throw std::overflow_error("MatchEngine order sequence exhausted");

		_next_order_sequence = 0;
		for (const auto& entry : existing)
		{
			auto it = _orders.find(entry.second);
			if (it != _orders.end())
				it->second._creation_sequence = ++_next_order_sequence;
		}
	}

	return ++_next_order_sequence;
}

/**
 * @brief 激活待处理的订单
 * @details 遍历所有订单，将状态为0的订单激活，并通知回调接口
 * 
 * @param stdCode 标准合约代码
 * @param to_erase 需要删除的订单ID列表
 */
void MatchEngine::fire_orders(const char* stdCode, OrderIDs& /*to_erase*/)
{
	if (stdCode == NULL)
		return;

	OrderIDs pending;
	pending.reserve(_orders.size());
	for (const auto& entry : _orders)
	{
		const OrderInfo& ordInfo = entry.second;
		if (ordInfo._state == 0 && strcmp(ordInfo._code, stdCode) == 0)
			pending.emplace_back(entry.first);
	}

	for (uint32_t localid : pending)
	{
		OrderInfo ordInfo;
		{
			auto it = _orders.find(localid);
			if (it == _orders.end() || it->second._state != 0 || strcmp(it->second._code, stdCode) != 0)
				continue;
			ordInfo = it->second;
		}

		const bool isBuy = ordInfo._buy;
		const std::string orderCode = ordInfo._code;
		const uint64_t orderTime = ordInfo._time;
		_sink->handle_entrust(localid, orderCode.c_str(), true, "", orderTime);

		// A callback may insert or clear orders. Re-find this one before reading
		// it again; no reference into the unordered map survives the callback.
		double currentLeft = 0.0;
		double currentLimit = 0.0;
		{
			auto it = _orders.find(localid);
			if (it == _orders.end() || strcmp(it->second._code, stdCode) != 0)
				continue;
			currentLeft = it->second._left;
			currentLimit = it->second._limit;
		}
		_sink->handle_order(localid, orderCode.c_str(), isBuy, currentLeft, currentLimit, false, orderTime);

		{
			auto it = _orders.find(localid);
			if (it != _orders.end() && strcmp(it->second._code, stdCode) == 0 && it->second._state == 0)
				it->second._state = 1;
		}
	}
}

/**
 * @brief 撮合订单
 * @details 根据当前行情数据对所有活跃订单进行撮合处理，包括撤单和成交处理
 * 
 * @param stdCode 当前 Tick 的标准合约代码
 * @param curTick 当前Tick数据指针
 * @param to_erase 需要删除的订单ID列表
 * @param cancelPendingAtTickStart 在当前 Tick 到达前已处于待撤销状态的订单
 */
void MatchEngine::match_orders(const char* stdCode, WTSTickData* curTick, OrderIDs& to_erase,
	const OrderIDs& cancelPendingAtTickStart)
{
	if (stdCode == NULL || curTick == NULL)
		return;

	OrderIDs candidates;
	candidates.reserve(_orders.size());
	for (const auto& entry : _orders)
	{
		if (strcmp(entry.second._code, stdCode) == 0)
			candidates.emplace_back(entry.first);
	}

	for (uint32_t localid : candidates)
	{
		OrderInfo ordInfo;
		{
			auto it = _orders.find(localid);
			if (it == _orders.end() || strcmp(it->second._code, stdCode) != 0)
				continue;
			ordInfo = it->second;
		}

		if (ordInfo._state == 9)	// cancel response is delivered on a later tick for this code
		{
			if (std::find(cancelPendingAtTickStart.begin(), cancelPendingAtTickStart.end(), localid) ==
				cancelPendingAtTickStart.end())
				continue;

			const bool isBuy = ordInfo._buy;
			const double left = ordInfo._left;
			const double limit = ordInfo._limit;
			const uint64_t orderTime = ordInfo._time;
			const std::string orderCode = ordInfo._code;
			_sink->handle_order(localid, orderCode.c_str(), isBuy, 0, limit, true, orderTime);

			// Keep state 9 visible during the cancel callback, then transition to
			// terminal state if the callback did not clear the matcher.
			{
				auto it = _orders.find(localid);
				if (it != _orders.end() && strcmp(it->second._code, stdCode) == 0 && it->second._state == 9)
				{
					it->second._state = 99;
					it->second._left = 0;
				}
			}
			to_erase.emplace_back(localid);
			WTSLogger::info("订单{}已撤销, 剩余数量: {}", localid, left*(isBuy ? 1 : -1));
			continue;
		}

		if (ordInfo._state != 1 || curTick->volume() == 0)
			continue;

		const bool isBuy = ordInfo._buy;
		const bool positive = ordInfo._positive;
		double price = positive ? (isBuy ? curTick->askprice(0) : curTick->bidprice(0)) : curTick->price();
		double volume = positive ? (isBuy ? curTick->askqty(0) : curTick->bidqty(0)) : curTick->volume();
		const double limit = ordInfo._limit;
		if (isBuy ? !decimal::le(price, limit) : !decimal::ge(price, limit))
			continue;

		// When the trade price equals the limit, account for earlier queue volume.
		if (!positive && decimal::eq(price, limit))
		{
			if (volume <= ordInfo._queue)
			{
				ordInfo._queue -= volume;
				{
					auto it = _orders.find(localid);
					if (it != _orders.end() && it->second._state == 1 && strcmp(it->second._code, stdCode) == 0)
						it->second._queue = ordInfo._queue;
				}
				continue;
			}
			if (ordInfo._queue != 0)
			{
				volume -= ordInfo._queue;
				ordInfo._queue = 0;
			}
		}
		else if (!positive)
		{
			volume = ordInfo._left;
		}

		double qty = min(volume, ordInfo._left);
		// A zero or negative displayed contra quantity cannot fill an order.
		// Keep the order active and unchanged until a later tick has available
		// contra volume.
		if (qty <= 0.0)
			continue;

		const double firePrice = ordInfo._price;
		const uint64_t orderTime = ordInfo._time;
		const std::string orderCode = ordInfo._code;
		ordInfo._traded += qty;
		ordInfo._left -= qty;
		const double leftover = ordInfo._left;
		{
			auto it = _orders.find(localid);
			if (it == _orders.end() || it->second._state != 1 || strcmp(it->second._code, stdCode) != 0)
				continue;
			it->second._queue = ordInfo._queue;
			it->second._traded = ordInfo._traded;
			it->second._left = ordInfo._left;
		}
		if (leftover == 0.0)
			to_erase.emplace_back(localid);

		// Update matcher state before callbacks and keep no map reference across
		// them: callbacks may submit or clear orders and rehash the underlying map.
		_sink->handle_trade(localid, orderCode.c_str(), isBuy, qty, firePrice, price, orderTime);
		bool orderStillPresent = false;
		{
			auto it = _orders.find(localid);
			orderStillPresent = it != _orders.end() && strcmp(it->second._code, stdCode) == 0;
		}
		if (orderStillPresent)
			_sink->handle_order(localid, orderCode.c_str(), isBuy, leftover, price, false, orderTime);
	}
}

/**
 * @brief 更新限价委托账本(Limit Order Book)
 * @details 根据当前Tick数据更新内部的委托账本数据，包括当前价格、买一价、卖一价等
 * 
 * @param curTick 当前Tick数据指针
 */
void MatchEngine::update_lob(WTSTickData* curTick)
{
	LmtOrdBook& curBook = _lmt_ord_books[curTick->code()];
	curBook._cur_px = PRICE_DOUBLE_TO_INT(curTick->price());
	curBook._ask_px = PRICE_DOUBLE_TO_INT(curTick->askprice(0));
	curBook._bid_px = PRICE_DOUBLE_TO_INT(curTick->bidprice(0));

	for (uint32_t i = 0; i < 10; i++)
	{
		if (PRICE_DOUBLE_TO_INT(curTick->askprice(i)) == 0 && PRICE_DOUBLE_TO_INT(curTick->bidprice(i)) == 0)
			break;

		uint32_t px = PRICE_DOUBLE_TO_INT(curTick->askprice(i));
		if (px != 0)
		{
			double& volume = curBook._items[px];
			volume = curTick->askqty(i);
		}

		px = PRICE_DOUBLE_TO_INT(curTick->bidprice(i));
		if (px != 0)
		{
			double& volume = curBook._items[px];
			volume = curTick->bidqty(i);
		}
	}

	//卖一和买一之间的报价必须全部清除掉
	if (!curBook._items.empty())
	{
		auto end = curBook._items.end();
		auto sit = curBook._items.lower_bound(curBook._bid_px);
		if (sit != end && sit->first == curBook._bid_px)
			sit++;

		auto eit = curBook._items.lower_bound(curBook._ask_px);

		// 单边盘口及锁定/交叉盘口都可能令其中一个搜索迭代器到达 end。
		// 只有两个有效边界且范围正向时，才清理买一和卖一之间的旧档位。
		if (sit != end && eit != end && sit->first < eit->first)
			curBook._items.erase(sit, eit);
	}
}


/**
 * @brief 创建买入订单
 * @details 创建一个买入订单，并计算订单排队位置
 * 
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param curTime 当前时间戳
 * @return OrderIDs 返回创建的订单ID列表
 */
OrderIDs MatchEngine::buy(const char* stdCode, double price, double qty, uint64_t curTime)
{
	WTSTickData* lastTick = grab_last_tick(stdCode);
	if (lastTick == NULL)
		return OrderIDs();

	const uint64_t creationSequence = next_order_sequence();
	uint32_t localid = makeLocalOrderID();
	OrderInfo& ordInfo = _orders[localid];
	strcpy(ordInfo._code, stdCode);
	ordInfo._buy = true;
	ordInfo._limit = price;
	ordInfo._qty = qty;
	ordInfo._left = qty;
	ordInfo._price = lastTick->price();

	//订单排队,如果是对手价,则按照对手价的挂单量来排队
	//如果是最新价,则按照买一卖一的加权平均
	if (lastTick->askprice(0) > 0.0 && decimal::ge(price, lastTick->askprice(0)))
		ordInfo._positive = true;
	else if (decimal::eq(price, lastTick->bidprice(0)))
		ordInfo._queue = lastTick->bidqty(0);
	if (decimal::eq(price, lastTick->price()))
		ordInfo._queue = (uint32_t)round((lastTick->askqty(0)*lastTick->askprice(0) + lastTick->bidqty(0)*lastTick->bidprice(0)) / (lastTick->askprice(0) + lastTick->bidprice(0)));

	//排队位置按照平均撤单率,撤销掉部分
	ordInfo._queue -= (uint32_t)round(ordInfo._queue*_cancelrate);
	ordInfo._time = curTime;
	ordInfo._creation_sequence = creationSequence;

	lastTick->release();

	OrderIDs ret;
	ret.emplace_back(localid);
	return ret;
}

/**
 * @brief 创建卖出订单
 * @details 创建一个卖出订单，并计算订单排队位置
 * 
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param curTime 当前时间戳
 * @return OrderIDs 返回创建的订单ID列表
 */
OrderIDs MatchEngine::sell(const char* stdCode, double price, double qty, uint64_t curTime)
{
	WTSTickData* lastTick = grab_last_tick(stdCode);
	if (lastTick == NULL)
		return OrderIDs();

	const uint64_t creationSequence = next_order_sequence();
	uint32_t localid = makeLocalOrderID();
	OrderInfo& ordInfo = _orders[localid];
	strcpy(ordInfo._code, stdCode);
	ordInfo._buy = false;
	ordInfo._limit = price;
	ordInfo._qty = qty;
	ordInfo._left = qty;
	ordInfo._price = lastTick->price();

	//订单排队,如果是对手价,则按照对手价的挂单量来排队
	//如果是最新价,则按照买一卖一的加权平均
	if (decimal::eq(price, lastTick->askprice(0)))
		ordInfo._queue = lastTick->askqty(0);
	else if (decimal::le(price, lastTick->bidprice(0)))
		ordInfo._positive = true;
	if (decimal::eq(price, lastTick->price()))
		ordInfo._queue = (uint32_t)round((lastTick->askqty(0)*lastTick->askprice(0) + lastTick->bidqty(0)*lastTick->bidprice(0)) / (lastTick->askprice(0) + lastTick->bidprice(0)));

	ordInfo._queue -= (uint32_t)round(ordInfo._queue*_cancelrate);
	ordInfo._time = curTime;
	ordInfo._creation_sequence = creationSequence;

	lastTick->release();

	OrderIDs ret;
	ret.emplace_back(localid);
	return ret;
}

/**
 * @brief 按合约和方向撤销订单
 * @details 撤销指定合约和方向的订单，可以指定撤销数量
 * 
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入订单
 * @param qty 需要撤销的数量，为0时撤销全部
 * @param cb 撤单回调函数，用于通知撤单结果
 * @return OrderIDs 返回撤销的订单ID列表
 */
OrderIDs MatchEngine::cancel(const char* stdCode, bool isBuy, double qty, FuncCancelCallback cb)
{
	OrderIDs ret;
	const bool allCodes = (stdCode == NULL || stdCode[0] == '\0');
	struct CancelCandidate
	{
		uint32_t localid;
		uint64_t time;
		uint64_t creation_sequence;
	};
	std::vector<CancelCandidate> candidates;
	candidates.reserve(_orders.size());
	for (const auto& entry : _orders)
	{
		const OrderInfo& ordInfo = entry.second;
		if (ordInfo._state == 1 && ordInfo._buy == isBuy &&
			(allCodes || strcmp(ordInfo._code, stdCode) == 0))
			candidates.push_back({ entry.first, ordInfo._time, ordInfo._creation_sequence });
	}
	std::sort(candidates.begin(), candidates.end(), [](const CancelCandidate& lhs, const CancelCandidate& rhs) {
		if (lhs.time != rhs.time)
			return lhs.time < rhs.time;
		return lhs.creation_sequence < rhs.creation_sequence;
	});

	double left = qty;
	for (const CancelCandidate& candidate : candidates)
	{
		const uint32_t localid = candidate.localid;
		double orderLeft = 0.0;
		double change = 0.0;
		{
			auto it = _orders.find(localid);
			if (it == _orders.end())
				continue;
			OrderInfo& ordInfo = it->second;
			if (ordInfo._state != 1 || ordInfo._buy != isBuy ||
				(!allCodes && strcmp(ordInfo._code, stdCode) != 0))
				continue;

			orderLeft = ordInfo._left;
			change = orderLeft * (ordInfo._buy ? 1 : -1);
			ordInfo._state = 9;
		}
		ret.emplace_back(localid);
		if (cb)
			cb(change);

		if (qty != 0)
		{
			if (left <= orderLeft)
				break;
			left -= orderLeft;
		}
	}

	return ret;
}

/**
 * @brief 按订单ID撤销订单
 * @details 撤销指定订单ID的订单
 * 
 * @param localid 本地订单ID
 * @return double 返回撤销的数量，买入为正值，卖出为负值，撤单失败返回0
 */
double MatchEngine::cancel(uint32_t localid)
{
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return 0.0;

	OrderInfo& ordInfo = (OrderInfo&)it->second;
	if ((ordInfo._state != 0 && ordInfo._state != 1) || ordInfo._left <= 0.0)
		return 0.0;

	ordInfo._state = 9;

	return ordInfo._left*(ordInfo._buy ? 1 : -1);
}

std::vector<MatchEngine::OrderSnapshot> MatchEngine::get_order_snapshots(const char* stdCode) const
{
	std::vector<OrderSnapshot> snapshots;
	const bool allCodes = (stdCode == NULL || stdCode[0] == '\0');
	snapshots.reserve(_orders.size());

	// MatchEngine is driven synchronously by the replayer; matcher callbacks can
	// re-enter ExecuteContext on that same thread. Copying here avoids retaining
	// references into _orders without adding a lock around those callbacks.
	for (auto it = _orders.begin(); it != _orders.end(); ++it)
	{
		const OrderInfo& ordInfo = it->second;
		if (!allCodes && strcmp(ordInfo._code, stdCode) != 0)
			continue;
		if (ordInfo._left <= 0.0)
			continue;
		if (ordInfo._state != 0 && ordInfo._state != 1 && ordInfo._state != 9)
			continue;

		OrderSnapshot snapshot{};
		snapshot.localid = it->first;
		strncpy(snapshot.code, ordInfo._code, sizeof(snapshot.code) - 1);
		snapshot.buy = ordInfo._buy;
		snapshot.qty = ordInfo._qty;
		snapshot.left = ordInfo._left;
		snapshot.traded = ordInfo._traded;
		snapshot.price = ordInfo._limit;
		snapshot.state = ordInfo._state;
		snapshot.time = ordInfo._time;
		snapshots.emplace_back(snapshot);
	}

	return snapshots;
}

/**
 * @brief 处理Tick数据
 * @details 处理新到达的Tick数据，包括更新委托账本、激活订单和进行订单撮合
 * 
 * @param stdCode 标准合约代码
 * @param curTick 当前Tick数据指针
 */
void MatchEngine::handle_tick(const char* stdCode, WTSTickData* curTick)
{
	if (NULL == stdCode || NULL == curTick)
		return;

	if (NULL == _tick_cache)
		_tick_cache = WTSTickCache::create();

	_tick_cache->add(stdCode, curTick, true);

	update_lob(curTick);

	OrderIDs cancelPendingAtTickStart;
	for (const auto& entry : _orders)
	{
		if (entry.second._state == 9 && strcmp(entry.second._code, stdCode) == 0)
			cancelPendingAtTickStart.emplace_back(entry.first);
	}

	OrderIDs to_erase;
	//检查订单状态
	fire_orders(stdCode, to_erase);

	//撮合
	match_orders(stdCode, curTick, to_erase, cancelPendingAtTickStart);

	for (uint32_t localid : to_erase)
	{
		auto it = _orders.find(localid);
		if (it != _orders.end())
			_orders.erase(it);
	}
}

/**
 * @brief 获取最新的Tick数据
 * @details 从缓存中获取指定合约的最新Tick数据
 * 
 * @param stdCode 标准合约代码
 * @return WTSTickData* 返回Tick数据指针，如果不存在则返回NULL
 */
WTSTickData* MatchEngine::grab_last_tick(const char* stdCode)
{
	if (NULL == _tick_cache)
		return NULL;

	return (WTSTickData*)_tick_cache->grab(stdCode);
}
