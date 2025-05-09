/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易适配器模块，用于连接交易API和策略引擎
 * \details 该模块实现了交易接口的统一封装，处理订单管理、持仓查询、风控规则等功能，
 *          并通过回调机制向上层通知交易事件。同时提供了TraderAdapterMgr用于管理多个交易通道。
 */
#pragma once

#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/BoostFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSCollection.hpp"
#include "../Share/SpinMutex.hpp"

NS_WTP_BEGIN
class WTSVariant;
class WTSContractInfo;
class WTSCommodityInfo;
class ITrdNotifySink;
class ActionPolicyMgr;

/**
 * @brief 订单ID集合类型定义
 */
typedef std::vector<uint32_t> OrderIDs;

/**
 * @brief 订单映射类型定义
 */
typedef WTSMap<uint32_t> OrderMap;

/**
 * @brief 交易适配器类
 * @details 该类继承自ITraderSpi接口，用于统一封装不同交易API的接口，
 *          提供订单管理、持仓管理、风控监控等功能，并支持多账户交易
 */
class TraderAdapter : public ITraderSpi
{
public:
	TraderAdapter();
	~TraderAdapter();

	/**
	 * @brief 交易适配器状态枚举
	 * @details 用于标识交易适配器当前的工作状态
	 */
	typedef enum tagAdapterState
	{
		AS_NOTLOGIN,		///<未登录
		AS_LOGINING,		///<正在登录
		AS_LOGINED,			///<已登录
		AS_LOGINFAILED,		///<登录失败
		AS_POSITION_QRYED,	///<仓位已查询
		AS_ORDERS_QRYED,	///<订单已查询
		AS_TRADES_QRYED,	///<成交已查询
		AS_ALLREADY			///<全部就绪
	} AdapterState;

	/**
	 * @brief 持仓项结构体
	 * @details 记录合约的多空两个方向的持仓数据，包括今仓和昨仓的数量和可用数量
	 */
	typedef struct _PosItem
	{
		//多仓数据
		double	l_newvol;		///<多头今仓数量
		double	l_newavail;		///<多头今仓可用数量
		double	l_prevol;		///<多头昨仓数量
		double	l_preavail;		///<多头昨仓可用数量

		//空仓数据
		double	s_newvol;		///<空头今仓数量
		double	s_newavail;		///<空头今仓可用数量
		double	s_prevol;		///<空头昨仓数量
		double	s_preavail;		///<空头昨仓可用数量

		/**
		 * @brief 构造函数
		 * @details 初始化所有成员变量为0
		 */
		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}

		/**
		 * @brief 获取总持仓量
		 * @param isLong 是否为多头持仓，默认为true
		 * @return 返回对应方向的总持仓量(今仓+昨仓)
		 */
		double total_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newvol + l_prevol;
			else
				return s_newvol + s_prevol;
		}

		/**
		 * @brief 获取可用持仓量
		 * @param isLong 是否为多头持仓，默认为true
		 * @return 返回对应方向的可用持仓量(今仓可用+昨仓可用)
		 */
		double avail_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newavail + l_preavail;
			else
				return s_newavail + s_preavail;
		}

	} PosItem;

	/**
	 * @brief 风控参数结构体
	 * @details 定义交易风控的相关参数，包括下单和撤单的频率限制和总量限制
	 */
	typedef struct _RiskParams
	{
		uint32_t	_order_times_boundary;		///<下单次数边界值，指定时间段内下单次数超过此值将触发风控
		uint32_t	_order_stat_timespan;		///<下单统计时间跨度，单位为秒
		uint32_t	_order_total_limits;		///<下单总量限制

		uint32_t	_cancel_times_boundary;		///<撤单次数边界值，指定时间段内撤单次数超过此值将触发风控
		uint32_t	_cancel_stat_timespan;		///<撤单统计时间跨度，单位为秒
		uint32_t	_cancel_total_limits;		///<撤单总量限制

		/**
		 * @brief 构造函数
		 * @details 初始化所有成员变量为0
		 */
		_RiskParams()
		{
			memset(this, 0, sizeof(_RiskParams));
		}
	} RiskParams;

