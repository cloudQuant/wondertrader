/**
 * @file WtDataReaderAD.h
 * @author Wesley
 * @brief 基于LMDB的数据读取器定义
 * 
 * 该文件定义了基于LMDB数据库的历史数据读取器，用于从LMDB数据库中读取历史行情数据
 * 包括K线数据和Tick数据，并提供缓存机制以提高读取效率
 */

#pragma once
#include <string>
#include <stdint.h>
#include <boost/circular_buffer.hpp>

#include "DataDefineAD.h"

#include "../WTSUtils/WtLMDB.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/IDataReader.h"

#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"

NS_WTP_BEGIN

/**
 * @brief Boost内存映射文件的智能指针类型定义
 */
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

/**
 * @brief 基于LMDB的数据读取器类
 * 
 * 该类实现了IDataReader接口，用于从LMDB数据库中读取历史行情数据，
 * 包括Tick数据和K线数据（日线、1分钟、5分钟）。
 * 实现了数据缓存机制，提高数据读取效率。
 */
class WtDataReaderAD : public IDataReader
{
public:
	/**
	 * @brief 构造函数
	 */
	WtDataReaderAD();

	/**
	 * @brief 析构函数
	 */
	virtual ~WtDataReaderAD();


public:
	/**
	 * @brief 初始化数据读取器
	 * 
	 * @param cfg 配置项，包含数据存储路径等信息
	 * @param sink 数据接收器，用于接收读取器返回的数据和日志
	 * @param loader 历史数据加载器，可选参数，默认为NULL
	 */
	virtual void init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) override;

	/**
	 * @brief 分钟结束时的回调函数，用于更新缓存数据
	 * 
	 * @param uDate 当前日期，格式YYYYMMDD
	 * @param uTime 当前时间，格式HHMM
	 * @param endTDate 结束交易日，默认为0
	 */
	virtual void onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override;

	/**
	 * @brief 读取Tick数据切片
	 * 
	 * @param stdCode 标准合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，格式YYYYMMDDHHMMSSmmm，默认为0（表示当前时间）
	 * @return WTSTickSlice* Tick数据切片指针，需要用户释放
	 */
	virtual WTSTickSlice*	readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 读取K线数据切片
	 * 
	 * @param stdCode 标准合约代码
	 * @param period K线周期，支持日线、1分钟、5分钟
	 * @param count 请求的数据条数
	 * @param etime 结束时间，格式YYYYMMDDHHMMSSmmm，默认为0（表示当前时间）
	 * @return WTSKlineSlice* K线数据切片指针，需要用户释放
	 */
	virtual WTSKlineSlice*	readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override;

private:
	/**
	 * @brief 数据存储的基础目录
	 */
	std::string		_base_dir;

	/**
	 * @brief 基础数据管理器指针，用于获取合约信息
	 */
	IBaseDataMgr*	_base_data_mgr;

	/**
	 * @brief 主力合约管理器指针，用于获取主力合约映射
	 */
	IHotMgr*		_hot_mgr;

	/**
	 * @brief 实时K线缓存包装器结构体
	 */
	typedef struct _RTBarCacheWrapper
	{
		/**
		 * @brief 互斥锁，用于保护缓存访问
		 */
		StdUniqueMutex	_mtx;

		/**
		 * @brief 缓存文件名
		 */
		std::string		_filename;

		/**
		 * @brief 缓存索引映射，键为合约代码，值为索引位置
		 */
		wt_hashmap<std::string, uint32_t> _idx;

		/**
		 * @brief 内存映射文件指针
		 */
		BoostMFPtr		_file_ptr;

		/**
		 * @brief 缓存块指针
		 */
		RTBarCache*		_cache_block;

		/**
		 * @brief 上次缓存大小
		 */
		uint32_t		_last_size;

		/**
		 * @brief 构造函数
		 */
		_RTBarCacheWrapper() :_cache_block(NULL), _file_ptr(NULL), _last_size(0){}

		/**
		 * @brief 检查缓存是否为空
		 * 
		 * @return true 缓存为空
		 * @return false 缓存非空
		 */
		inline bool empty() const { return _cache_block == NULL; }
	} RTBarCacheWrapper;

	/**
	 * @brief 1分钟K线缓存
	 */
	RTBarCacheWrapper _m1_cache;

	/**
	 * @brief 5分钟K线缓存
	 */
	RTBarCacheWrapper _m5_cache;

	/**
	 * @brief 日线K线缓存
	 */
	RTBarCacheWrapper _d1_cache;

	/**
	 * @brief K线数据列表结构体，用于缓存某一合约的K线数据
	 */
	typedef struct _BarsList
	{
		/**
		 * @brief 交易所代码
		 */
		std::string		_exchg;

		/**
		 * @brief 合约代码
		 */
		std::string		_code;

		/**
		 * @brief K线周期
		 */
		WTSKlinePeriod	_period;

		/**
		 * @brief 最后一条是否从缓存里读取的
		 * 如果是，下次更新的时候要从数据库更新一次，最后一条再按照原有逻辑处理
		 */
		bool			_last_from_cache;

		/**
		 * @brief 最后请求时间
		 */
		uint64_t		_last_req_time;

		/**
		 * @brief K线数据循环缓冲区
		 */
		boost::circular_buffer<WTSBarStruct>	_bars;

		/**
		 * @brief 构造函数
		 */
		_BarsList():_last_from_cache(false),_last_req_time(0){}
	} BarsList;

	/**
	 * @brief Tick数据列表结构体，用于缓存某一合约的Tick数据
	 */
	typedef struct _TicksList
	{
		/**
		 * @brief 交易所代码
		 */
		std::string		_exchg;

		/**
		 * @brief 合约代码
		 */
		std::string		_code;

		/**
		 * @brief 最后请求时间
		 */
		uint64_t		_last_req_time;

		/**
		 * @brief Tick数据循环缓冲区
		 */
		boost::circular_buffer<WTSTickStruct>	_ticks;

		/**
		 * @brief 构造函数
		 */
		_TicksList():_last_req_time(0){}
	} TicksList;

	/**
	 * @brief K线数据缓存类型，以标准合约代码为键
	 */
	typedef wt_hashmap<std::string, BarsList> BarsCache;

	/**
	 * @brief K线数据缓存实例
	 */
	BarsCache	_bars_cache;

	/**
	 * @brief Tick数据缓存类型，以标准合约代码为键
	 */
	typedef wt_hashmap<std::string, TicksList> TicksCache;

	/**
	 * @brief Tick数据缓存实例
	 */
	TicksCache	_ticks_cache;

	/**
	 * @brief 最后处理时间
	 */
	uint64_t	_last_time;	

