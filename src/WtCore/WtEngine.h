/*!
 * \file WtEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader核心交易引擎类头文件。
 * \details 定义了WtEngine类及其相关成员，是WonderTrader框架的核心组件，负责管理策略、行情、订单、资金、风控等核心交易逻辑。
 * 本文件为新手提供了清晰的接口和结构说明，便于快速理解和上手。
 */

#pragma once
#include <queue>
#include <functional>
#include <stdint.h>

#include "ParserAdapter.h"
#include "WtFilterMgr.h"


#include "../Includes/FasterDefs.h"
#include "../Includes/RiskMonDefs.h"

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Share/BoostFile.hpp"
#include "../Share/SpinMutex.hpp"


NS_WTP_BEGIN
/**
 * @class WTSSessionInfo
 * @brief 会话信息类
 */
class WTSSessionInfo;
/**
 * @class WTSCommodityInfo
 * @brief 品种信息类
 */
class WTSCommodityInfo;
/**
 * @class WTSContractInfo
 * @brief 合约信息类
 */
class WTSContractInfo;

/**
 * @class IBaseDataMgr
 * @brief 基础数据管理器接口
 */
class IBaseDataMgr;
/**
 * @class IHotMgr
 * @brief 主力管理器接口
 */
class IHotMgr;

/**
 * @class WTSVariant
 * @brief 变量类
 */
class WTSVariant;

/**
 * @class WTSTickData
 * @brief Tick数据类
 */
class WTSTickData;
/**
 * @struct WTSBarStruct
 * @brief K线数据结构体
 */
struct WTSBarStruct;
/**
 * @class WTSTickSlice
 * @brief Tick数据切片类
 */
class WTSTickSlice;
/**
 * @class WTSKlineSlice
 * @brief K线数据切片类
 */
class WTSKlineSlice;
/**
 * @class WTSPortFundInfo
 * @brief 组合资金信息类
 */
class WTSPortFundInfo;

/**
 * @class WtDtMgr
 * @brief 数据管理器类
 */
class WtDtMgr;
/**
 * @class TraderAdapterMgr
 * @brief 适配器管理器类
 */
class TraderAdapterMgr;

/**
 * @class EventNotifier
 * @brief 事件通知器类
 */
class EventNotifier;

/**
 * @typedef TaskItem
 * @brief 任务项类型定义
 */
typedef std::function<void()>	TaskItem;

/**
 * @class WtRiskMonWrapper
 * @brief 风控监控器包装类
 */
class WtRiskMonWrapper
{
public:
    /**
     * @brief 构造函数
     * @param mon 风控监控器指针
     * @param fact 风控监控器工厂指针
     */
    WtRiskMonWrapper(WtRiskMonitor* mon, IRiskMonitorFact* fact) :_mon(mon), _fact(fact){}
    /**
     * @brief 析构函数
     */
    ~WtRiskMonWrapper()
    {
        if (_mon)
        {
            _fact->deleteRiskMonotor(_mon);
        }
    }

    /**
     * @brief 获取风控监控器指针
     * @return 风控监控器指针
     */
    WtRiskMonitor* self(){ return _mon; }


private:
	WtRiskMonitor*		_mon;
	IRiskMonitorFact*	_fact;
};
typedef std::shared_ptr<WtRiskMonWrapper>	WtRiskMonPtr;

class IEngineEvtListener
{
public:
	virtual void on_initialize_event() {}
	virtual void on_schedule_event(uint32_t uDate, uint32_t uTime) {}
	virtual void on_session_event(uint32_t uDate, bool isBegin = true) {}
};

class WtEngine : public WtPortContext, public IParserStub
{
public:
	WtEngine();

	inline void set_adapter_mgr(TraderAdapterMgr* mgr) { _adapter_mgr = mgr; }

