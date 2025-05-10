/*!
 * @file WtRdmDtReaderAD.h
 * @author wondertrader
 * @brief 基于LMDB存储的历史数据读取器头文件
 * @details 定义了基于LMDB存储的历史行情数据（K线、Tick等）的读取器类
 *          实现了IRdmDtReader接口，提供从LMDB读取K线和行情数据的功能
 */

#pragma once
#include <string>
#include <stdint.h>
#include <boost/circular_buffer.hpp>

#include "DataDefineAD.h"

#include "../WTSUtils/WtLMDB.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/IRdmDtReader.h"

#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"

NS_WTP_BEGIN

/** @brief Boost内存映射文件的智能指针类型 */
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

/**
 * @brief 基于LMDB存储的历史数据读取器类
 * @details 通过LMDB实现的历史数据读取器，主要用于读取存储在LMDB中的K线和Tick等市场数据
 *          包含缓存机制，可以减少重复读取数据的开销
 */
class WtRdmDtReaderAD : public IRdmDtReader
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化数据读取器实例，设置初始参数
	 */
	WtRdmDtReaderAD();

	/**
	 * @brief 析构函数
	 * @details 清理资源并释放内存
	 */
	virtual ~WtRdmDtReaderAD();


//////////////////////////////////////////////////////////////////////////
//IRdmDtReader
public:
	/**
	 * @brief 初始化数据读取器
	 * @param cfg 配置对象
	 * @param sink 数据读取器回调接口
	 * @details 从配置对象中读取数据目录等信息，初始化数据读取器
	 */
	virtual void init(WTSVariant* cfg, IRdmDtReaderSink* sink);

	/**
	 * @brief 按时间区间读取逐笔成交数据片段
	 * @param stdCode 标准合约代码
	 * @param stime 开始时间
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 成交数据片段指针
	 * @note 当前版本未实现该方法
	 */
	virtual WTSOrdDtlSlice*	readOrdDtlSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override { return NULL; }

	/**
	 * @brief 按时间区间读取委托队列数据片段
	 * @param stdCode 标准合约代码
	 * @param stime 开始时间
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 委托队列数据片段指针
	 * @note 当前版本未实现该方法
	 */
	virtual WTSOrdQueSlice*	readOrdQueSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override { return NULL; }

	/**
	 * @brief 按时间区间读取逐笔成交数据片段
	 * @param stdCode 标准合约代码
	 * @param stime 开始时间
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 逐笔成交数据片段指针
	 * @note 当前版本未实现该方法
	 */
	virtual WTSTransSlice*	readTransSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override { return NULL; }

	/**
	 * @brief 按时间区间读取Tick数据片段
	 * @param stdCode 标准合约代码
	 * @param stime 开始时间
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return Tick数据片段指针
	 * @details 从指定时间区间读取Tick数据，如果数据在缓存中存在，则直接返回
	 */
	virtual WTSTickSlice*	readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override;

	/**
	 * @brief 按时间区间读取K线数据片段
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param stime 开始时间
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return K线数据片段指针
	 * @details 从指定时间区间读取K线数据，如果数据在缓存中存在，则直接返回
	 */
	virtual WTSKlineSlice*	readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime = 0) override;

	/**
	 * @brief 按数量读取Tick数据片段
	 * @param stdCode 标准合约代码
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return Tick数据片段指针
	 * @details 从etime开始向前读取指定数量的Tick数据片段
	 */
	virtual WTSTickSlice*	readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 按数量读取K线数据片段
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return K线数据片段指针
	 * @details 从etime开始向前读取指定数量的K线数据片段
	 */
	virtual WTSKlineSlice*	readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 按交易日读取Tick数据片段
	 * @param stdCode 标准合约代码
	 * @param uDate 交易日，默认为0表示当前交易日
	 * @return Tick数据片段指针
	 * @details 读取指定交易日的全部Tick数据
	 */
	virtual WTSTickSlice*	readTickSliceByDate(const char* stdCode, uint32_t uDate /* = 0 */) override;