private:
	/**
	 * @brief 将历史数据放入缓存
	 * 
	 * @param key 缓存键名，通常是标准合约代码
	 * @param stdCode 标准合约代码
	 * @param period K线周期
	 * @param count 请求的数据条数
	 * @return true 缓存成功
	 * @return false 缓存失败
	 */
	bool	cacheBarsFromStorage(const std::string& key, const char* stdCode, WTSKlinePeriod period, uint32_t count);

	/**
	 * @brief 从LMDB数据库中更新缓存的数据
	 * 
	 * @param barsList 要更新的K线数据列表
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期
	 * @param lastBarTime 最后K线时间，引用参数，会被更新
	 */
	void	update_cache_from_lmdb(BarsList& barsList, const char* exchg, const char* code, WTSKlinePeriod period, uint32_t& lastBarTime);

	/**
	 * @brief 将K线数据读入到缓冲区
	 * 
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期
	 * @return std::string 包含K线数据的字符串缓冲区
	 */
	std::string	read_bars_to_buffer(const char* exchg, const char* code, WTSKlinePeriod period);

	/**
	 * @brief 获取实时缓存中的K线数据
	 * 
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期
	 * @return WTSBarStruct* K线数据结构指针，如果不存在则返回NULL
	 */
	WTSBarStruct* get_rt_cache_bar(const char* exchg, const char* code, WTSKlinePeriod period);

private:
	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief LMDB的数据库定义
	 * K线数据，按照每个市场m1/m5/d1三个周期一共三个数据库，路径如./m1/CFFEX
	 * Tick数据，每个合约一个数据库，路径如./ticks/CFFEX/IF2101
	 */
	typedef std::shared_ptr<WtLMDB> WtLMDBPtr;
	typedef wt_hashmap<std::string, WtLMDBPtr> WtLMDBMap;

	/**
	 * @brief 1分钟K线数据库映射，以交易所为键
	 */
	WtLMDBMap	_exchg_m1_dbs;

	/**
	 * @brief 5分钟K线数据库映射，以交易所为键
	 */
	WtLMDBMap	_exchg_m5_dbs;

	/**
	 * @brief 日线数据库映射，以交易所为键
	 */
	WtLMDBMap	_exchg_d1_dbs;

	/**
	 * @brief Tick数据库映射，用exchg.code作为key，如BINANCE.BTCUSDT
	 */
	WtLMDBMap	_tick_dbs;

	/**
	 * @brief 获取指定交易所和周期的K线数据库
	 * 
	 * @param exchg 交易所代码
	 * @param period K线周期，支持1分钟、5分钟和日线
	 * @return WtLMDBPtr 返回LMDB数据库指针，如果不存在则创建
	 */
	WtLMDBPtr	get_k_db(const char* exchg, WTSKlinePeriod period);

	/**
	 * @brief 获取指定交易所和合约的Tick数据库
	 * 
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return WtLMDBPtr 返回LMDB数据库指针，如果不存在则创建
	 */
	WtLMDBPtr	get_t_db(const char* exchg, const char* code);
};

NS_WTP_END
