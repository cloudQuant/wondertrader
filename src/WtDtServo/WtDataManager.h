/*!
 * \file WtDataManager.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器头文件，定义了WonderTrader的数据管理核心类
 * 
 * 该文件定义了WtDataManager类，负责管理和提供交易数据，包括：
 * 1. 行情数据（Tick、K线）的获取和缓存
 * 2. 订单队列、订单明细、成交明细等数据的获取
 * 3. 数据订阅和实时更新
 * 4. 复权因子的计算和应用
 */
#pragma once
#include <vector>
#include <stdint.h>

#include "../Includes/IDataManager.h"
#include "../Includes/IRdmDtReader.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSCollection.hpp"
#include "../Share/StdUtils.hpp"


class WtDtRunner;

NS_WTP_BEGIN
class WTSVariant;
class WTSTickData;
class WTSKlineSlice;
class WTSKlineData;
class WTSTickSlice;
class IBaseDataMgr;
class IHotMgr;
class WTSSessionInfo;
struct WTSBarStruct;
class WTSCommodityInfo;

/**
 * @brief 数据管理器类
 * 
 * @details WtDataManager类是WonderTrader的数据管理核心类，实现了IRdmDtReaderSink接口。
 * 该类负责：
 * 1. 数据存储和读取的初始化
 * 2. 提供各种方式的数据获取接口（按日期、按时间范围、按数量）
 * 3. 管理数据缓存和订阅
 * 4. 处理实时数据更新
 * 5. 提供复权因子计算
 */
class WtDataManager : public IRdmDtReaderSink
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * @details 初始化数据管理器对象，初始化内部成员变量
	 */
	WtDataManager();

	/**
	 * @brief 析构函数
	 * 
	 * @details 清理数据管理器对象的资源，释放内存
	 */
	~WtDataManager();

private:
	/**
	 * @brief 初始化数据存储
	 * @param cfg 配置对象指针
	 * @return 初始化是否成功
	 * 
	 * @details 根据配置初始化数据存储模块，包括设置数据路径、创建数据读取器等
	 */
	bool	initStore(WTSVariant* cfg);

	/**
	 * @brief 获取交易时段信息
	 * @param sid 交易时段ID或合约代码
	 * @param isCode 是否为合约代码，默认为false
	 * @return 交易时段信息对象指针
	 * 
	 * @details 根据时段ID或合约代码获取交易时段信息。如果isCode为true，则sid被视为合约代码，需要先获取对应的时段ID
	 */
	WTSSessionInfo* get_session_info(const char* sid, bool isCode = false);

//////////////////////////////////////////////////////////////////////////
//IRdmDtReaderSink
public:
	/*
	 *	@brief	获取基础数据管理接口指针
	 */
	virtual IBaseDataMgr*	get_basedata_mgr() override { return _bd_mgr; }

	/*
	 *	@brief	获取主力切换规则管理接口指针
	 */
	virtual IHotMgr*		get_hot_mgr() override { return _hot_mgr; }

	/*
	 *	@brief	输出数据读取模块的日志
	 */
	virtual void			reader_log(WTSLogLevel ll, const char* message) override;