public:
	/**
	 * @brief 初始化交易适配器
	 * @param id 交易通道ID
	 * @param params 配置参数
	 * @param bdMgr 基础数据管理器
	 * @param policyMgr 策略管理器
	 * @return 是否初始化成功
	 * @details 通过配置创建并初始化交易适配器，读取风控配置等
	 */
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	/**
	 * @brief 使用外部API初始化交易适配器
	 * @param id 交易通道ID
	 * @param api 外部交易API接口
	 * @param bdMgr 基础数据管理器
	 * @param policyMgr 策略管理器
	 * @return 是否初始化成功
	 * @details 使用已创建的交易API对象初始化交易适配器
	 */
	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	/**
	 * @brief 释放交易适配器资源
	 * @details 清理交易适配器占用的资源
	 */
	void release();

	/**
	 * @brief 启动交易适配器
	 * @return 是否成功启动
	 * @details 启动交易适配器，建立交易连接
	 */
	bool run();

	/**
	 * @brief 获取交易通道ID
	 * @return 交易通道ID
	 */
	inline const char* id() const{ return _id.c_str(); }

	/**
	 * @brief 获取交易适配器当前状态
	 * @return 交易适配器状态
	 */
	AdapterState state() const{ return _state; }

	/**
	 * @brief 添加交易通知接收器
	 * @param sink 交易通知接收器
	 * @details 添加交易事件接收器，用于接收订单、成交等交易事件通知
	 */
	void addSink(ITrdNotifySink* sink)
	{
		_sinks.insert(sink);
	}

private:
	/**
	 * @brief 执行委托下单
	 * @param entrust 委托对象
	 * @return 本地订单ID
	 * @details 发送指定的委托到交易所
	 */
	uint32_t doEntrust(WTSEntrust* entrust);
	
	/**
	 * @brief 执行撤单操作
	 * @param ordInfo 订单信息对象
	 * @return 是否成功发送撤单请求
	 * @details 将指定订单发送撤单请求到交易所
	 */
	bool	doCancel(WTSOrderInfo* ordInfo);

	/**
	 * @brief 打印持仓信息
	 * @param stdCode 标准合约代码
	 * @param pItem 持仓项目
	 * @details 打印输出指定合约的持仓信息
	 */
	inline void	printPosition(const char* stdCode, const PosItem& pItem);

	/**
	 * @brief 获取合约信息
	 * @param stdCode 标准合约代码
	 * @return 合约信息对象
	 * @details 从基础数据管理器中获取指定合约的详细信息
	 */
	inline WTSContractInfo* getContract(const char* stdCode);

	/**
	 * @brief 更新未完成数量
	 * @param stdCode 标准合约代码
	 * @param qty 数量
	 * @details 更新合约的未完成委托数量
	 */
	inline void updateUndone(const char* stdCode, double qty);

	/**
	 * @brief 获取风控参数
	 * @param stdCode 标准合约代码
	 * @return 风控参数对象
	 * @details 根据合约代码获取相应的风控参数配置
	 */
	const RiskParams* getRiskParams(const char* stdCode);