private:
	std::string		_base_dir;        /**< 数据文件存储的基础路径 */
	IBaseDataMgr*	_base_data_mgr;    /**< 基础数据管理器指针 */
	IHotMgr*		_hot_mgr;          /**< 主力合约管理器指针 */

	/**
	 * @brief K线数据缓存结构体
	 * @details 用于缓存某一合约的K线数据，包含交易所、合约代码、周期等信息
	 */
	typedef struct _BarsList
	{
		std::string		_exchg;         /**< 交易所代码 */
		std::string		_code;          /**< 合约代码 */
		WTSKlinePeriod	_period;        /**< K线周期 */
		uint64_t		_last_bar_time;  /**< 最后K线时间 */

		std::vector<WTSBarStruct>	_bars;  /**< K线数据存储容器 */

		/**
		 * @brief 构造函数
		 * @details 初始化最后K线时间为0
		 */
		_BarsList() :_last_bar_time(0){}
	} BarsList;

	/**
	 * @brief Tick数据缓存结构体
	 * @details 用于缓存某一合约的Tick数据，包含交易所、合约代码、时间等信息
	 */
	typedef struct _TicksList
	{
		std::string		_exchg;         /**< 交易所代码 */
		std::string		_code;          /**< 合约代码 */
		uint64_t		_first_tick_time; /**< 第一个Tick时间 */
		uint64_t		_last_tick_time;  /**< 最后Tick时间 */

		std::vector<WTSTickStruct>	_ticks; /**< Tick数据存储容器 */

		/**
		 * @brief 构造函数
		 * @details 初始化最后Tick时间为0，第一个Tick时间为最大值
		 */
		_TicksList() :_last_tick_time(0), _first_tick_time(UINT64_MAX){}
	} TicksList;

	typedef wt_hashmap<std::string, BarsList> BarsCache;  /**< K线数据缓存容器类型定义 */
	BarsCache	_bars_cache;     /**< K线数据缓存映射表，以合约代码为键 */

	typedef wt_hashmap<std::string, TicksList> TicksCache;  /**< Tick数据缓存容器类型定义 */
	TicksCache	_ticks_cache;    /**< Tick数据缓存映射表，以合约代码为键 */

private:
	//////////////////////////////////////////////////////////////////////////
	/**
	 * 这里放LMDB的数据库定义
	 * K线数据，按照每个市场m1/m5/d1三个周期一共三个数据库，路径如./m1/CFFEX
	 * Tick数据，每个合约一个数据库，路径如./ticks/CFFEX/IF2101
	 */
	typedef std::shared_ptr<WtLMDB> WtLMDBPtr;      /**< LMDB数据库智能指针类型 */
	typedef wt_hashmap<std::string, WtLMDBPtr> WtLMDBMap;  /**< LMDB数据库映射表类型 */

	WtLMDBMap	_exchg_m1_dbs;  /**< 1分钟K线数据库映射表，以交易所为键 */
	WtLMDBMap	_exchg_m5_dbs;  /**< 5分钟K线数据库映射表，以交易所为键 */
	WtLMDBMap	_exchg_d1_dbs;  /**< 日线K线数据库映射表，以交易所为键 */

	WtLMDBMap	_tick_dbs;      /**< Tick数据库映射表，以交易所.合约代码为键，如BINANCE.BTCUSDT */

	/**
	 * @brief 获取K线数据库指针
	 * @param exchg 交易所代码
	 * @param period K线周期
	 * @return LMDB数据库指针
	 * @details 根据交易所和K线周期获取对应的LMDB数据库指针，如果不存在则创建
	 */
	WtLMDBPtr	get_k_db(const char* exchg, WTSKlinePeriod period);

	/**
	 * @brief 获取Tick数据库指针
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return LMDB数据库指针
	 * @details 根据交易所和合约代码获取对应的LMDB数据库指针，如果不存在则创建
	 */
	WtLMDBPtr	get_t_db(const char* exchg, const char* code);
};

NS_WTP_END
