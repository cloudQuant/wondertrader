/*!
 * \file WtDtRunner.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据服务运行器定义
 * 
 * \details 本文件定义了WtDtRunner类，该类是数据服务模块的核心组件，
 * 负责数据的加载、管理、查询和订阅功能。
 * 它封装了底层的数据存储和解析器管理，为上层应用提供统一的数据访问接口。
 */
#pragma once
#include "../WTSTools/WTSHotMgr.h"
#include "../WTSTools/WTSBaseDataMgr.h"
#include "../Share/StdUtils.hpp"

#include "PorterDefs.h"
#include "ParserAdapter.h"
#include "WtDataManager.h"

NS_WTP_BEGIN
class WTSVariant;
class WtDataStorage;
class WTSKlineSlice;
class WTSTickSlice;
NS_WTP_END

/**
 * @brief 数据服务运行器类
 * 
 * @details 该类是数据服务模块的核心组件，负责数据的加载、管理、查询和订阅功能。
 * 它管理基础数据、热点合约、数据存储和解析器等组件，
 * 并提供了一系列方法来查询历史数据和订阅实时数据。
 */
class WtDtRunner
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * @details 初始化数据服务运行器对象，设置初始状态
	 */
	WtDtRunner();

	/**
	 * @brief 析构函数
	 * 
	 * @details 清理数据服务运行器对象的资源，释放内存
	 */
	~WtDtRunner();

public:
	/**
	 * @brief 初始化数据服务运行器
	 * @param cfgFile 配置文件路径或配置内容字符串
	 * @param isFile 是否为文件路径，如果为true则cfgFile为文件路径，否则为配置内容字符串
	 * @param modDir 模块目录，用于加载解析器模块
	 * @param logCfg 日志配置文件路径
	 * @param cbTick Tick数据回调函数，用于接收实时Tick数据
	 * @param cbBar K线数据回调函数，用于接收实时K线数据
	 * 
	 * @details 该函数加载配置文件，初始化日志系统，初始化数据管理器和解析器等组件。
	 * 这是使用WtDtRunner的第一步，必须在调用其他方法前调用。
	 */
	void	initialize(const char* cfgFile, bool isFile = true, const char* modDir = "", const char* logCfg = "logcfg.yaml", 
				FuncOnTickCallback cbTick = NULL, FuncOnBarCallback cbBar = NULL);
	/**
	 * @brief 启动数据服务运行器
	 * 
	 * @details 该函数启动所有已初始化的解析器，开始接收实时数据。
	 * 必须在initialize函数调用后才能调用该函数。
	 */
	void	start();

	/**
	 * @brief 获取基础数据管理器
	 * @return WTSBaseDataMgr& 基础数据管理器引用
	 * 
	 * @details 该函数返回基础数据管理器的引用，用于访问合约代码、交易时间等基础数据。
	 */
	inline WTSBaseDataMgr& getBaseDataMgr() { return _bd_mgr; }

	/**
	 * @brief 获取热点合约管理器
	 * @return WTSHotMgr& 热点合约管理器引用
	 * 
	 * @details 该函数返回热点合约管理器的引用，用于管理期货的主力合约和次主力合约。
	 */
	inline WTSHotMgr& getHotMgr() { return _hot_mgr; }

public:
	/**
	 * @brief 处理Tick数据
	 * @param curTick 当前Tick数据指针
	 * 
	 * @details 该函数处理新到达的Tick数据，并将其转发给相应的订阅者。
	 * 通常由解析器在接收到新数据时调用。
	 */
	void	proc_tick(WTSTickData* curTick);

	/**
	 * @brief 触发Tick数据回调
	 * @param stdCode 标准化合约代码
	 * @param curTick 当前Tick数据指针
	 * 
	 * @details 该函数触发外部订阅者的Tick数据回调函数，通常在proc_tick函数内部调用。
	 */
	void	trigger_tick(const char* stdCode, WTSTickData* curTick);

	/**
	 * @brief 触发K线数据回调
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"d1"
	 * @param lastBar 最新K线数据指针
	 * 
	 * @details 该函数触发外部订阅者的K线数据回调函数，通常在新的K线生成时调用。
	 */
	void	trigger_bar(const char* stdCode, const char* period, WTSBarStruct* lastBar);

	/**
	 * @brief 订阅Tick数据
	 * @param stdCode 标准化合约代码
	 * @param bReplace 是否替换已有订阅，如果为true则清空已有订阅后再订阅新的合约
	 * @param bInner 是否为内部订阅，内部订阅不会触发外部回调
	 * 
	 * @details 该函数订阅指定合约的Tick数据。订阅成功后，当有新的Tick数据到达时，
	 * 会通过初始化时设置的回调函数通知调用者。
	 */
	void	sub_tick(const char* stdCode, bool bReplace, bool bInner = false);

	/**
	 * @brief 订阅K线数据
	 * @param stdCode 标准化合约代码
	 * @param period K线周期，如"m1"/"d1"
	 * 
	 * @details 该函数订阅指定合约的指定周期K线数据。订阅成功后，当有新的K线数据生成时，
	 * 会通过初始化时设置的回调函数通知调用者。
	 */
	void	sub_bar(const char* stdCode, const char* period);

	/**
	 * @brief 清理数据缓存
	 * 
	 * @details 该函数清理内存中的数据缓存，释放内存资源。
	 * 在需要释放内存或重新加载数据时调用该函数。
	 * 清理缓存后，再次查询数据时会重新从存储中加载。
	 */
	void	clear_cache();