public:
	/**
	 * @brief 获取指定合约的持仓量
	 * @param stdCode 标准合约代码
	 * @param bValidOnly 是否只返回可用仓位
	 * @param flag 持仓方向标志：1-多头，2-空头，3-全部，默认为3
	 * @return 持仓数量
	 * @details 获取指定合约的指定方向的持仓量
	 */
	double	getPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3);

	/**
	 * @brief 枚举所有持仓
	 * @param stdCode 标准合约代码，默认为空字符串，表示枚举所有合约
	 * @return 持仓总量
	 * @details 枚举并打印所有持仓信息，如果指定了合约代码，则只枚举该合约的持仓
	 */
	double	enumPosition(const char* stdCode = "");

	/**
	 * @brief 获取订单映射
	 * @param stdCode 标准合约代码
	 * @return 订单映射对象
	 * @details 获取指定合约的订单映射，如果合约代码为空，则返回所有订单
	 */
	OrderMap* getOrders(const char* stdCode);

	/**
	 * @brief 获取未完成委托数量
	 * @param stdCode 标准合约代码
	 * @return 未完成委托数量
	 * @details 获取指定合约的未完成委托数量
	 */
	inline double getUndoneQty(const char* stdCode)
	{
		auto it = _undone_qty.find(stdCode);
		if (it != _undone_qty.end())
			return it->second;

		return 0;
	}

	/**
	 * @brief 获取合约信息
	 * @param stdCode 标准合约代码
	 * @return 本地单据ID
	 * @details 获取指定合约的信息
	 */
	uint32_t getInfos(const char* stdCode);

	/**
	 * @brief 买入操作
	 * @param stdCode 标准合约代码
	 * @param price 下单价格
	 * @param qty 下单数量
	 * @param flag 下单标志：0-普通单，1-FAK，2-FOK
	 * @param bForceClose 是否强制平仓
	 * @param cInfo 合约信息对象，可为空
	 * @return 订单ID列表
	 * @details 发送买入委托，可能包含多个订单
	 */
	OrderIDs buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/**
	 * @brief 卖出操作
	 * @param stdCode 标准合约代码
	 * @param price 下单价格
	 * @param qty 下单数量
	 * @param flag 下单标志：0-普通单，1-FAK，2-FOK
	 * @param bForceClose 是否强制平仓
	 * @param cInfo 合约信息对象，可为空
	 * @return 订单ID列表
	 * @details 发送卖出委托，可能包含多个订单
	 */
	OrderIDs sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/**
	 * @brief 开多仓下单接口
	 * 
	 * @param stdCode 标准合约代码
	 * @param price 下单价格，0则表示市价单
	 * @param qty 下单数量
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（充足则成单），2-fok（全部成交或撤销）
	 * @return 本地订单ID
	 * @details 发送开多仓委托，即买入并开仓
	 */
	uint32_t openLong(const char* stdCode, double price, double qty, int flag);

	/**
	 * @brief 开空仓下单接口
	 * 
	 * @param stdCode 标准合约代码
	 * @param price 下单价格，0则表示市价单
	 * @param qty 下单数量
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（充足则成单），2-fok（全部成交或撤销）
	 * @return 本地订单ID
	 * @details 发送开空仓委托，即卖出并开仓
	 */
	uint32_t openShort(const char* stdCode, double price, double qty, int flag);

	/**
	 * @brief 平多仓下单接口
	 * 
	 * @param stdCode 标准合约代码
	 * @param price 下单价格，0则表示市价单
	 * @param qty 下单数量
	 * @param isToday 是否平今仓，默认false（即先平昨仓）
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（充足则成单），2-fok（全部成交或撤销）
	 * @return 本地订单ID
	 * @details 发送平多仓委托，即卖出并平仓
	 */
	uint32_t closeLong(const char* stdCode, double price, double qty, bool isToday, int flag);

	/**
	 * @brief 平空仓下单接口
	 * 
	 * @param stdCode 标准合约代码
	 * @param price 下单价格，0则表示市价单
	 * @param qty 下单数量
	 * @param isToday 是否平今仓，默认false（即先平昨仓）
	 * @param flag 下单标志: 0-normal（普通限价单），1-fak（充足则成单），2-fok（全部成交或撤销）
	 * @return 本地订单ID
	 * @details 发送平空仓委托，即买入并平仓
	 */
	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag);
	
	/**
	 * @brief 撤销指定订单
	 * @param localid 本地订单ID
	 * @return 是否成功发送撤单请求
	 * @details 根据本地订单ID撤销特定订单
	 */
	bool	cancel(uint32_t localid);

	/**
	 * @brief 撤销指定合约的所有订单
	 * @param stdCode 标准合约代码
	 * @return 被撤销的订单ID列表
	 * @details 撤销指定合约的全部未完成委托
	 */
	OrderIDs cancelAll(const char* stdCode);

	/**
	 * @brief 检查合约是否允许交易
	 * @param stdCode 标准合约代码
	 * @return 是否允许交易
	 * @details 检查合约是否在排除列表中，如果在则不允许交易
	 */
	inline bool	isTradeEnabled(const char* stdCode) const;

	/**
	 * @brief 检查撤单频率是否超过限制
	 * @param stdCode 标准合约代码
	 * @return 是否在限制范围内
	 * @details 根据风控参数检查撤单频率是否超过限制
	 */
	bool	checkCancelLimits(const char* stdCode);

	/**
	 * @brief 检查下单频率是否超过限制
	 * @param stdCode 标准合约代码
	 * @return 是否在限制范围内
	 * @details 根据风控参数检查下单频率是否超过限制
	 */
	bool	checkOrderLimits(const char* stdCode);

