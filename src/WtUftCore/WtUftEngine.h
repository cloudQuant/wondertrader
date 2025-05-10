/*!
 * \file WtUftEngine.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略引擎头文件
 * \details 定义了WtUftEngine类，负责超高频交易(UFT)策略的执行和管理
 */
#pragma once

#include <queue>
#include <functional>
#include <stdint.h>

#include "ParserAdapter.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/RiskMonDefs.h"

#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Share/BoostFile.hpp"

#include "../Includes/IUftStraCtx.h"

NS_WTP_BEGIN
class WTSSessionInfo;
class WTSCommodityInfo;
class WTSContractInfo;

class IBaseDataMgr;
class IHotMgr;

class WTSVariant;

class WTSTickData;
struct WTSBarStruct;
class WTSTickSlice;
class WTSKlineSlice;
class WTSPortFundInfo;

class WtUftDtMgr;
class TraderAdapterMgr;

class EventNotifier;

typedef std::function<void()>	TaskItem;


class WTSVariant;
class WtUftRtTicker;
class EventNotifier;

typedef std::shared_ptr<IUftStraCtx> UftContextPtr;

/**
 * @brief UFT策略引擎类
 * @details 负责超高频交易策略的执行和管理，实现行情数据接收、策略调度和交易发送等核心功能
 * 继承自IParserStub接口，可以接收并处理行情和Level2数据
 */
class WtUftEngine : public IParserStub
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化UFT引擎对象，设置初始时间和基础类成员变量
	 */
	WtUftEngine();

	/**
	 * @brief 析构函数
	 * @details 清理引擎资源，释放内存，关闭时间计时器
	 */
	virtual ~WtUftEngine();

public:
	/**
	 * @brief 设置交易适配器管理器
	 * @param mgr 交易适配器管理器指针
	 */
	inline void set_adapter_mgr(TraderAdapterMgr* mgr) { _adapter_mgr = mgr; }

	/**
	 * @brief 设置当前日期和时间
	 * @param curDate 当前日期，格式YYYYMMDD
	 * @param curTime 当前时间，格式HHMM
	 * @param curSecs 当前秒数，包含毫秒，默认为0
	 * @param rawTime 原始时间，默认为0（使用curTime）
	 */
	void set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs = 0, uint32_t rawTime = 0);

	/**
	 * @brief 设置当前交易日期
	 * @param curTDate 交易日期，格式YYYYMMDD
	 */
	void set_trading_date(uint32_t curTDate);

	/**
	 * @brief 获取当前日期
	 * @return 当前日期，格式YYYYMMDD
	 */
	inline uint32_t get_date() { return _cur_date; }

	/**
	 * @brief 获取当前分钟时间
	 * @return 当前分钟时间，格式HHMM
	 */
	inline uint32_t get_min_time() { return _cur_time; }

	/**
	 * @brief 获取原始时间
	 * @return 原始时间，格式HHMM
	 */
	inline uint32_t get_raw_time() { return _cur_raw_time; }

	/**
	 * @brief 获取当前秒数
	 * @return 当前秒数，包含毫秒
	 */
	inline uint32_t get_secs() { return _cur_secs; }

	/**
	 * @brief 获取当前交易日期
	 * @return 当前交易日期，格式YYYYMMDD
	 */
	inline uint32_t get_trading_date() { return _cur_tdate; }

	/**
	 * @brief 获取基础数据管理器
	 * @return 基础数据管理器指针
	 */
	inline IBaseDataMgr*		get_basedata_mgr() { return _base_data_mgr; }

	/**
	 * @brief 获取交易时段信息
	 * @param sid 交易时段ID或标准化合约代码
	 * @param isCode 是否为合约代码，默认为false
	 * @return 交易时段信息指针
	 */
	WTSSessionInfo*		get_session_info(const char* sid, bool isCode = false);

	/**
	 * @brief 获取品种信息
	 * @param stdCode 标准化合约代码
	 * @return 品种信息指针
	 */
	WTSCommodityInfo*	get_commodity_info(const char* stdCode);

	/**
	 * @brief 获取合约信息
	 * @param stdCode 标准化合约代码
	 * @return 合约信息指针
	 */
	WTSContractInfo*	get_contract_info(const char* stdCode);

	/**
	 * @brief 获取最新的Tick数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @return 最新的Tick数据指针
	 */
	WTSTickData*	get_last_tick(uint32_t sid, const char* stdCode);

	/**
	 * @brief 获取Tick切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @return Tick切片数据指针
	 */
	WTSTickSlice*	get_tick_slice(uint32_t sid, const char* stdCode, uint32_t count);

	/**
	 * @brief 获取K线切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @param period 周期标识（如"m"(分钟)、"d"(日线)）
	 * @param count 请求的数据条数
	 * @param times 周期倍数，默认为1
	 * @param etime 结束时间，默认为0（当前时间）
	 * @return K线切片数据指针
	 */
	WTSKlineSlice*	get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times = 1, uint64_t etime = 0);

	/**
	 * @brief 订阅Tick数据
	 * @param sid 策略ID
	 * @param code 合约代码
	 */
	void sub_tick(uint32_t sid, const char* code);

	/**
	 * @brief 获取当前价格
	 * @param stdCode 标准化合约代码
	 * @return 当前价格，如果无数据则返回0.0
	 */
	double get_cur_price(const char* stdCode);

	/**
	 * @brief 通知参数更新
	 * @param name 策略名称
	 * @details 当策略参数更新时通知对应的策略对象
	 */
	void notify_params_update(const char* name);

