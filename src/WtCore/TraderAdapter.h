/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易适配器头文件
 * \details 定义了交易适配器类，用于封装交易接口，实现交易指令的发送和管理
 */
#pragma once

#include "../Includes/ExecuteDefs.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/BoostFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/SpinMutex.hpp"

NS_WTP_BEGIN
class WTSVariant;
class ActionPolicyMgr;
class WTSContractInfo;
class WTSCommodityInfo;
class WtLocalExecuter;
class EventNotifier;

class ITrdNotifySink;

/*!
 * \brief 枚举通道持仓回调函数类型
 * \details 用于枚举交易通道的持仓信息的回调函数类型
 * \param stdCode 标准化合约代码
 * \param isLong 是否为多头持仓
 * \param preTotalPos 昨日总持仓
 * \param preAvailPos 昨日可用持仓
 * \param newTotalPos 今日总持仓
 * \param newAvailPos 今日可用持仓
 */
typedef std::function<void(const char*, bool, double, double, double, double)> FuncEnumChnlPosCallBack;

/*!
 * \brief 交易适配器类
 * \details 实现了ITraderSpi接口，用于封装交易接口，处理交易指令的发送、撤单、查询等操作
 * 并提供了风控功能、持仓管理、订单管理等功能
 */
class TraderAdapter : public ITraderSpi
{
public:
	/*!
	 * \brief 构造函数
	 * \param caster 事件通知器指针，默认为NULL
	 */
	TraderAdapter(EventNotifier* caster = NULL);

	/*!
	 * \brief 析构函数
	 */
	~TraderAdapter();

		/*!
	 * \brief 适配器状态枚举
	 * \details 定义了交易适配器的各种状态，从未登录到全部就绪
	 */
	typedef enum tagAdapterState
	{
		AS_NOTLOGIN,		//未登录
		AS_LOGINING,		//正在登录
		AS_LOGINED,			//已登录
		AS_LOGINFAILED,		//登录失败
		AS_POSITION_QRYED,	//仓位已查
		AS_ORDERS_QRYED,	//订单已查
		AS_TRADES_QRYED,	//成交已查
		AS_ALLREADY			//全部就绪
	} AdapterState;

		/*!
	 * \brief 持仓项结构体
	 * \details 记录单个持仓项的详细信息，包括多空两个方向的今仓和昨仓数据
	 */
	typedef struct _PosItem
	{
		//多仓数据
		double	l_newvol;    ///< 多头今仓总量
		double	l_newavail;  ///< 多头今仓可用量
		double	l_prevol;    ///< 多头昨仓总量
		double	l_preavail;  ///< 多头昨仓可用量

		//空仓数据
		double	s_newvol;    ///< 空头今仓总量
		double	s_newavail;  ///< 空头今仓可用量
		double	s_prevol;    ///< 空头昨仓总量
		double	s_preavail;  ///< 空头昨仓可用量

		/*!
		 * \brief 构造函数
		 * \details 初始化所有持仓数据为0
		 */
		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}

		/*!
		 * \brief 获取总持仓量
		 * \param isLong 是否为多头持仓
		 * \return 总持仓量（今仓+昨仓）
		 */
		double total_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newvol + l_prevol;
			else
				return s_newvol + s_prevol;
		}

		/*!
		 * \brief 获取可用持仓量
		 * \param isLong 是否为多头持仓
		 * \return 可用持仓量（今仓可用+昨仓可用）
		 */
		double avail_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newavail + l_preavail;
			else
				return s_newavail + s_preavail;
		}

	} PosItem;

		/*!
	 * \brief 风控参数结构体
	 * \details 定义了交易风控的各项参数，包括下单和撤单的频率限制和总量限制
	 */
	typedef struct _RiskParams
	{
		uint32_t	_order_times_boundary;    ///< 下单次数边界（单位时间内的最大下单次数）
		uint32_t	_order_stat_timespan;     ///< 下单统计时间跨度（单位：秒）
		uint32_t	_order_total_limits;      ///< 下单总量限制（最大下单总次数）

		uint32_t	_cancel_times_boundary;   ///< 撤单次数边界（单位时间内的最大撤单次数）
		uint32_t	_cancel_stat_timespan;    ///< 撤单统计时间跨度（单位：秒）
		uint32_t	_cancel_total_limits;     ///< 撤单总量限制（最大撤单总次数）

		/*!
		 * \brief 构造函数
		 * \details 初始化所有风控参数为0
		 */
		_RiskParams()
		{
			memset(this, 0, sizeof(_RiskParams));
		}
	} RiskParams;