public:
	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief ITraderSpi接口实现部分
	 */
	//////////////////////////////////////////////////////////////////////////
	
	/**
	 * @brief 处理交易事件
	 * @param e 交易事件类型
	 * @param ec 事件代码
	 * @details 处理交易连接、断开等事件
	 */
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	/**
	 * @brief 登录结果回调
	 * @param bSucc 是否登录成功
	 * @param msg 登录结果消息
	 * @param tradingdate 交易日期
	 * @details 处理交易账户登录结果
	 */
	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	/**
	 * @brief 登出回调
	 * @details 处理交易账户登出事件
	 */
	virtual void onLogout() override;

	/**
	 * @brief 委托响应回调
	 * @param entrust 委托对象
	 * @param err 错误对象，如果为NULL则表示成功
	 * @details 处理委托下单后的响应结果
	 */
	virtual void onRspEntrust(WTSEntrust* entrust, WTSError *err) override;

	/**
	 * @brief 账户资金信息回调
	 * @param ayAccounts 账户资金信息集合
	 * @details 处理账户资金查询结果
	 */
	virtual void onRspAccount(WTSArray* ayAccounts) override;

	/**
	 * @brief 持仓信息回调
	 * @param ayPositions 持仓信息集合
	 * @details 处理持仓查询结果，更新内部持仓管理
	 */
	virtual void onRspPosition(const WTSArray* ayPositions) override;

	/**
	 * @brief 订单信息回调
	 * @param ayOrders 订单信息集合
	 * @details 处理订单查询结果，更新内部订单管理
	 */
	virtual void onRspOrders(const WTSArray* ayOrders) override;

	/**
	 * @brief 成交信息回调
	 * @param ayTrades 成交信息集合
	 * @details 处理成交查询结果
	 */
	virtual void onRspTrades(const WTSArray* ayTrades) override;

	/**
	 * @brief 订单状态推送回调
	 * @param orderInfo 订单信息对象
	 * @details 处理实时订单状态变化推送，更新订单状态并通知上层
	 */
	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	/**
	 * @brief 成交回报推送回调
	 * @param tradeRecord 成交记录对象
	 * @details 处理实时成交回报，更新持仓和订单状态，并通知上层
	 */
	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	/**
	 * @brief 交易错误回调
	 * @param err 错误对象
	 * @param pData 附加数据，默认为NULL
	 * @details 处理交易过程中的错误
	 */
	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	/**
	 * @brief 获取基础数据管理器
	 * @return 基础数据管理器指针
	 * @details 提供给其他模块访问基础数据的接口
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override;

	/**
	 * @brief 处理交易日志
	 * @param ll 日志级别
	 * @param message 日志消息
	 * @details 处理交易API产生的日志信息
	 */
	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	/**
	 * @brief 配置参数
	 * @details 存储交易适配器的配置参数
	 */
	WTSVariant*			_cfg;

	/**
	 * @brief 交易通道ID
	 * @details 标识交易适配器的唯一ID
	 */
	std::string			_id;

	/**
	 * @brief 订单模式
	 * @details 定义订单ID的生成模式
	 */
	std::string			_order_pattern;

	/**
	 * @brief 交易日
	 * @details 当前的交易日期，格式为YYYYMMDD
	 */
	uint32_t			_trading_day;

	/**
	 * @brief 交易API接口
	 * @details 实际的交易API实现对象
	 */
	ITraderApi*			_trader_api;

	/**
	 * @brief 交易API删除函数
	 * @details 用于清理交易API资源的函数指针
	 */
	FuncDeleteTrader	_remover;

	/**
	 * @brief 适配器当前状态
	 * @details 记录交易适配器当前的工作状态
	 */
	AdapterState		_state;

	/**
	 * @brief 交易通知接收器集合
	 * @details 注册的交易事件接收器集合，用于向上层通知交易相关事件
	 */
	wt_hashset<ITrdNotifySink*>	_sinks;

	/**
	 * @brief 基础数据管理器
	 * @details 用于获取合约、交易所等基础数据
	 */
	IBaseDataMgr*		_bd_mgr;

	/**
	 * @brief 行动策略管理器
	 * @details 管理交易行为的策略定义
	 */
	ActionPolicyMgr*	_policy_mgr;

	/**
	 * @brief 持仓集合
	 * @details 以合约代码为键的持仓映射表
	 */
	wt_hashmap<std::string, PosItem> _positions;

	/**
	 * @brief 订单操作互斥锁
	 * @details 用于保护订单集合的线程安全访问
	 */
	SpinMutex	_mtx_orders;

	/**
	 * @brief 订单映射
	 * @details 存储所有活动订单的映射表
	 */
	OrderMap*	_orders;

	/**
	 * @brief 订单ID集合
	 * @details 主要用于标记有没有处理过该订单
	 */
	wt_hashset<std::string> _orderids;

	/**
	 * @brief 未完成委托数量映射
	 * @details 以合约代码为键，记录未完成委托的数量
	 */
	wt_hashmap<std::string, double> _undone_qty;

	/**
	 * @brief 交易统计映射类型定义
	 * @details 用于统计交易数据的映射类型
	 */
	typedef WTSHashMap<std::string>	TradeStatMap;

	/**
	 * @brief 统计数据映射
	 * @details 存储交易统计数据，用于风控监控
	 */
	TradeStatMap*	_stat_map;

	/**
	 * @brief 时间缓存列表类型定义
	 * @details 用于缓存时间点的列表类型
	 */
	typedef std::vector<uint64_t> TimeCacheList;

	/**
	 * @brief 代码时间缓存映射类型定义
	 * @details 用于按合约代码缓存时间点的映射类型，主要用于控制瞬间流量
	 */
	typedef wt_hashmap<std::string, TimeCacheList> CodeTimeCacheMap;

	/**
	 * @brief 下单时间缓存
	 * @details 缓存每个合约的下单时间点，用于频率限制
	 */
	CodeTimeCacheMap	_order_time_cache;

	/**
	 * @brief 撤单时间缓存
	 * @details 缓存每个合约的撤单时间点，用于频率限制
	 */
	CodeTimeCacheMap	_cancel_time_cache;

	/**
	 * @brief 排除代码集合
	 * @details 如果某个合约被风控系统排除，则会进入此队列
	 */
	wt_hashset<std::string>	_exclude_codes;

	/**
	 * @brief 风控参数映射类型定义
	 * @details 以商品代码为键的风控参数映射类型
	 */
	typedef wt_hashmap<std::string, RiskParams>	RiskParamsMap;

	/**
	 * @brief 风控参数映射
	 * @details 存储不同商品的风控参数
	 */
	RiskParamsMap	_risk_params_map;

	/**
	 * @brief 风控监控启用标志
	 * @details 标记是否启用风控监控
	 */
	bool			_risk_mon_enabled;
};