public:
	/**
	 * @brief 初始化引擎
	 * @param cfg 配置项变量集
	 * @param bdMgr 基础数据管理器指针
	 * @param dataMgr UFT数据管理器指针
	 * @param notifier 事件通知器指针
	 * @details 初始化引擎对象，设置数据管理器和配置项
	 */
	void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtUftDtMgr* dataMgr, EventNotifier* notifier);

	/**
	 * @brief 启动引擎
	 * @details 初始化各个策略上下文，启动时间计时器
	 */
	void run();

	/**
	 * @brief 处理Tick数据
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前收到的Tick数据指针
	 * @details 接收并处理实时Tick数据，分发给已订阅的策略
	 */
	void on_tick(const char* stdCode, WTSTickData* curTick);

	/**
	 * @brief 处理K线数据
	 * @param stdCode 标准化合约代码
	 * @param period 周期标识
	 * @param times 周期倍数
	 * @param newBar 新的K线数据指针
	 * @details 接收并处理K线数据，分发给已订阅的策略
	 */
	void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar);

	/**
	 * @brief 引擎初始化回调
	 * @details 当引擎初始化时的回调函数，当前为空实现
	 */
	void on_init(){}

	/**
	 * @brief 交易时段开始回调
	 * @details 当交易时段开始时触发，通知各个策略上下文
	 */
	void on_session_begin();

	/**
	 * @brief 交易时段结束回调
	 * @details 当交易时段结束时触发，通知各个策略上下文
	 */
	void on_session_end();

	/**
	 * @brief 处理行情数据推送
	 * @param newTick 新的Tick数据指针
	 * @details 实现IParserStub接口的方法，用于接收并处理实时行情数据
	 */
	virtual void handle_push_quote(WTSTickData* newTick) override;

	/**
	 * @brief 处理委托明细数据推送
	 * @param curOrdDtl 当前委托明细数据指针
	 * @details 实现IParserStub接口的方法，用于接收并处理Level2委托明细数据
	 */
	virtual void handle_push_order_detail(WTSOrdDtlData* curOrdDtl) override;

	/**
	 * @brief 处理委托队列数据推送
	 * @param curOrdQue 当前委托队列数据指针
	 * @details 实现IParserStub接口的方法，用于接收并处理Level2委托队列数据
	 */
	virtual void handle_push_order_queue(WTSOrdQueData* curOrdQue) override;

	/**
	 * @brief 处理成交数据推送
	 * @param curTrans 当前成交数据指针
	 * @details 实现IParserStub接口的方法，用于接收并处理Level2成交数据
	 */
	virtual void handle_push_transaction(WTSTransData* curTrans) override;

