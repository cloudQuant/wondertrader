/**
 * @file MatchEngine.h
 * @brief 回测撮合引擎头文件
 * @details 定义了回测过程中的订单撮合机制相关的类和接口，用于模拟真实市场中的订单撮合过程
 */

#pragma once
#include <stdint.h>
#include <map>
#include <vector>
#include <functional>
#include <string.h>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSCollection.hpp"
#include "../Includes/FasterDefs.h"

NS_WTP_BEGIN
class WTSTickData;
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

typedef std::vector<uint32_t> OrderIDs;

typedef WTSHashMap<std::string>	WTSTickCache;

/**
 * @brief 撮合引擎回调接口
 * @details 定义了撮合引擎需要回调的方法，包括成交、订单和委托回报
 */
class IMatchSink
{
public:
	/**
	 * @brief 成交回报回调函数
	 * @details 当订单成交时调用此函数进行通知
	 * 
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否为买入订单
	 * @param vol 成交数量，这里没有正负，通过isBuy确定买入还是卖出
	 * @param fireprice 委托价格
	 * @param price 成交价格
	 * @param ordTime 订单时间
	 */
	virtual void handle_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double fireprice, double price, uint64_t ordTime) = 0;

	/**
	 * @brief 订单状态回报回调函数
	 * @details 当订单状态发生变化时调用此函数进行通知，包括创建、部分成交和撤销等
	 * 
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param isBuy 是否为买入订单
	 * @param leftover 剩余数量
	 * @param price 委托价格
	 * @param isCanceled 是否已撤销
	 * @param ordTime 订单时间
	 */
	virtual void handle_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled, uint64_t ordTime) = 0;

	/**
	 * @brief 委托回报回调函数
	 * @details 当委托发送到市场后调用此函数进行通知，包含委托成功或失败的信息
	 * 
	 * @param localid 本地订单ID
	 * @param stdCode 标准合约代码
	 * @param bSuccess 委托是否成功
	 * @param message 委托消息，如错误原因
	 * @param ordTime 订单时间
	 */
	virtual void handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, uint64_t ordTime) = 0;
};

/**
 * @brief 撤单回调函数类型
 * @details 定义了撤单操作的回调函数类型，参数为撤单数量（正值表示买入，负值表示卖出）
 */
typedef std::function<void(double)> FuncCancelCallback;

/**
 * @brief 回测撮合引擎类
 * @details 实现了回测过程中的订单撮合功能，模拟真实市场中的订单撮合机制，
 * 包括订单创建、撮合、排队和撤销等功能
 */
class MatchEngine
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化撮合引擎，设置初始参数
	 */
	MatchEngine() : _tick_cache(NULL),_cancelrate(0), _sink(NULL)
	{
		
	}
private:
	/**
	 * @brief 激活待处理的订单
	 * @details 遍历所有订单，将状态为0的订单激活，并通知回调接口
	 * 
	 * @param stdCode 标准合约代码
	 * @param to_erase 需要删除的订单ID列表
	 */
	void	fire_orders(const char* stdCode, OrderIDs& to_erase);
	/**
	 * @brief 撮合订单
	 * @details 根据当前行情数据对所有活跃订单进行撮合处理，包括撤单和成交处理
	 * 
	 * @param curTick 当前Tick数据指针
	 * @param to_erase 需要删除的订单ID列表
	 */
	void	match_orders(WTSTickData* curTick, OrderIDs& to_erase);
	/**
	 * @brief 更新限价委托账本(Limit Order Book)
	 * @details 根据当前Tick数据更新内部的委托账本数据，包括当前价格、买一价和卖一价等
	 * 
	 * @param curTick 当前Tick数据指针
	 */
	void	update_lob(WTSTickData* curTick);

	/**
	 * @brief 获取最新的Tick数据
	 * @details 从缓存中获取指定合约的最新Tick数据
	 * 
	 * @param stdCode 标准合约代码
	 * @return WTSTickData* 返回Tick数据指针，如果不存在则返回NULL
	 */
	inline WTSTickData*	grab_last_tick(const char* stdCode);

