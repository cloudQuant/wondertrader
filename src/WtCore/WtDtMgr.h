/*!
 * \file WtDataManager.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器头文件
 * 
 * 该文件定义了WonderTrader的数据管理器，负责管理历史数据和实时行情数据
 * 包括K线、Tick、委托队列、成交明细等数据的获取和缓存
 * 是WonderTrader中数据访问的核心组件
 */
#pragma once
#include <vector>
#include "../Includes/IDataReader.h"
#include "../Includes/IDataManager.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/WTSCollection.hpp"

NS_WTP_BEGIN
class WTSVariant;
class WTSTickData;
class WTSKlineSlice;
class WTSTickSlice;
class IBaseDataMgr;
class IBaseDataMgr;
class WtEngine;

/**
 * @brief 数据管理器类
 * 
 * @details 该类实现了IDataReaderSink和IDataManager接口
 * 负责管理历史数据和实时行情数据，包括K线、Tick、委托队列、成交明细等
 * 提供了数据的缓存和访问机制，是策略引擎数据访问的核心组件
 */
class WtDtMgr : public IDataReaderSink, public IDataManager
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化数据管理器对象
	 */
	WtDtMgr();

	/**
	 * @brief 析构函数
	 * @details 清理数据管理器对象的资源
	 */
	~WtDtMgr();

private:
	/**
	 * @brief 初始化数据存储
	 * @param cfg 配置项
	 * @return bool 初始化是否成功
	 * @details 根据配置初始化数据存储模块，设置数据缓存策略
	 */
	bool	initStore(WTSVariant* cfg);

public:
	/**
	 * @brief 初始化数据管理器
	 * @param cfg 配置项
	 * @param engine 交易引擎指针
	 * @param bForceCache 是否强制缓存数据，默认为false
	 * @return bool 初始化是否成功
	 * @details 根据配置初始化数据管理器，设置数据读取器和引擎关联
	 */
	bool	init(WTSVariant* cfg, WtEngine* engine, bool bForceCache = false);

	/**
	 * @brief 注册历史数据加载器
	 * @param loader 历史数据加载器指针
	 * @details 设置用于加载历史数据的加载器对象
	 */
	void	regsiter_loader(IHisDataLoader* loader) { _loader = loader; }

	/**
	 * @brief 处理推送的行情数据
	 * @param stdCode 标准化合约代码
	 * @param newTick 新的Tick数据
	 * @details 处理实时推送的行情数据，更新内部缓存并进行必要的数据处理
	 */
	void	handle_push_quote(const char* stdCode, WTSTickData* newTick);

	//////////////////////////////////////////////////////////////////////////
	//IDataManager 接口
	/**
	 * @brief 获取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return WTSTickSlice* Tick数据切片指针，由调用者负责释放
	 * @details 获取指定合约的历史Tick数据切片，包含指定数量的最新数据
	 */
	virtual WTSTickSlice* get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	/**
	 * @brief 获取委托队列数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return WTSOrdQueSlice* 委托队列数据切片指针，由调用者负责释放
	 * @details 获取指定合约的委托队列历史数据，包含指定数量的最新数据
	 */
	virtual WTSOrdQueSlice* get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取委托明细数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return WTSOrdDtlSlice* 委托明细数据切片指针，由调用者负责释放
	 * @details 获取指定合约的委托明细历史数据，包含指定数量的最新数据
	 */
	virtual WTSOrdDtlSlice* get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取成交明细数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return WTSTransSlice* 成交明细数据切片指针，由调用者负责释放
	 * @details 获取指定合约的成交明细历史数据，包含指定数量的最新数据
	 */
	virtual WTSTransSlice* get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	/**
	 * @brief 获取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param times 周期倍数，用于生成非标准周期的K线
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return WTSKlineSlice* K线数据切片指针，由调用者负责释放
	 * @details 获取指定合约的历史K线数据，支持标准周期和非标准周期
	 */
	virtual WTSKlineSlice* get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 获取最新的Tick数据
	 * @param stdCode 标准化合约代码
	 * @return WTSTickData* Tick数据指针，由调用者负责释放
	 * @details 获取指定合约的最新市场行情数据
	 */
	virtual WTSTickData* grab_last_tick(const char* stdCode) override;

	/**
	 * @brief 获取复权因子
	 * @param stdCode 标准化合约代码
	 * @param uDate 日期，格式YYYYMMDD
	 * @return double 复权因子
	 * @details 获取指定合约在指定日期的复权因子，用于数据复权
	 */
	virtual double get_adjusting_factor(const char* stdCode, uint32_t uDate) override;

	/**
	 * @brief 获取复权模式标志
	 * @return uint32_t 复权模式标志，0-不复权，1-前复权，2-后复权
	 * @details 获取当前系统设置的复权模式
	 */
	virtual uint32_t get_adjusting_flag() override;

	//////////////////////////////////////////////////////////////////////////
	//IDataReaderSink
	/**
	 * @brief K线数据回调
	 * @param code 合约代码
	 * @param period K线周期
	 * @param newBar 新的K线数据
	 * @details 当有新的K线数据生成时，由数据读取器调用此回调函数
	 */
	virtual void	on_bar(const char* code, WTSKlinePeriod period, WTSBarStruct* newBar) override;

	/**
	 * @brief 所有K线数据更新完成回调
	 * @param updateTime 更新时间
	 * @details 当所有K线数据更新完成时，由数据读取器调用此回调函数
	 */
	virtual void	on_all_bar_updated(uint32_t updateTime) override;

	/**
	 * @brief 获取基础数据管理器
	 * @return IBaseDataMgr* 基础数据管理器指针
	 * @details 返回用于管理基础数据（合约、交易所等）的管理器
	 */
	virtual IBaseDataMgr*	get_basedata_mgr() override;

	/**
	 * @brief 获取主力合约管理器
	 * @return IHotMgr* 主力合约管理器指针
	 * @details 返回用于管理主力合约映射的管理器
	 */
	virtual IHotMgr*		get_hot_mgr() override;

	/**
	 * @brief 获取当前交易日期
	 * @return uint32_t 交易日期，格式YYYYMMDD
	 * @details 返回当前交易系统的日期
	 */
	virtual uint32_t	get_date() override;

	/**
	 * @brief 获取当前交易分钟时间
	 * @return uint32_t 分钟时间，格式HHMM
	 * @details 返回当前交易系统的分钟时间
	 */
	virtual uint32_t	get_min_time()override;

	/**
	 * @brief 获取当前交易秒数
	 * @return uint32_t 秒数，从0点开始的秒数
	 * @details 返回当前交易系统的秒数
	 */
	virtual uint32_t	get_secs() override;

	/**
	 * @brief 记录数据读取器日志
	 * @param ll 日志级别
	 * @param message 日志消息
	 * @details 由数据读取器调用，用于记录数据读取过程中的日志
	 */
	virtual void		reader_log(WTSLogLevel ll, const char* message) override;

	/**
	 * @brief 获取数据读取器
	 * @return IDataReader* 数据读取器指针
	 * @details 返回当前数据管理器使用的数据读取器
	 */
	inline IDataReader*	reader() { return _reader; }

	/**
	 * @brief 获取历史数据加载器
	 * @return IHisDataLoader* 历史数据加载器指针
	 * @details 返回当前数据管理器使用的历史数据加载器
	 */
	inline IHisDataLoader*	loader() { return _loader; }