public:
	/*!
	 * \brief 初始化交易适配器
	 * \param id 适配器ID
	 * \param params 初始化参数
	 * \param bdMgr 基础数据管理器
	 * \param policyMgr 动作策略管理器
	 * \return 是否初始化成功
	 */
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	/*!
	 * \brief 使用外部API初始化交易适配器
	 * \param id 适配器ID
	 * \param api 外部交易API接口
	 * \param bdMgr 基础数据管理器
	 * \param policyMgr 动作策略管理器
	 * \return 是否初始化成功
	 */
	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	/*!
	 * \brief 释放交易适配器资源
	 */
	void release();

	/*!
	 * \brief 启动交易适配器
	 * \return 是否启动成功
	 */
	bool run();

		/*!
	 * \brief 获取适配器ID
	 * \return 适配器ID字符串
	 */
	inline const char* id() const{ return _id.c_str(); }

	/*!
	 * \brief 获取适配器状态
	 * \return 适配器状态枚举值
	 */
	AdapterState state() const{ return _state; }

		/*!
	 * \brief 添加交易通知接收器
	 * \param sink 交易通知接收器指针
	 */
	void addSink(ITrdNotifySink* sink)
	{
		_sinks.insert(sink);
	}

		/*!
	 * \brief 检查适配器是否已就绪
	 * \return 是否已就绪
	 */
	inline bool isReady() const { return _state == AS_ALLREADY; }

		/*!
	 * \brief 查询资金信息
	 */
	void queryFund();

private:
	uint32_t doEntrust(WTSEntrust* entrust);
	bool	doCancel(WTSOrderInfo* ordInfo);

	inline void	printPosition(const char* stdCode, const PosItem& pItem);

	inline WTSContractInfo* getContract(const char* stdCode);
	inline WTSCommodityInfo* getCommodify(const char* stdCommID);

	const RiskParams* getRiskParams(const char* stdCode);

	void initSaveData();

	inline void	logTrade(uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);
	inline void	logOrder(uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);

	void	saveData(WTSArray* ayFunds = NULL);

	inline void updateUndone(const char* stdCode, double qty, bool bOuput = true);