typedef std::shared_ptr<TraderAdapter>					TraderAdapterPtr;
typedef wt_hashmap<std::string, TraderAdapterPtr>	TraderAdapterMap;


//////////////////////////////////////////////////////////////////////////
/**
 * @brief 交易适配器管理器
 * @details 管理多个交易适配器实例，提供添加、获取、启动和释放等操作
 */
class TraderAdapterMgr
{
public:
	/**
	 * @brief 释放所有交易适配器资源
	 * @details 清理并释放所有交易适配器占用的资源
	 */
	void	release();

	/**
	 * @brief 运行所有交易适配器
	 * @details 启动所有交易适配器，建立交易连接
	 */
	void	run();

	/**
	 * @brief 获取所有交易适配器
	 * @return 交易适配器映射对象
	 * @details 返回所有注册的交易适配器集合
	 */
	const TraderAdapterMap& getAdapters() const { return _adapters; }

	/**
	 * @brief 获取指定名称的交易适配器
	 * @param tname 交易适配器名称
	 * @return 交易适配器指针
	 * @details 根据名称从注册表中获取对应的交易适配器
	 */
	TraderAdapterPtr getAdapter(const char* tname);

	/**
	 * @brief 添加交易适配器
	 * @param tname 交易适配器名称
	 * @param adapter 交易适配器指针
	 * @return 是否添加成功
	 * @details 将交易适配器注册到管理器中，如果名称已存在则添加失败
	 */
	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

private:
	/**
	 * @brief 交易适配器集合
	 * @details 存储所有注册的交易适配器，以名称为键
	 */
	TraderAdapterMap	_adapters;
};

NS_WTP_END