public:
	/**
	 * @brief 初始化数据管理器
	 * @param cfg 配置对象指针
	 * @param runner 数据服务运行器指针
	 * @return 初始化是否成功
	 * 
	 * @details 根据配置初始化数据管理器，包括初始化数据存储、基础数据管理器、主力合约管理器等
	 * 该方法是数据管理器使用前必须调用的初始化方法
	 */
	bool	init(WTSVariant* cfg, WtDtRunner* runner);

	/**
	 * @brief 获取订单队列数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return 订单队列数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的订单队列数据
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSOrdQueSlice* get_order_queue_slice(const char* stdCode, uint64_t stime, uint64_t etime = 0);

	/**
	 * @brief 获取订单明细数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return 订单明细数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的订单明细数据
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSOrdDtlSlice* get_order_detail_slice(const char* stdCode, uint64_t stime, uint64_t etime = 0);

	/**
	 * @brief 获取成交明细数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return 成交明细数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的成交明细数据
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSTransSlice* get_transaction_slice(const char* stdCode, uint64_t stime, uint64_t etime = 0);

	/**
	 * @brief 根据日期获取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param uDate 交易日期，格式为YYYYMMDD，默认为0（表示当前日期）
	 * @return Tick数据切片指针
	 * 
	 * @details 获取指定日期的Tick数据。如果日期为0，则使用当前日期。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSTickSlice* get_tick_slice_by_date(const char* stdCode, uint32_t uDate = 0);

	/**
	 * @brief 根据日期获取秒线数据切片
	 * @param stdCode 标准化合约代码
	 * @param secs 秒线周期，单位为秒
	 * @param uDate 交易日期，格式为YYYYMMDD，默认为0（表示当前日期）
	 * @return 秒线K线数据切片指针
	 * 
	 * @details 获取指定日期的秒线数据。如果日期为0，则使用当前日期。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSKlineSlice* get_skline_slice_by_date(const char* stdCode, uint32_t secs, uint32_t uDate = 0);

	/**
	 * @brief 根据日期获取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期类型（分钟、日线等）
	 * @param times 周期倍数，如当5分钟线时为5
	 * @param uDate 交易日期，格式为YYYYMMDD，默认为0（表示当前日期）
	 * @return K线数据切片指针
	 * 
	 * @details 获取指定日期的K线数据。如果日期为0，则使用当前日期。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSKlineSlice* get_kline_slice_by_date(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t uDate = 0);

	/**
	 * @brief 根据时间范围获取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return Tick数据切片指针
	 * 
	 * @details 获取指定时间范围内的Tick数据。如果结束时间为0，则使用当前时间作为结束时间。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSTickSlice* get_tick_slices_by_range(const char* stdCode, uint64_t stime, uint64_t etime = 0);

	/**
	 * @brief 根据时间范围获取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期类型（分钟、日线等）
	 * @param times 周期倍数，如当5分钟线时为5
	 * @param stime 开始时间，格式为YYYYMMDDHHMMSS
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return K线数据切片指针
	 * 
	 * @details 获取指定时间范围内的K线数据。如果结束时间为0，则使用当前时间作为结束时间。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSKlineSlice* get_kline_slice_by_range(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t stime, uint64_t etime = 0);

	/**
	 * @brief 根据数量获取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 要获取的Tick数量
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return Tick数据切片指针
	 * 
	 * @details 获取指定数量的Tick数据。如果结束时间为0，则使用当前时间作为结束时间。
	 * 返回的数据是从结束时间往前数的count条数据。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSTickSlice* get_tick_slice_by_count(const char* stdCode, uint32_t count, uint64_t etime = 0);

	/**
	 * @brief 根据数量获取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期类型（分钟、日线等）
	 * @param times 周期倍数，如当5分钟线时为5
	 * @param count 要获取的K线数量
	 * @param etime 结束时间，格式为YYYYMMDDHHMMSS，默认为0（表示当前时间）
	 * @return K线数据切片指针
	 * 
	 * @details 获取指定数量的K线数据。如果结束时间为0，则使用当前时间作为结束时间。
	 * 返回的数据是从结束时间往前数的count条数据。
	 * 返回的数据切片需要用户调用release方法释放内存
	 */
	WTSKlineSlice* get_kline_slice_by_count(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0);

	/*
	 *	获取复权因子
	 *	@stdCode	合约代码
	 *	@commInfo	品种信息
	 */
	double	get_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo = NULL);

	/**
	 * @brief 订阅K线数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期类型（分钟、日线等）
	 * @param times 周期倍数，如当5分钟线时为5
	 * 
	 * @details 订阅指定合约的K线数据。订阅后，当有新的Tick数据到来时，会自动生成并更新K线数据。
	 * 该方法通常由WtDtRunner的sub_bar方法调用。
	 */
	void	subscribe_bar(const char* stdCode, WTSKlinePeriod period, uint32_t times);

	/**
	 * @brief 清除所有K线数据订阅
	 * 
	 * @details 清除所有已订阅的K线数据。该方法通常在重新订阅或重置系统状态时调用。
	 */
	void	clear_subbed_bars();

	/**
	 * @brief 更新K线数据
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据指针
	 * 
	 * @details 根据新的Tick数据更新K线数据。该方法会根据已订阅的K线周期，自动生成或更新相应的K线数据。
	 * 如果生成了新的K线数据，还会触发相应的回调函数。
	 * 该方法通常由WtDtRunner的trigger_tick方法调用。
	 */
	void	update_bars(const char* stdCode, WTSTickData* newTick);

	/**
	 * @brief 清除所有数据缓存
	 * 
	 * @details 清除数据管理器中的所有数据缓存，包括K线缓存和实时数据缓存。
	 * 该方法通常在重置系统状态或释放内存时调用。
	 */
	void	clear_cache();

private:
	/// @brief 数据读取器指针，用于从存储中读取数据
	IRdmDtReader*			_reader;
	/// @brief 数据读取器删除函数，用于释放读取器资源
	FuncDeleteRdmDtReader	_remover;

	/// @brief 基础数据管理器指针，管理合约、交易时段等基础数据
	IBaseDataMgr*	_bd_mgr;
	/// @brief 主力合约管理器指针，管理主力合约切换规则
	IHotMgr*		_hot_mgr;
	/// @brief 数据服务运行器指针，用于回调数据更新
	WtDtRunner*		_runner;
	/// @brief 是否按交易时段对齐数据，当为true时会根据交易时段对数据进行对齐
	bool			_align_by_section;

	//K线缓存
	/**
	 * @brief K线缓存结构体
	 * 
	 * @details 用于存储缓存的K线数据及相关信息
	 */
	typedef struct _BarCache
	{
		WTSKlineData*	_bars;          ///< K线数据指针
		uint64_t		_last_bartime;    ///< 最后一条K线的时间
		WTSKlinePeriod	_period;        ///< K线周期类型
		uint32_t		_times;          ///< 周期倍数

		/// @brief 构造函数，初始化成员变量
		_BarCache():_last_bartime(0),_period(KP_DAY),_times(1),_bars(NULL){}
	} BarCache;
	/// @brief K线缓存映射类型，键为合约代码，值为K线缓存
	typedef wt_hashmap<std::string, BarCache>	BarCacheMap;
	/// @brief K线缓存映射，存储所有合约的K线缓存
	BarCacheMap	_bars_cache;

	/// @brief 实时K线映射类型，用于存储实时生成的K线数据
	typedef WTSHashMap<std::string>	RtBarMap;
	/// @brief 实时K线映射，存储实时生成的K线数据
	RtBarMap*		_rt_bars;
	/// @brief 实时K线数据的互斥锁，保证线程安全
	StdUniqueMutex	_mtx_rtbars;
};

NS_WTP_END