	void set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs = 0, uint32_t rawTime = 0);

	void set_trading_date(uint32_t curTDate);

	inline uint32_t get_date() { return _cur_date; }
	inline uint32_t get_min_time() { return _cur_time; }
	inline uint32_t get_raw_time() { return _cur_raw_time; }
	inline uint32_t get_secs() { return _cur_secs; }
	inline uint32_t get_trading_date() { return _cur_tdate; }

	inline IBaseDataMgr*		get_basedata_mgr(){ return _base_data_mgr; }
	inline IHotMgr*				get_hot_mgr() { return _hot_mgr; }
	WTSSessionInfo*		get_session_info(const char* sid, bool isCode = false);
	WTSCommodityInfo*	get_commodity_info(const char* stdCode);
	WTSContractInfo*	get_contract_info(const char* stdCode);
	std::string			get_rawcode(const char* stdCode);

	WTSTickData*	get_last_tick(uint32_t sid, const char* stdCode);
	WTSTickSlice*	get_tick_slice(uint32_t sid, const char* stdCode, uint32_t count);
	WTSKlineSlice*	get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times = 1, uint64_t etime = 0);

	void sub_tick(uint32_t sid, const char* code);

	double get_cur_price(const char* stdCode);

	double get_day_price(const char* stdCode, int flag = 0);

	/*
	 *	获取复权因子
	 *	@stdCode	合约代码
	 *	@commInfo	品种信息
	 */
	double get_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo = NULL);

	uint32_t get_adjusting_flag();

	double calc_fee(const char* stdCode, double price, double qty, uint32_t offset);

	inline void setRiskMonitor(WtRiskMonPtr& monitor)
	{
		_risk_mon = monitor;
	}

	inline void regEventListener(IEngineEvtListener* listener)
	{
		_evt_listener = listener;
	}

	//////////////////////////////////////////////////////////////////////////
	//WtPortContext接口
	virtual WTSPortFundInfo* getFundInfo() override;

	virtual void setVolScale(double scale) override;

	virtual bool isInTrading() override;

	virtual void writeRiskLog(const char* message) override;

	virtual uint32_t	getCurDate() override;
	virtual uint32_t	getCurTime() override;
	virtual uint32_t	getTradingDate() override;
	virtual uint32_t	transTimeToMin(uint32_t uTime) override{ return 0; }

	//////////////////////////////////////////////////////////////////////////
	/// IParserStub接口
	virtual void handle_push_quote(WTSTickData* newTick) override;

public:
	virtual void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier);

	virtual void run() = 0;

	virtual void on_tick(const char* stdCode, WTSTickData* curTick);

	virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) = 0;

	virtual void on_init(){}
	virtual void on_session_begin();
	virtual void on_session_end();


protected:
	void		load_fees(const char* filename);

	void		load_datas();

	void		save_datas();

	void		append_signal(const char* stdCode, double qty, bool bStandBy);

	void		do_set_position(const char* stdCode, double qty, double curPx = -1);

	void		task_loop();

	void		push_task(TaskItem task);

	void		update_fund_dynprofit();

	bool		init_riskmon(WTSVariant* cfg);

private:
	void		init_outputs();
	inline void	log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee = 0.0);
	inline void	log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
		double profit, double totalprofit = 0);

