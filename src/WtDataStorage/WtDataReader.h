/*!
 * \file WtDataReader.h
 * \project	WonderTrader
 *
 * \author Wesley
 * 
 * \brief 数据读取器定义
 * \details 定义了数据读取器WtDataReader类，用于读取实时和历史行情数据
 */

#pragma once
#include <string>
#include <stdint.h>

#include "DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IDataReader.h"

#include "../Share/BoostMappingFile.hpp"

NS_WTP_BEGIN

typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

/**
 * @brief 数据读取器类
 * @details 实现了IDataReader接口，用于读取实时和历史行情数据，包括k线、tick、逆序、委托队列、成交等数据
 */
class WtDataReader : public IDataReader
{
public:
	/**
	 * @brief 构造函数
	 */
	WtDataReader();

	/**
	 * @brief 析构函数
	 */
	virtual ~WtDataReader();

private:
	/**
	 * @brief 实时K线数据块对
	 * @details 存储实时K线数据的内存映射文件和数据块指针
	 */
	typedef struct _RTKBlockPair
	{
		RTKlineBlock*	_block;		///< K线数据块指针
		BoostMFPtr		_file;		///< 内存映射文件指针
		uint64_t		_last_cap;	///< 上次容量

		/**
		 * @brief 构造函数
		 */
		_RTKBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}

	} RTKlineBlockPair;
	typedef wt_hashmap<std::string, RTKlineBlockPair>	RTKBlockFilesMap;

	/**
	 * @brief 实时Tick数据块对
	 * @details 存储实时Tick数据的内存映射文件和数据块指针
	 */
	typedef struct _TBlockPair
	{
		RTTickBlock*	_block;		///< Tick数据块指针
		BoostMFPtr		_file;		///< 内存映射文件指针
		uint64_t		_last_cap;	///< 上次容量

		/**
		 * @brief 构造函数
		 */
		_TBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} TickBlockPair;
	typedef wt_hashmap<std::string, TickBlockPair>	TBlockFilesMap;

	/**
	 * @brief 实时成交数据块对
	 * @details 存储实时成交数据的内存映射文件和数据块指针
	 */
	typedef struct _TransBlockPair
	{
		RTTransBlock*	_block;		///< 成交数据块指针
		BoostMFPtr		_file;		///< 内存映射文件指针
		uint64_t		_last_cap;	///< 上次容量

		std::shared_ptr< std::ofstream>	_fstream;	///< 文件流指针

		/**
		 * @brief 构造函数
		 */
		_TransBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} TransBlockPair;
	typedef wt_hashmap<std::string, TransBlockPair>	TransBlockFilesMap;

	/**
	 * @brief 实时委托明细数据块对
	 * @details 存储实时委托明细数据的内存映射文件和数据块指针
	 */
	typedef struct _OdrDtlBlockPair
	{
		RTOrdDtlBlock*	_block;		///< 委托明细数据块指针
		BoostMFPtr		_file;		///< 内存映射文件指针
		uint64_t		_last_cap;	///< 上次容量

		std::shared_ptr< std::ofstream>	_fstream;	///< 文件流指针

		/**
		 * @brief 构造函数
		 */
		_OdrDtlBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} OrdDtlBlockPair;
	typedef wt_hashmap<std::string, OrdDtlBlockPair>	OrdDtlBlockFilesMap;

	/**
	 * @brief 实时委托队列数据块对
	 * @details 存储实时委托队列数据的内存映射文件和数据块指针
	 */
	typedef struct _OdrQueBlockPair
	{
		RTOrdQueBlock*	_block;		///< 委托队列数据块指针
		BoostMFPtr		_file;		///< 内存映射文件指针
		uint64_t		_last_cap;	///< 上次容量

		std::shared_ptr< std::ofstream>	_fstream;	///< 文件流指针

		/**
		 * @brief 构造函数
		 */
		_OdrQueBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} OrdQueBlockPair;
	typedef wt_hashmap<std::string, OrdQueBlockPair>	OrdQueBlockFilesMap;

	RTKBlockFilesMap	_rt_min1_map;
	RTKBlockFilesMap	_rt_min5_map;

	TBlockFilesMap		_rt_tick_map;
	TransBlockFilesMap	_rt_trans_map;
	OrdDtlBlockFilesMap	_rt_orddtl_map;
	OrdQueBlockFilesMap	_rt_ordque_map;

	/**
	 * @brief 历史Tick数据块对
	 * @details 存储历史Tick数据的数据块指针和缓冲区
	 */
	typedef struct _HisTBlockPair
	{
		HisTickBlock*	_block;		///< 历史Tick数据块指针
		uint64_t		_date;		///< 日期
		std::string		_buffer;	///< 数据缓冲区

		/**
		 * @brief 构造函数
		 */
		_HisTBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisTBlockPair;

	typedef wt_hashmap<std::string, HisTBlockPair>	HisTickBlockMap;

	/**
	 * @brief 历史成交数据块对
	 * @details 存储历史成交数据的数据块指针和缓冲区
	 */
	typedef struct _HisTransBlockPair
	{
		HisTransBlock*	_block;		///< 历史成交数据块指针
		uint64_t		_date;		///< 日期
		std::string		_buffer;	///< 数据缓冲区

		/**
		 * @brief 构造函数
		 */
		_HisTransBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisTransBlockPair;

	typedef wt_hashmap<std::string, HisTransBlockPair>	HisTransBlockMap;

	/**
	 * @brief 历史委托明细数据块对
	 * @details 存储历史委托明细数据的数据块指针和缓冲区
	 */
	typedef struct _HisOrdDtlBlockPair
	{
		HisOrdDtlBlock*	_block;		///< 历史委托明细数据块指针
		uint64_t		_date;		///< 日期
		std::string		_buffer;	///< 数据缓冲区

		/**
		 * @brief 构造函数
		 */
		_HisOrdDtlBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisOrdDtlBlockPair;

	typedef wt_hashmap<std::string, HisOrdDtlBlockPair>	HisOrdDtlBlockMap;

	/**
	 * @brief 历史委托队列数据块对
	 * @details 存储历史委托队列数据的数据块指针和缓冲区
	 */
	typedef struct _HisOrdQueBlockPair
	{
		HisOrdQueBlock*	_block;		///< 历史委托队列数据块指针
		uint64_t		_date;		///< 日期
		std::string		_buffer;	///< 数据缓冲区

		/**
		 * @brief 构造函数
		 */
		_HisOrdQueBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisOrdQueBlockPair;

	typedef wt_hashmap<std::string, HisOrdQueBlockPair>	HisOrdQueBlockMap;

	HisTickBlockMap		_his_tick_map;
	HisOrdDtlBlockMap	_his_orddtl_map;
	HisOrdQueBlockMap	_his_ordque_map;
	HisTransBlockMap	_his_trans_map;

private:
	/**
	 * @brief 获取实时K线数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期
	 * @return 实时K线数据块对指针
	 */
	RTKlineBlockPair* getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period);

	/**
	 * @brief 获取实时Tick数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return 实时Tick数据块对指针
	 */
	TickBlockPair* getRTTickBlock(const char* exchg, const char* code);

	/**
	 * @brief 获取实时委托队列数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return 实时委托队列数据块对指针
	 */
	OrdQueBlockPair* getRTOrdQueBlock(const char* exchg, const char* code);

	/**
	 * @brief 获取实时委托明细数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return 实时委托明细数据块对指针
	 */
	OrdDtlBlockPair* getRTOrdDtlBlock(const char* exchg, const char* code);

	/**
	 * @brief 获取实时成交数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return 实时成交数据块对指针
	 */
	TransBlockPair* getRTTransBlock(const char* exchg, const char* code);

	/**
	 * @brief 缓存集成的K线数据
	 * @param codeInfo 合约信息
	 * @param key 缓存键
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @return 是否成功
	 */
	bool	cacheIntegratedBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);

	/**
	 * @brief 缓存复权后的股票K线数据
	 * @param codeInfo 合约信息
	 * @param key 缓存键
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @return 是否成功
	 */
	bool	cacheAdjustedStkBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);

	/**
	 * @brief 将历史数据放入缓存
	 * @details 从文件中读取历史K线数据并放入缓存
	 * @param codeInfo 合约信息
	 * @param key 缓存键
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @return 是否成功
	 */
	bool	cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);

	/**
	 * @brief 从数据加载器中缓存最终K线数据
	 * @param codeInfo 合约信息
	 * @param key 缓存键
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @return 是否成功
	 */
	bool	cacheFinalBarsFromLoader(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);

	/**
	 * @brief 从文件中加载股票复权因子
	 * @param adjfile 复权因子文件路径
	 * @return 是否成功
	 */
	bool	loadStkAdjFactorsFromFile(const char* adjfile);

	/**
	 * @brief 从数据加载器中加载股票复权因子
	 * @return 是否成功
	 */
	bool	loadStkAdjFactorsFromLoader();

