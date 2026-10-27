/**
 * @file MatchEngine.cpp
 * @brief 回测撮合引擎实现文件
 * @details 该文件实现了回测过程中的订单撮合功能，模拟真实市场中的订单撮合机制
 */

#include "MatchEngine.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../WTSTools/WTSLogger.h"

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

/**
 * @brief 激活待处理的订单
 * @details 遍历所有订单，将状态为0的订单激活，并通知回调接口
 * 
 * @param stdCode 标准合约代码
 * @param to_erase 需要删除的订单ID列表
 */
void MatchEngine::fire_orders(const char* stdCode, OrderIDs& to_erase)
{
	for (auto& v : _orders)
	{
		uint32_t localid = v.first;
		OrderInfo& ordInfo = (OrderInfo&)v.second;

		if (ordInfo._state == 0)	//需要激活
		{
			_sink->handle_entrust(localid, stdCode, true, "", ordInfo._time);
			_sink->handle_order(localid, stdCode, ordInfo._buy, ordInfo._left, ordInfo._limit, false, ordInfo._time);
			ordInfo._state = 1;
		}
	}
}

/**
 * @brief 撮合订单
 * @details 根据当前行情数据对所有活跃订单进行撮合处理，包括撤单和成交处理
 * 
 * @param curTick 当前Tick数据指针
 * @param to_erase 需要删除的订单ID列表
 */
void MatchEngine::match_orders(WTSTickData* curTick, OrderIDs& to_erase)
{
	uint64_t curTime = (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime();
	uint64_t curUnixTime = TimeUtils::makeTime(curTick->actiondate(), curTick->actiontime());

	for (auto& v : _orders)
	{
		uint32_t localid = v.first;
		OrderInfo& ordInfo = (OrderInfo&)v.second;

		if (ordInfo._state == 9)//要撤单
		{
			_sink->handle_order(localid, ordInfo._code, ordInfo._buy, 0, ordInfo._limit, true, ordInfo._time);
			ordInfo._state = 99;

			to_erase.emplace_back(localid);

			WTSLogger::info("订单{}已撤销, 剩余数量: {}", localid, ordInfo._left*(ordInfo._buy ? 1 : -1));
			ordInfo._left = 0;
			continue;
		}

		if (ordInfo._state != 1 || curTick->volume() == 0)
			continue;

		if (ordInfo._buy)
		{
			double price;
			double volume;

			//主动订单就按照对手价
			if (ordInfo._positive)
			{
				price = curTick->askprice(0);
				volume = curTick->askqty(0);
			}
			else
			{
				price = curTick->price();
				volume = curTick->volume();
			}

			if (decimal::le(price, ordInfo._limit))
			{
				//如果价格相等,需要先看排队位置,如果价格不等说明已经全部被大单吃掉了
				if (!ordInfo._positive && decimal::eq(price, ordInfo._limit))
				{
					double& quepos = ordInfo._queue;

					//如果成交量小于排队位置,则不能成交
					if (volume <= quepos)
					{
						quepos -= volume;
						continue;
					}
					else if (quepos != 0)
					{
						//如果成交量大于排队位置,则可以成交
						volume -= quepos;
						quepos = 0;
					}
				}
				else if (!ordInfo._positive)
				{
					volume = ordInfo._left;
				}

				double qty = min(volume, ordInfo._left);
				if (decimal::eq(qty, 0.0))
					qty = 1;

				_sink->handle_trade(localid, ordInfo._code, ordInfo._buy, qty, ordInfo._price, price, ordInfo._time);

				ordInfo._traded += qty;
				ordInfo._left -= qty;

				_sink->handle_order(localid, ordInfo._code, ordInfo._buy, ordInfo._left, price, false, ordInfo._time);

				if (ordInfo._left == 0)
					to_erase.emplace_back(localid);
			}
		}

		if (!ordInfo._buy)
		{
			double price;
			double volume;

			//主动订单就按照对手价
			if (ordInfo._positive)
			{
				price = curTick->bidprice(0);
				volume = curTick->bidqty(0);
			}
			else
			{
				price = curTick->price();
				volume = curTick->volume();
			}

			if (decimal::ge(price, ordInfo._limit))
			{
				//如果价格相等,需要先看排队位置,如果价格不等说明已经全部被大单吃掉了
				if (!ordInfo._positive && decimal::eq(price, ordInfo._limit))
				{
					double& quepos = ordInfo._queue;

					//如果成交量小于排队位置,则不能成交
					if (volume <= quepos)
					{
						quepos -= volume;
						continue;
					}
					else if (quepos != 0)
					{
						//如果成交量大于排队位置,则可以成交
						volume -= quepos;
						quepos = 0;
					}
				}
				else if (!ordInfo._positive)
				{
					volume = ordInfo._left;
				}

				double qty = min(volume, ordInfo._left);
				if (decimal::eq(qty, 0.0))
					qty = 1;

				_sink->handle_trade(localid, ordInfo._code, ordInfo._buy, qty, ordInfo._price, price, ordInfo._time);
				ordInfo._traded += qty;
				ordInfo._left -= qty;

				_sink->handle_order(localid, ordInfo._code, ordInfo._buy, ordInfo._left, price, false, ordInfo._time);

				if (ordInfo._left == 0)
					to_erase.emplace_back(localid);
			}

		}
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
			volume = curTick->askqty(i);
		}
	}

	//卖一和买一之间的报价必须全部清除掉
	if (!curBook._items.empty())
	{
		auto sit = curBook._items.lower_bound(curBook._bid_px);
		if (sit->first == curBook._bid_px)
			sit++;

		auto eit = curBook._items.lower_bound(curBook._ask_px);

		if (sit->first <= eit->first)
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
	if (decimal::ge(price, lastTick->askprice(0)))
		ordInfo._positive = true;
	else if (decimal::eq(price, lastTick->bidprice(0)))
		ordInfo._queue = lastTick->bidqty(0);
	if (decimal::eq(price, lastTick->price()))
		ordInfo._queue = (uint32_t)round((lastTick->askqty(0)*lastTick->askprice(0) + lastTick->bidqty(0)*lastTick->bidprice(0)) / (lastTick->askprice(0) + lastTick->bidprice(0)));

	//排队位置按照平均撤单率,撤销掉部分
	ordInfo._queue -= (uint32_t)round(ordInfo._queue*_cancelrate);
	ordInfo._time = curTime;

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
	for (auto& v : _orders)
	{
		OrderInfo& ordInfo = (OrderInfo&)v.second;
		if (ordInfo._state != 1)
			continue;

		double left = qty;
		if (ordInfo._buy == isBuy)
		{
			uint32_t localid = v.first;
			ret.emplace_back(localid);
			ordInfo._state = 9;
			cb(ordInfo._left*(ordInfo._buy ? 1 : -1));

			if (qty != 0)
			{
				if ((int32_t)left <= ordInfo._left)
					break;

				left -= ordInfo._left;
			}
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
	ordInfo._state = 9;

	return ordInfo._left*(ordInfo._buy ? 1 : -1);
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
	if (NULL == curTick)
		return;

	if (NULL == _tick_cache)
		_tick_cache = WTSTickCache::create();

	_tick_cache->add(stdCode, curTick, true);

	update_lob(curTick);

	OrderIDs to_erase;
	//检查订单状态
	fire_orders(stdCode, to_erase);

	//撮合
	match_orders(curTick, to_erase);

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