private:
	/**
	 * @brief 数据读取器指针
	 * @details 用于读取各种历史和实时数据
	 */
	IDataReader*	_reader;

	/**
	 * @brief 历史数据加载器指针
	 * @details 用于加载历史数据，可以由外部注册
	 */
	IHisDataLoader*	_loader;

	/**
	 * @brief 交易引擎指针
	 * @details 关联的交易引擎，用于获取交易日期、时间等信息
	 */
	WtEngine*		_engine;

	/**
	 * @brief 强制小节对齐标志
	 * @details 如果为true，则K线数据将强制按照交易时间小节对齐
	 */
	bool			_align_by_section;	//强制小节对齐

	/**
	 * @brief 强制缓存K线标志
	 * @details 如果为true，则强制缓存K线数据，提高访问效率
	 */
	bool			_force_cache;		//强制缓存K线

	/**
	 * @brief 订阅的基础K线集合
	 * @details 存储已订阅的基础K线合约代码
	 */
	wt_hashset<std::string> _subed_basic_bars;

	/**
	 * @brief 数据缓存映射类型定义
	 * @details 用于定义数据缓存映射，键为字符串，值为数据对象
	 */
	typedef WTSHashMap<std::string> DataCacheMap;

	/**
	 * @brief K线缓存映射
	 * @details 缓存各种K线数据，提高访问效率
	 */
	DataCacheMap*	_bars_cache;	//K线缓存

	/**
	 * @brief 实时Tick缓存映射
	 * @details 缓存实时Tick数据，提高访问效率
	 */
	DataCacheMap*	_rt_tick_map;	//实时tick缓存

	//By Wesley @ 2022.02.11
	//这个只有后复权tick数据
	//因为前复权和不复权，都不需要缓存
	/**
	 * @brief 复权Tick缓存映射
	 * @details 缓存后复权的Tick数据，前复权和不复权的数据不需要缓存
	 */
	DataCacheMap*	_ticks_adjusted;	//复权tick缓存

	/**
	 * @brief K线数据通知项结构
	 * @details 用于存储K线数据更新通知的相关信息
	 */
	typedef struct _NotifyItem
	{
		/**
		 * @brief 合约代码
		 * @details 存储标准化合约代码
		 */
		char		_code[MAX_INSTRUMENT_LENGTH];

		/**
		 * @brief K线周期
		 * @details 存储K线周期代码，如'm'表示分钟线
		 */
		char		_period[2] = { 0 };

		/**
		 * @brief 周期倍数
		 * @details 存储K线周期的倍数，用于非标准周期
		 */
		uint32_t	_times;

		/**
		 * @brief 新K线数据指针
		 * @details 指向新生成的K线数据
		 */
		WTSBarStruct* _newBar;

		/**
		 * @brief 构造函数
		 * @param code 合约代码
		 * @param period K线周期
		 * @param times 周期倍数
		 * @param newBar 新K线数据指针
		 * @details 初始化通知项结构
		 */
		_NotifyItem(const char* code, char period, uint32_t times, WTSBarStruct* newBar)
			: _times(times), _newBar(newBar)
		{
			wt_strcpy(_code, code);
			_period[0] = period;
		}
	} NotifyItem;

	/**
	 * @brief K线数据通知项列表
	 * @details 存储待处理的K线数据更新通知
	 */
	std::vector<NotifyItem> _bar_notifies;
};

NS_WTP_END