public:
	/**
	 * @brief 初始化数据读取器
	 * @param cfg 配置项
	 * @param sink 数据读取器回调接口
	 * @param loader 历史数据加载器，默认为NULL
	 */
	virtual void init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) override;

	/**
	 * @brief 分钟结束回调
	 * @param uDate 日期，格式YYYYMMDD
	 * @param uTime 时间，格式HHMMSS或HHMM
	 * @param endTDate 结束交易日，默认为0
	 */
	virtual void onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override;

	/**
	 * @brief 读取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return Tick数据切片指针
	 */
	virtual WTSTickSlice*	readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 读取委托明细数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 委托明细数据切片指针
	 */
	virtual WTSOrdDtlSlice*	readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 读取委托队列数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 委托队列数据切片指针
	 */
	virtual WTSOrdQueSlice*	readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 读取成交数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return 成交数据切片指针
	 */
	virtual WTSTransSlice*	readTransSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 读取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param count 读取数量
	 * @param etime 结束时间，默认为0表示当前时间
	 * @return K线数据切片指针
	 */
	virtual WTSKlineSlice*	readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 根据日期获取复权因子
	 * @param stdCode 标准化合约代码
	 * @param date 日期，默认为0表示当前日期
	 * @return 复权因子
	 */
	virtual double getAdjFactorByDate(const char* stdCode, uint32_t date = 0) override;

	/**
	 * @brief 获取复权标志
	 * @return 复权标志，采用位运算表示，1|表示成交量复权，2表示成交额复权，4表示总持复权
	 */
	virtual uint32_t getAdjustingFlag() override { return _adjust_flag; }