public:
	/**
	 * @brief 根据时间范围获取K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
	 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数获取指定时间范围内的K线数据。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSKlineSlice*	get_bars_by_range(const char* stdCode, const char* period, uint64_t beginTime, uint64_t endTime = 0);

	/**
	 * @brief 根据日期获取K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * @param uDate 交易日期，格式为YYYYMMDD，如果为0则表示当前交易日
	 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数获取指定交易日的K线数据。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSKlineSlice*	get_bars_by_date(const char* stdCode, const char* period, uint32_t uDate = 0);

	/**
	 * @brief 根据时间范围获取Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param beginTime 开始时间，格式为YYYYMMDDHHmmss
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
	 * @return WTSTickSlice* Tick数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数获取指定时间范围内的Tick数据。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSTickSlice*	get_ticks_by_range(const char* stdCode, uint64_t beginTime, uint64_t endTime = 0);

	/**
	 * @brief 根据数量和结束时间获取K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param period K线周期，如"m1"/"d1"
	 * @param count 要获取的K线数量
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
	 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数从结束时间往前获取指定数量的K线数据。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSKlineSlice*	get_bars_by_count(const char* stdCode, const char* period, uint32_t count, uint64_t endTime = 0);

	/**
	 * @brief 根据数量和结束时间获取Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param count 要获取的Tick数量
	 * @param endTime 结束时间，格式为YYYYMMDDHHmmss，如果为0则表示当前时间
	 * @return WTSTickSlice* Tick数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数从结束时间往前获取指定数量的Tick数据。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSTickSlice*	get_ticks_by_count(const char* stdCode, uint32_t count, uint64_t endTime = 0);

	/**
	 * @brief 根据日期获取Tick数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param uDate 交易日期，格式为YYYYMMDD，如果为0则表示当前交易日
	 * @return WTSTickSlice* Tick数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数获取指定交易日的Tick数据。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSTickSlice*	get_ticks_by_date(const char* stdCode, uint32_t uDate = 0);

	/**
	 * @brief 根据日期获取秒线K线数据
	 * @param stdCode 标准化合约代码，格式如"SHFE.rb.HOT"
	 * @param secs 秒线周期，如常见的30秒成60秒
	 * @param uDate 交易日期，格式为YYYYMMDD，如果为0则表示当前交易日
	 * @return WTSKlineSlice* K线数据切片指针，使用后需要调用release方法释放
	 * 
	 * @details 该函数获取指定交易日的秒线K线数据。
	 * 秒线K线是指周期小于1分钟的K线，如30秒成60秒K线。
	 * 如果数据已经在缓存中，则直接从缓存返回；否则从存储中加载。
	 * 返回的数据切片需要用户调用release方法释放内存。
	 */
	WTSKlineSlice*	get_sbars_by_date(const char* stdCode, uint32_t secs, uint32_t uDate = 0);

private:
	/**
	 * @brief 初始化数据管理器
	 * @param config 配置对象指针
	 * 
	 * @details 该函数根据配置初始化数据管理器，包括数据存储和数据管理器等组件。
	 * 通常在initialize函数中调用。
	 */
	void	initDataMgr(WTSVariant* config);

	/**
	 * @brief 初始化解析器
	 * @param cfg 解析器配置对象指针
	 * 
	 * @details 该函数根据配置初始化解析器，加载并初始化各个解析器模块。
	 * 通常在initialize函数中调用。
	 */
	void	initParsers(WTSVariant* cfg);

private:
	/**
	 * @brief Tick数据回调函数
	 * @details 用于将实时Tick数据转发给外部订阅者
	 */
	FuncOnTickCallback	_cb_tick;

	/**
	 * @brief K线数据回调函数
	 * @details 用于将实时K线数据转发给外部订阅者
	 */
	FuncOnBarCallback	_cb_bar;

	/**
	 * @brief 基础数据管理器
	 * @details 管理合约代码、交易时间等基础数据
	 */
	WTSBaseDataMgr	_bd_mgr;

	/**
	 * @brief 热点合约管理器
	 * @details 管理期货的主力合约和次主力合约
	 */
	WTSHotMgr		_hot_mgr;

	/**
	 * @brief 数据存储对象指针
	 * @details 负责数据的存储和读取
	 */
	WtDataStorage*	_data_store;

	/**
	 * @brief 数据管理器
	 * @details 负责数据的管理和缓存
	 */
	WtDataManager	_data_mgr;

	/**
	 * @brief 解析器管理器
	 * @details 管理所有已加载的解析器
	 */
	ParserAdapterMgr	_parsers;

	/**
	 * @brief 初始化标志
	 * @details 标记数据服务运行器是否已经初始化
	 */
	bool			_is_inited;

	/**
	 * @brief 订阅标志集合类型
	 * @details 用于存储订阅标志
	 */
	typedef std::set<uint32_t> SubFlags;

	/**
	 * @brief 订阅映射表类型
	 * @details 用于存储合约代码到订阅标志的映射
	 */
	typedef wt_hashmap<std::string, SubFlags>	StraSubMap;

	/**
	 * @brief Tick数据订阅表
	 * @details 存储外部订阅者的Tick数据订阅信息
	 */
	StraSubMap		_tick_sub_map;

	/**
	 * @brief 订阅表互斥锁
	 * @details 保护订阅表的线程安全访问
	 */
	StdUniqueMutex	_mtx_subs;

	/**
	 * @brief 内部Tick数据订阅表
	 * @details 存储内部订阅者的Tick数据订阅信息
	 */
	StraSubMap		_tick_innersub_map;

	/**
	 * @brief 内部订阅表互斥锁
	 * @details 保护内部订阅表的线程安全访问
	 */
	StdUniqueMutex	_mtx_innersubs;
};