public:
	/**
	 * @brief 获取委托队列切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @return 委托队列切片数据指针
	 */
	WTSOrdQueSlice* get_order_queue_slice(uint32_t sid, const char* stdCode, uint32_t count);
	
	/**
	 * @brief 获取委托明细切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @return 委托明细切片数据指针
	 */
	WTSOrdDtlSlice* get_order_detail_slice(uint32_t sid, const char* stdCode, uint32_t count);
	
	/**
	 * @brief 获取成交切片数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @return 成交切片数据指针
	 */
	WTSTransSlice* get_transaction_slice(uint32_t sid, const char* stdCode, uint32_t count);

public:
	/**
	 * @brief 分钟结束回调
	 * @param curDate 当前日期，格式YYYYMMDD
	 * @param curTime 当前时间，格式HHMM
	 * @details 当分钟结束时的回调函数
	 */
	void on_minute_end(uint32_t curDate, uint32_t curTime);

	/**
	 * @brief 添加策略上下文
	 * @param ctx 策略上下文指针
	 * @details 将策略上下文添加到引擎的策略映射中
	 */
	void addContext(UftContextPtr ctx);

	/**
	 * @brief 获取策略上下文
	 * @param id 策略ID
	 * @return 策略上下文指针，如果不存在返回空指针
	 */
	UftContextPtr	getContext(uint32_t id);

	/**
	 * @brief 订阅委托队列数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @details 将指定策略添加到委托队列数据的订阅列表中
	 */
	void sub_order_queue(uint32_t sid, const char* stdCode);

	/**
	 * @brief 订阅委托明细数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @details 将指定策略添加到委托明细数据的订阅列表中
	 */
	void sub_order_detail(uint32_t sid, const char* stdCode);

	/**
	 * @brief 订阅成交数据
	 * @param sid 策略ID
	 * @param stdCode 标准化合约代码
	 * @details 将指定策略添加到成交数据的订阅列表中
	 */
	void sub_transaction(uint32_t sid, const char* stdCode);

private:
	uint32_t		_cur_date;      ///< 当前日期，格式YYYYMMDD
	uint32_t		_cur_time;      ///< 当前时间，为1分钟线K线时间（比如0900时刻对应0901分钟线K线，便于CTA策略使用）
	uint32_t		_cur_raw_time;  ///< 当前原始时间，格式HHMM
	uint32_t		_cur_secs;      ///< 当前秒数，包含毫秒
	uint32_t		_cur_tdate;     ///< 当前交易日期，格式YYYYMMDD

	IBaseDataMgr*	_base_data_mgr; ///< 基础数据管理器指针
	WtUftDtMgr*		_data_mgr;      ///< UFT数据管理器指针

	/**
	 * @brief 策略订阅列表类型，存储策略ID集合
	 * @details 使用哈希集实现的策略ID列表
	 */
	typedef wt_hashset<uint32_t> SubList;

	/**
	 * @brief 策略订阅映射类型，将合约代码映射到订阅的策略列表
	 * @details 使用字符串到策略列表的映射表
	 */
	typedef wt_hashmap<std::string, SubList>	StraSubMap;

	StraSubMap		_tick_sub_map;   ///< Tick数据订阅表，将合约映射到策略ID列表
	StraSubMap		_ordque_sub_map; ///< 委托队列订阅表，将合约映射到策略ID列表
	StraSubMap		_orddtl_sub_map; ///< 委托明细订阅表，将合约映射到策略ID列表
	StraSubMap		_trans_sub_map;  ///< 成交明细订阅表，将合约映射到策略ID列表
	StraSubMap		_bar_sub_map;    ///< K线数据订阅表，将合约周期指标映射到策略ID列表

	TraderAdapterMgr*	_adapter_mgr;  ///< 交易适配器管理器指针

	/**
	 * @brief 策略上下文映射类型，将策略ID映射到策略上下文
	 * @details 使用整数ID到策略上下文指针的映射表
	 */
	typedef wt_hashmap<uint32_t, UftContextPtr> ContextMap;
	ContextMap		_ctx_map;     ///< 策略上下文映射表

	WtUftRtTicker*	_tm_ticker;  ///< UFT实时计时器指针
	WTSVariant*		_cfg;         ///< 配置项变量集指针

	bool			_dependent;   ///< 子策略是否独立记账

	EventNotifier*	_notifier;    ///< 事件通知器指针
};

NS_WTP_END