private:
	std::string		_rt_dir;			///< 实时数据目录
	std::string		_his_dir;			///< 历史数据目录
	IBaseDataMgr*	_base_data_mgr;	///< 基础数据管理器
	IHotMgr*		_hot_mgr;			///< 主力合约管理器

	//By Wesley @ 2022.08.15
	//复权标记，采用位运算表示，1|2|4,1表示成交量复权，2表示成交额复权，4表示总持复权，其他待定
	uint32_t		_adjust_flag;		///< 复权标记

	/**
	 * @brief K线列表结构体
	 * @details 存储K线数据及相关信息的结构体
	 * @note 该结构体用于存储K线数据，包括交易所代码、合约代码、K线周期、实时数据游标、原始代码、K线数据数组和复权因子
	 */
	typedef struct _BarsList
	{
		std::string		_exchg;			///< 交易所代码
		std::string		_code;			///< 合约代码
		WTSKlinePeriod	_period;		///< K线周期
		uint32_t		_rt_cursor;		///< 实时数据游标
		std::string		_raw_code;		///< 原始代码

		std::vector<WTSBarStruct>	_bars;	///< K线数据数组
		double			_factor;		///< 复权因子

		/**
		 * @brief 构造函数
		 */
		_BarsList() :_rt_cursor(UINT_MAX), _factor(DBL_MAX){}
	} BarsList;

	typedef wt_hashmap<std::string, BarsList> BarsCache;
	BarsCache	_bars_cache;

	uint64_t	_last_time;

	/**
	 * @brief 除权因子结构体
	 * @details 存储股票除权信息的结构体
	 */
	typedef struct _AdjFactor
	{
		uint32_t	_date;		///< 日期
		double		_factor;	///< 复权因子
	} AdjFactor;
	typedef std::vector<AdjFactor> AdjFactorList;
	typedef wt_hashmap<std::string, AdjFactorList>	AdjFactorMap;
	AdjFactorMap	_adj_factors;

	/**
	 * @brief 获取复权因子列表
	 * @param code 合约代码
	 * @param exchg 交易所代码
	 * @param pid 品种ID
	 * @return 复权因子列表的常量引用
	 */
	const AdjFactorList& getAdjFactors(const char* code, const char* exchg, const char* pid);
	
};

NS_WTP_END