protected:
	uint32_t		_cur_date;		//当前日期
	uint32_t		_cur_time;		//当前时间, 是1分钟线时间, 比如0900, 这个时候的1分钟线是0901, _cur_time也就是0901, 这个是为了CTA里面方便
	uint32_t		_cur_raw_time;	//当前真实时间
	uint32_t		_cur_secs;		//当前秒数, 包含毫秒
	uint32_t		_cur_tdate;		//当前交易日

	uint32_t		_fund_udt_span;	//组合资金更新时间间隔

	IBaseDataMgr*	_base_data_mgr;	//基础数据管理器
	IHotMgr*		_hot_mgr;		//主力管理器
	WtDtMgr*		_data_mgr;		//数据管理器
	IEngineEvtListener*	_evt_listener;

	//By Wesley @ 2022.02.07
	//tick数据订阅项，first是contextid，second是订阅选项，0-原始订阅，1-前复权，2-后复权
	typedef std::pair<uint32_t, uint32_t> SubOpt;
	typedef wt_hashmap<uint32_t, SubOpt> SubList;
	typedef wt_hashmap<std::string, SubList>	StraSubMap;
	StraSubMap		_tick_sub_map;	//tick数据订阅表
	StraSubMap		_bar_sub_map;	//K线数据订阅表

	//By Wesley @ 2022.02.07 
	//这个好像没有用到，不需要了
	//wt_hashset<std::string>		_ticksubed_raw_codes;	//tick订阅表（真实代码模式）
	

	//////////////////////////////////////////////////////////////////////////
	//
	typedef struct _SigInfo
	{
		double		_volume;
		uint64_t	_gentime;

		_SigInfo()
		{
			_volume = 0;
			_gentime = 0;
		}
	}SigInfo;
	typedef wt_hashmap<std::string, SigInfo>	SignalMap;
	SignalMap		_sig_map;

	//////////////////////////////////////////////////////////////////////////
	//信号过滤器
	WtFilterMgr		_filter_mgr;
	EventNotifier*	_notifier;

	//////////////////////////////////////////////////////////////////////////
	//手续费模板
	typedef struct _FeeItem
	{
		double	_open;
		double	_close;
		double	_close_today;
		bool	_by_volume;

		_FeeItem()
		{
			memset(this, 0, sizeof(_FeeItem));
		}
	} FeeItem;
	typedef wt_hashmap<std::string, FeeItem>	FeeMap;
	FeeMap		_fee_map;
	

	    /**
     * @brief 组合资金信息指针
     * @details 用于记录当前交易组合的资金状况，包括动态权益、可用资金等
     */
    WTSPortFundInfo*	_port_fund;

	//////////////////////////////////////////////////////////////////////////
	/**
     * @brief 持仓明细信息结构体
     * @details 记录每一笔持仓的详细信息，如方向、价格、数量、开仓时间等
     */
	typedef struct _DetailInfo
	{
		bool		_long;      ///< 持仓方向，true为多头，false为空头
		double		_price;     ///< 开仓价格
		double		_volume;    ///< 持仓数量
		uint64_t	_opentime;  ///< 开仓时间戳
		uint32_t	_opentdate; ///< 开仓日期
		double		_profit;    ///< 当前持仓的浮动盈亏

		/**
         * @brief 构造函数
         * @details 初始化所有成员变量为0
         */
		_DetailInfo()
		{
			memset(this, 0, sizeof(_DetailInfo));
		}
	} DetailInfo;

	/**
     * @brief 持仓信息结构体
     * @details 记录某一合约的总持仓、已实现盈亏、浮动盈亏及持仓明细
     */
	typedef struct _PosInfo
	{
		double		_volume;        ///< 当前总持仓数量
		double		_closeprofit;   ///< 已实现平仓盈亏
		double		_dynprofit;     ///< 当前浮动盈亏
		SpinMutex	_mtx;           ///< 持仓互斥锁，用于多线程保护

		std::vector<DetailInfo> _details; ///< 持仓明细列表

		/**
         * @brief 构造函数
         * @details 初始化持仓数量、已实现盈亏和浮动盈亏为0
         */
		_PosInfo()
		{
			_volume = 0;
			_closeprofit = 0;
			_dynprofit = 0;
		}
	} PosInfo;
	typedef std::shared_ptr<PosInfo> PosInfoPtr;  ///< 持仓信息智能指针
	typedef wt_hashmap<std::string, PosInfoPtr> PositionMap;  ///< 合约持仓映射表
	PositionMap		_pos_map;  ///< 当前所有合约的持仓映射表

	//////////////////////////////////////////////////////////////////////////
	/**
     * @brief 合约价格映射表类型
     * @details 用于存储各合约的最新价格
     */
	typedef wt_hashmap<std::string, double> PriceMap;
	PriceMap		_price_map;  ///< 当前合约价格映射表

	/**
     * @brief 后台任务线程相关成员
     * @details 用于异步处理风控、资金、持仓等更新任务
     */
	typedef std::queue<TaskItem>	TaskQueue;  ///< 任务队列类型
	StdThreadPtr	_thrd_task;    ///< 后台任务线程指针
	TaskQueue		_task_queue;   ///< 任务队列
	StdUniqueMutex	_mtx_task;     ///< 任务队列互斥锁
	StdCondVariable	_cond_task;    ///< 任务队列条件变量
	bool			_terminated;   ///< 任务线程终止标志

	/**
     * @brief 风控工厂信息结构体
     * @details 记录风控动态库、工厂对象及其创建/销毁方法
     */
	typedef struct _RiskMonFactInfo
	{
		std::string		_module_path;  ///< 风控动态库路径
		DllHandle		_module_inst;  ///< 动态库实例句柄
		IRiskMonitorFact*	_fact;  ///< 风控工厂对象指针
		FuncCreateRiskMonFact	_creator;  ///< 创建工厂方法指针
		FuncDeleteRiskMonFact	_remover;  ///< 销毁工厂方法指针
	} RiskMonFactInfo;
	RiskMonFactInfo	_risk_fact;  ///< 当前风控工厂信息
	WtRiskMonPtr	_risk_mon;  ///< 风控实例智能指针
	double			_risk_volscale;  ///< 风控用合约乘数
	uint32_t		_risk_date;  ///< 风控日期

	/**
     * @brief 交易适配器管理器指针
     * @details 用于管理不同交易接口的适配器
     */
	TraderAdapterMgr*	_adapter_mgr;

	/**
     * @brief 交易日志文件指针
     * @details 用于记录交易执行详情
     */
	BoostFilePtr	_trade_logs;
	/**
     * @brief 平仓日志文件指针
     * @details 用于记录平仓执行详情
     */
	BoostFilePtr	_close_logs;

	/**
     * @brief 复权因子缓存表
     * @details 缓存各合约的复权因子，提高计算效率
     */
	wt_hashmap<std::string, double>	_factors_cache;

	/**
     * @brief 系统就绪标志
     * @details 标记引擎是否已完成初始化，可以推送tick数据
     */
	bool			_ready;
};
NS_WTP_END