public:
		/*!
	 * \brief 获取持仓量
	 * \param stdCode 标准化合约代码
	 * \param bValidOnly 是否只获取有效持仓
	 * \param flag 持仓标志，默认为3（多空均获取）
	 * \return 持仓量
	 */
	double getPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3);

	/*!
	 * \brief 获取指定合约的订单集合
	 * \param stdCode 标准化合约代码
	 * \return 订单集合指针
	 */
	OrderMap* getOrders(const char* stdCode);
		/*!
	 * \brief 获取未完成数量
	 * \param stdCode 标准化合约代码
	 * \return 未完成数量
	 */
	double getUndoneQty(const char* stdCode)
	{
		auto it = _undone_qty.find(stdCode);
		if (it != _undone_qty.end())
			return it->second;

		return 0;
	}

		/*!
	 * \brief 枚举所有持仓
	 * \param cb 回调函数
	 * \details 遍历所有持仓信息，并通过回调函数返回每个持仓项的详细信息
	 */
	void enumPosition(FuncEnumChnlPosCallBack cb);

		/*!
	 * \brief 开多仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t openLong(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 开空仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t openShort(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 平多仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param isToday 是否平今仓
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t closeLong(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 平空仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param isToday 是否平今仓
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);
	
		/*!
	 * \brief 买入操作
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param bForceClose 是否强制平仓
	 * \param cInfo 合约信息，默认为NULL
	 * \return 订单ID集合
	 */
	OrderIDs buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 卖出操作
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param bForceClose 是否强制平仓
	 * \param cInfo 合约信息，默认为NULL
	 * \return 订单ID集合
	 */
	OrderIDs sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 撤销指定订单
	 * \param localid 本地订单ID
	 * \return 是否撤单成功
	 */
	bool	cancel(uint32_t localid);

	/*!
	 * \brief 撤销指定合约的订单
	 * \param stdCode 标准化合约代码
	 * \param isBuy 是否为买入订单
	 * \param qty 撤单数量，默认为0（全部撤销）
	 * \return 撤销的订单ID集合
	 */
	OrderIDs cancel(const char* stdCode, bool isBuy, double qty = 0);

		/*!
	 * \brief 检查是否允许交易
	 * \param stdCode 标准化合约代码
	 * \return 是否允许交易
	 */
	inline bool	isTradeEnabled(const char* stdCode) const;

		/*!
	 * \brief 检查撤单限制
	 * \param stdCode 标准化合约代码
	 * \return 是否允许撤单
	 */
	bool	checkCancelLimits(const char* stdCode);

	/*!
	 * \brief 检查下单限制
	 * \param stdCode 标准化合约代码
	 * \return 是否允许下单
	 */
	bool	checkOrderLimits(const char* stdCode);

		/*!
	 * \brief 检查自成交
	 * \param stdCode 标准化合约代码
	 * \param tInfo 成交信息
	 * \return 是否存在自成交
	 */
	bool	checkSelfMatch(const char* stdCode, WTSTradeInfo* tInfo);

		/*!
	 * \brief 检查是否自成交
	 * \param stdCode 标准化合约代码
	 * \return 是否自成交
	 */
	inline	bool isSelfMatched(const char* stdCode)
	{
		//如果忽略自成交，则直接返回false
		if (_ignore_sefmatch)
			return false;

		auto it = _self_matches.find(stdCode);
		return it != _self_matches.end();
	}

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderSpi接口
	/*!
	 * \brief 处理交易事件
	 * \param e 交易事件类型
	 * \param ec 事件代码
	 */
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	/*!
	 * \brief 登录结果回调
	 * \param bSucc 是否登录成功
	 * \param msg 登录结果消息
	 * \param tradingdate 交易日期
	 */
	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	/*!
	 * \brief 登出回调
	 */
	virtual void onLogout() override;

	/*!
	 * \brief 委托响应回调
	 * \param entrust 委托信息
	 * \param err 错误信息
	 */
	virtual void onRspEntrust(WTSEntrust* entrust, WTSError *err) override;

	/*!
	 * \brief 账户资金响应回调
	 * \param ayAccounts 账户资金信息数组
	 */
	virtual void onRspAccount(WTSArray* ayAccounts) override;

	/*!
	 * \brief 持仓响应回调
	 * \param ayPositions 持仓信息数组
	 */
	virtual void onRspPosition(const WTSArray* ayPositions) override;

	/*!
	 * \brief 订单响应回调
	 * \param ayOrders 订单信息数组
	 */
	virtual void onRspOrders(const WTSArray* ayOrders) override;

	/*!
	 * \brief 成交响应回调
	 * \param ayTrades 成交信息数组
	 */
	virtual void onRspTrades(const WTSArray* ayTrades) override;

	/*!
	 * \brief 订单推送回调
	 * \param orderInfo 订单信息
	 */
	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	/*!
	 * \brief 成交推送回调
	 * \param tradeRecord 成交记录
	 */
	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	/*!
	 * \brief 交易错误回调
	 * \param err 错误信息
	 * \param pData 附加数据，默认为NULL
	 */
	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	/*!
	 * \brief 获取基础数据管理器
	 * \return 基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override;

	/*!
	 * \brief 处理交易日志
	 * \param ll 日志级别
	 * \param message 日志消息
	 */
	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	WTSVariant*			_cfg;				///< 配置项
	std::string			_id;					///< 适配器ID
	std::string			_order_pattern;		///< 订单模式

		uint32_t			_trading_day;			///< 交易日

	ITraderApi*			_trader_api;			///< 交易API接口
	FuncDeleteTrader	_remover;				///< 交易实例删除器
	AdapterState		_state;				///< 适配器状态

		EventNotifier*		_notifier;			///< 事件通知器

	wt_hashset<ITrdNotifySink*>	_sinks;	///< 交易通知接收器集合

	IBaseDataMgr*		_bd_mgr;				///< 基础数据管理器
	ActionPolicyMgr*	_policy_mgr;			///< 动作策略管理器

		wt_hashmap<std::string, PosItem> _positions;	///< 持仓集合

	SpinMutex	_mtx_orders;					///< 订单互斥锁
	OrderMap*	_orders;						///< 订单集合
	wt_hashset<std::string> _orderids;			///< 主要用于标记有没有处理过该订单

		wt_hashmap<std::string, std::string>		_trade_refs;		///< 用于记录成交单和订单的匹配
	wt_hashset<std::string>					_self_matches;	///< 自成交的合约

	/*
	 *	By Wesley @ 2023.03.16
	 *	加一个控制，这样自成交发生以后，还可以恢复交易
	 */
		bool			_ignore_sefmatch;		///< 忽略自成交限制

		wt_hashmap<std::string, double> _undone_qty;	///< 未完成数量

		typedef WTSHashMap<std::string>	TradeStatMap;
	TradeStatMap*	_stat_map;	///< 统计数据

		//这两个缓存时间内的容器,主要是为了控制电光流量而设置的
	typedef std::vector<uint64_t> TimeCacheList;
	typedef wt_hashmap<std::string, TimeCacheList> CodeTimeCacheMap;
	CodeTimeCacheMap	_order_time_cache;	///< 下单时间缓存
	CodeTimeCacheMap	_cancel_time_cache;	///< 撤单时间缓存

		//如果被风控了,就会进入到排除队列
	wt_hashset<std::string>	_exclude_codes;	///< 被风控排除的合约代码集合
	
		typedef wt_hashmap<std::string, RiskParams>	RiskParamsMap;
	RiskParamsMap	_risk_params_map;		///< 风控参数映射表
	bool			_risk_mon_enabled;			///< 是否启用风控监控

		bool			_save_data;	///< 是否保存交易日志
	BoostFilePtr	_trades_log;		///< 交易数据日志
	BoostFilePtr	_orders_log;		///< 订单数据日志
	std::string		_rt_data_file;		///< 实时数据文件
};