public:
	/**
	 * @brief 初始化撮合引擎
	 * @details 从配置中读取撮合引擎的相关参数，如撤单率等
	 * 
	 * @param cfg 配置项指针，包含撮合引擎的配置参数
	 */
	void	init(WTSVariant* cfg);

	/**
	 * @brief 注册回调接口
	 * @details 注册撮合引擎的回调接口，用于接收成交、订单和委托回报
	 * 
	 * @param sink 回调接口指针
	 */
	void	regisSink(IMatchSink* sink) { _sink = sink; }

	/**
	 * @brief 清空撮合引擎中的所有订单
	 * @details 清除撮合引擎中的所有订单数据，通常在重置回测环境或结束回测时调用
	 */
	void	clear();

	/**
	 * @brief 处理Tick数据
	 * @details 处理新到达的Tick数据，包括更新委托账本、激活订单和进行订单撮合
	 * 
	 * @param stdCode 标准合约代码
	 * @param curTick 当前Tick数据指针
	 */
	void	handle_tick(const char* stdCode, WTSTickData* curTick);

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
	OrderIDs	buy(const char* stdCode, double price, double qty, uint64_t curTime);
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
	OrderIDs	sell(const char* stdCode, double price, double qty, uint64_t curTime);
	/**
	 * @brief 按订单ID撤销订单
	 * @details 撤销指定订单ID的订单
	 * 
	 * @param localid 本地订单ID
	 * @return double 返回撤销的数量，买入为正值，卖出为负值，撤单失败返回0
	 */
	double		cancel(uint32_t localid);
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
	virtual OrderIDs cancel(const char* stdCode, bool isBuy, double qty, FuncCancelCallback cb);

private:
	/**
	 * @brief 订单信息结构体
	 * @details 存储订单的各种信息，包括合约代码、方向、数量、价格等
	 */
	typedef struct _OrderInfo
	{
		char		_code[32];      ///< 合约代码
		bool		_buy;           ///< 是否为买入订单
		double		_qty;           ///< 委托总数量
		double		_left;          ///< 剩余数量
		double		_traded;        ///< 已成交数量
		double		_limit;         ///< 限价
		double		_price;         ///< 委托时的市场价格
		uint32_t	_state;         ///< 订单状态（0-未激活，1-活跃，9-要撤单，99-已撤单）
		uint64_t	_time;          ///< 订单时间
		double		_queue;         ///< 排队位置
		bool		_positive;      ///< 是否为主动委托（对手价）

		/**
		 * @brief 构造函数
		 * @details 初始化订单信息，将所有字段设置为0
		 */
		_OrderInfo()
		{
			memset(this, 0, sizeof(_OrderInfo));
		}
	} OrderInfo;

	typedef wt_hashmap<uint32_t, OrderInfo> Orders;
	Orders	_orders;

	typedef std::map<uint32_t, double>	LOBItems;

	/**
	 * @brief 限价委托账本结构体
	 * @details 存储市场深度数据，包括各价位的委托量、当前价格、买一价和卖一价
	 */
	typedef struct _LmtOrdBook
	{
		LOBItems	_items;        ///< 各价位的委托量映射表
		uint32_t	_cur_px;       ///< 当前价格（乘以10000后的整数表示）
		uint32_t	_ask_px;       ///< 卖一价（乘以10000后的整数表示）
		uint32_t	_bid_px;       ///< 买一价（乘以10000后的整数表示）

		/**
		 * @brief 清空委托账本
		 * @details 清除委托账本中的所有数据，包括委托量和价格信息
		 */
		void clear()
		{
			_items.clear();
			_cur_px = 0;
			_ask_px = 0;
			_bid_px = 0;
		}

		/**
		 * @brief 构造函数
		 * @details 初始化委托账本，将价格字段设置为0
		 */
		_LmtOrdBook()
		{
			_cur_px = 0;
			_ask_px = 0;
			_bid_px = 0;
		}
	} LmtOrdBook;
	typedef wt_hashmap<std::string, LmtOrdBook> LmtOrdBooks;
	LmtOrdBooks	_lmt_ord_books;

	IMatchSink*	_sink;          ///< 撮合引擎回调接口指针

	double			_cancelrate;     ///< 撤单率，用于模拟市场中的撤单行为
	WTSTickCache*	_tick_cache;    ///< Tick数据缓存指针
};