/*!
 * \brief 交易适配器智能指针类型
 */
typedef std::shared_ptr<TraderAdapter>				TraderAdapterPtr;

/*!
 * \brief 交易适配器映射表类型
 */
typedef wt_hashmap<std::string, TraderAdapterPtr>	TraderAdapterMap;


//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
/*!
 * \brief 交易适配器管理器
 * \details 管理多个交易适配器实例，提供适配器的添加、获取等功能
 */
class TraderAdapterMgr : private boost::noncopyable
{
public:
	/*!
	 * \brief 释放所有适配器资源
	 */
	void	release();

	/*!
	 * \brief 运行所有适配器
	 */
	void	run();

	/*!
	 * \brief 获取所有适配器
	 * \return 适配器映射表引用
	 */
	const TraderAdapterMap& getAdapters() const { return _adapters; }

	/*!
	 * \brief 获取指定适配器
	 * \param tname 适配器名称
	 * \return 适配器指针
	 */
	TraderAdapterPtr getAdapter(const char* tname);

	/*!
	 * \brief 添加交易适配器
	 * \param tname 适配器名称
	 * \param adapter 适配器指针
	 * \return 是否添加成功
	 */
	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

	/*!
	 * \brief 刷新资金信息
	 */
	void	refresh_funds();

private:
	TraderAdapterMap	_adapters;			///< 适配器映射表
};

NS_WTP_END
