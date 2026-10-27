/*!\n 
* \brief WonderTrader数据随机访问器头文件\n 
* \author Wesley\n * \date 2020/03/30\n 
* \details 该文件定义了WonderTrader数据随机访问器的实现类WtRdmDtReader，\n 
*          提供对各种类型历史和实时行情数据的随机访问功能\n */

#pragma once
#include <string>
#include <stdint.h>
#include <unordered_map>

#include "DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IRdmDtReader.h"

#include "../Share/BoostMappingFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/fmtlib.h"

NS_WTP_BEGIN
class WTSVariant;
class WTSTickSlice;
class WTSKlineSlice;
class WTSOrdDtlSlice;
class WTSOrdQueSlice;
class WTSTransSlice;
class WTSArray;

class IBaseDataMgr;
class IHotMgr;
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

/*!\n * \brief 数据随机访问器类\n * \details 实现IRdmDtReader接口，提供对各种类型历史和实时行情数据的随机访问功能，\n *          包括K线数据、Tick数据、成交明细、委托明细、委托队列等\n */
class WtRdmDtReader : public IRdmDtReader
{
public:
	//! \brief 构造函数
	WtRdmDtReader();
	
	//! \brief 析构函数
	virtual ~WtRdmDtReader();

private:
	/**
	 * @brief 实时K线数据块对结构体
	 * @details 用于管理内存映射文件中存储的实时K线数据
	 */
	typedef struct _RTKBlockPair
	{
		StdUniqueMutex*	_mtx;        ///< 互斥锁，用于线程安全访问
		RTKlineBlock*	_block;      ///< 实时K线数据块指针
		BoostMFPtr		_file;       ///< 内存映射文件指针
		uint64_t		_last_cap;    ///< 最后容量记录
		uint64_t		_last_time;   ///< 最后访问时间

		_RTKBlockPair()
		{
			_mtx = new StdUniqueMutex();
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
			_last_time = 0;
		}
		~_RTKBlockPair() { delete _mtx; }

	} RTKlineBlockPair;
	typedef std::unordered_map<std::string, RTKlineBlockPair>	RTKBlockFilesMap;

	/**
	 * @brief 实时Tick数据块对结构体
	 * @details 用于管理内存映射文件中存储的实时Tick数据
	 */
	typedef struct _TBlockPair
	{
		StdUniqueMutex*	_mtx;        ///< 互斥锁，用于线程安全访问
		RTTickBlock*	_block;      ///< 实时Tick数据块指针
		BoostMFPtr		_file;       ///< 内存映射文件指针
		uint64_t		_last_cap;    ///< 最后容量记录
		uint64_t		_last_time;   ///< 最后访问时间

		_TBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
			_last_time = 0;
			_mtx = new StdUniqueMutex();
		}
		~_TBlockPair() { delete _mtx; }
	} TickBlockPair;
	typedef std::unordered_map<std::string, TickBlockPair>	TBlockFilesMap;

	/**
	 * @brief 实时成交数据块对结构体
	 * @details 用于管理内存映射文件中存储的实时成交明细数据
	 */
	typedef struct _TransBlockPair
	{
		StdUniqueMutex*	_mtx;        ///< 互斥锁，用于线程安全访问
		RTTransBlock*	_block;      ///< 实时成交数据块指针
		BoostMFPtr		_file;       ///< 内存映射文件指针
		uint64_t		_last_cap;    ///< 最后容量记录
		uint64_t		_last_time;   ///< 最后访问时间

		_TransBlockPair()
		{
			_mtx = new StdUniqueMutex();
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
			_last_time = 0;
		}
		~_TransBlockPair() { delete _mtx; }
	} TransBlockPair;
	typedef std::unordered_map<std::string, TransBlockPair>	TransBlockFilesMap;

	/**
	 * @brief 实时委托明细数据块对结构体
	 * @details 用于管理内存映射文件中存储的实时委托明细数据
	 */
	typedef struct _OdeDtlBlockPair
	{
		StdUniqueMutex*	_mtx;        ///< 互斥锁，用于线程安全访问
		RTOrdDtlBlock*	_block;      ///< 实时委托明细数据块指针
		BoostMFPtr		_file;       ///< 内存映射文件指针
		uint64_t		_last_cap;    ///< 最后容量记录
		uint64_t		_last_time;   ///< 最后访问时间

		_OdeDtlBlockPair()
		{
			_mtx = new StdUniqueMutex();
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
			_last_time = 0;
		}
		~_OdeDtlBlockPair() { delete _mtx; }
	} OrdDtlBlockPair;
	typedef std::unordered_map<std::string, OrdDtlBlockPair>	OrdDtlBlockFilesMap;

	/**
	 * @brief 实时委托队列数据块对结构体
	 * @details 用于管理内存映射文件中存储的实时委托队列数据
	 */
	typedef struct _OdeQueBlockPair
	{
		StdUniqueMutex*	_mtx;        ///< 互斥锁，用于线程安全访问
		RTOrdQueBlock*	_block;      ///< 实时委托队列数据块指针
		BoostMFPtr		_file;       ///< 内存映射文件指针
		uint64_t		_last_cap;    ///< 最后容量记录
		uint64_t		_last_time;   ///< 最后访问时间

		_OdeQueBlockPair()
		{
			_mtx = new StdUniqueMutex();
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
			_last_time = 0;
		}
		~_OdeQueBlockPair() { delete _mtx; }
	} OrdQueBlockPair;
	typedef std::unordered_map<std::string, OrdQueBlockPair>	OrdQueBlockFilesMap;

	RTKBlockFilesMap	_rt_min1_map;    ///< 1分钟实时K线数据映射
	RTKBlockFilesMap	_rt_min5_map;    ///< 5分钟实时K线数据映射

	TBlockFilesMap		_rt_tick_map;    ///< 实时Tick数据映射
	TransBlockFilesMap	_rt_trans_map;   ///< 实时成交数据映射
	OrdDtlBlockFilesMap	_rt_orddtl_map;  ///< 实时委托明细映射
	OrdQueBlockFilesMap	_rt_ordque_map;  ///< 实时委托队列映射

	/**
	 * @brief 历史Tick数据块对结构体
	 * @details 用于管理内存中缓存的历史Tick数据
	 */
	typedef struct _HisTBlockPair
	{
		HisTickBlock*	_block;      ///< 历史Tick数据块指针
		uint64_t		_date;       ///< 数据日期
		std::string		_buffer;     ///< 数据缓冲区

		/**
		 * @brief 构造函数，初始化历史Tick数据块
		 */
		_HisTBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisTBlockPair;

	typedef std::unordered_map<std::string, HisTBlockPair>	HisTickBlockMap;

	/**
	 * @brief 历史成交数据块对结构体
	 * @details 用于管理内存中缓存的历史成交数据
	 */
	typedef struct _HisTransBlockPair
	{
		HisTransBlock*	_block;      ///< 历史成交数据块指针
		uint64_t		_date;       ///< 数据日期
		std::string		_buffer;     ///< 数据缓冲区

		/**
		 * @brief 构造函数，初始化历史成交数据块
		 */
		_HisTransBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisTransBlockPair;

	typedef std::unordered_map<std::string, HisTransBlockPair>	HisTransBlockMap;

	/**
	 * @brief 历史委托明细数据块对结构体
	 * @details 用于管理内存中缓存的历史委托明细数据
	 */
	typedef struct _HisOrdDtlBlockPair
	{
		HisOrdDtlBlock*	_block;      ///< 历史委托明细数据块指针
		uint64_t		_date;       ///< 数据日期
		std::string		_buffer;     ///< 数据缓冲区

		/**
		 * @brief 构造函数，初始化历史委托明细数据块
		 */
		_HisOrdDtlBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisOrdDtlBlockPair;

	typedef std::unordered_map<std::string, HisOrdDtlBlockPair>	HisOrdDtlBlockMap;

	/**
	 * @brief 历史委托队列数据块对结构体
	 * @details 用于管理内存中缓存的历史委托队列数据
	 */
	typedef struct _HisOrdQueBlockPair
	{
		HisOrdQueBlock*	_block;      ///< 历史委托队列数据块指针
		uint64_t		_date;       ///< 数据日期
		std::string		_buffer;     ///< 数据缓冲区

		/**
		 * @brief 构造函数，初始化历史委托队列数据块
		 */
		_HisOrdQueBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisOrdQueBlockPair;

	typedef std::unordered_map<std::string, HisOrdQueBlockPair>	HisOrdQueBlockMap;

	HisTickBlockMap		_his_tick_map;    ///< 历史Tick数据映射
	HisOrdDtlBlockMap	_his_orddtl_map;  ///< 历史委托明细映射
	HisOrdQueBlockMap	_his_ordque_map;  ///< 历史委托队列映射
	HisTransBlockMap	_his_trans_map;   ///< 历史成交数据映射

private:
	/**
	 * @brief 获取实时K线数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @param period K线周期类型
	 * @return RTKlineBlockPair* 实时K线数据块对象指针
	 * 
	 * @details 根据合约和周期找到或创建实时K线数据块，用于后续访问
	 */
	RTKlineBlockPair* getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period);
	/**
	 * @brief 获取实时Tick数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return TickBlockPair* 实时Tick数据块对象指针
	 * 
	 * @details 根据合约找到或创建实时Tick数据块，用于后续访问
	 */
	TickBlockPair* getRTTickBlock(const char* exchg, const char* code);
	/**
	 * @brief 获取实时委托队列数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return OrdQueBlockPair* 实时委托队列数据块对象指针
	 * 
	 * @details 根据合约找到或创建实时委托队列数据块，用于后续访问
	 */
	OrdQueBlockPair* getRTOrdQueBlock(const char* exchg, const char* code);
	/**
	 * @brief 获取实时委托明细数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return OrdDtlBlockPair* 实时委托明细数据块对象指针
	 * 
	 * @details 根据合约找到或创建实时委托明细数据块，用于后续访问
	 */
	OrdDtlBlockPair* getRTOrdDtlBlock(const char* exchg, const char* code);
	/**
	 * @brief 获取实时成交数据块
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return TransBlockPair* 实时成交数据块对象指针
	 * 
	 * @details 根据合约找到或创建实时成交数据块，用于后续访问
	 */
	TransBlockPair* getRTTransBlock(const char* exchg, const char* code);

	/**
	 * @brief 将历史数据放入缓存
	 * @param codeInfo 合约信息指针
	 * @param key 数据缓存键
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @return bool 缓存是否成功
	 * 
	 * @details 根据给定的合约信息和周期，从文件读取历史K线数据并加载到内存缓存中
	 */
	bool		cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);

	/**
	 * @brief 根据时间范围从缓存中读取K线数据
	 * @param key 数据缓存键
	 * @param stime 开始时间
	 * @param etime 结束时间
	 * @param ayBars K线数据存储数组，用于存放返回结果
	 * @param isDay 是否是日线数据，默认为false
	 * @return uint32_t 返回读取到的K线条数
	 * 
	 * @details 根据时间范围，从缓存中提取指定区间的K线数据，并存入参数提供的数组中
	 */
	uint32_t		readBarsFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, std::vector<WTSBarStruct>& ayBars, bool isDay = false);
	/**
	 * @brief 根据时间范围从缓存中获取K线数据指针
	 * @param key 数据缓存键
	 * @param stime 开始时间
	 * @param etime 结束时间
	 * @param count 数据条数，通过引用方式返回
	 * @param isDay 是否是日线数据，默认为false
	 * @return WTSBarStruct* 返回K线数据数组指针
	 * 
	 * @details 根据时间范围，从缓存中获取指定区间的K线数据的指针，不复制数据，提高效率
	 */
	WTSBarStruct*	indexBarFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, uint32_t& count, bool isDay = false);

	/**
	 * @brief 根据数量从缓存中获取K线数据指针
	 * @param key 数据缓存键
	 * @param etime 结束时间
	 * @param count 请求的数据条数如果条数不足，将返回实际读取到的条数
	 * @param isDay 是否是日线数据，默认为false
	 * @return WTSBarStruct* 返回K线数据数组指针
	 * 
	 * @details 从缓存中向前读取指定数量的K线数据，不复制数据，提高效率
	 */
	WTSBarStruct*	indexBarFromCacheByCount(const std::string& key, uint64_t etime, uint32_t& count, bool isDay = false);

	/**
	 * @brief 从文件加载股票除权因子
	 * @param adjfile 除权因子文件路径
	 * @return bool 返回是否加载成功
	 * 
	 * @details 从指定文件路径读取股票的除权因子列表，用于实现前复权或者后复权的功能
	 */
	bool	loadStkAdjFactorsFromFile(const char* adjfile);
	

//////////////////////////////////////////////////////////////////////////
//IRdmDtReader
public:
	/**
	 * @brief 初始化随机数据访问器
	 * @param cfg 配置项
	 * @param sink 数据存储回调接口
	 * 
	 * @details 根据给定的配置对象初始化数据存储器，设置数据路径和其他相关参数
	 */
	virtual void init(WTSVariant* cfg, IRdmDtReaderSink* sink);

	/**
	 * @brief 按时间范围读取委托明细数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 起始时间
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSOrdDtlSlice* 委托明细数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的委托明细数据
	 */
	virtual WTSOrdDtlSlice*	readOrdDtlSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override;
	/**
	 * @brief 按时间范围读取委托队列数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 起始时间
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSOrdQueSlice* 委托队列数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的委托队列数据
	 */
	virtual WTSOrdQueSlice*	readOrdQueSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override;
	/**
	 * @brief 按时间范围读取成交数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 起始时间
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSTransSlice* 成交数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的成交数据
	 */
	virtual WTSTransSlice*	readTransSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override;

	/**
	 * @brief 按时间范围读取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param stime 起始时间
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSTickSlice* Tick数据切片指针
	 * 
	 * @details 根据时间范围获取指定合约的Tick数据
	 */
	virtual WTSTickSlice*	readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override;
	/**
	 * @brief 按时间范围读取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param stime 起始时间
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSKlineSlice* K线数据切片指针
	 * 
	 * @details 根据时间范围和周期获取指定合约的K线数据
	 */
	virtual WTSKlineSlice*	readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime = 0) override;

	/**
	 * @brief 按数量读取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSTickSlice* Tick数据切片指针
	 * 
	 * @details 根据指定数量和截止时间，获取指定合约的Tick数据
	 */
	virtual WTSTickSlice*	readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	/**
	 * @brief 按数量读取K线数据切片
	 * @param stdCode 标准化合约代码
	 * @param period K线周期
	 * @param count 请求的数据条数
	 * @param etime 结束时间，默认为0（表示当前时间）
	 * @return WTSKlineSlice* K线数据切片指针
	 * 
	 * @details 根据指定数量、周期和截止时间，获取指定合约的K线数据
	 */
	virtual WTSKlineSlice*	readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override;

	/**
	 * @brief 按日期读取Tick数据切片
	 * @param stdCode 标准化合约代码
	 * @param uDate 交易日期，默认为0（表示当前日期）
	 * @return WTSTickSlice* Tick数据切片指针
	 * 
	 * @details 根据指定日期，获取指定合约当日的全部Tick数据
	 */
	virtual WTSTickSlice*	readTickSliceByDate(const char* stdCode, uint32_t uDate = 0 ) override;

	/**
	 * @brief 根据日期获取除权因子
	 * @param stdCode 标准化合约代码
	 * @param date 日期，默认为0（表示当前日期）
	 * @return double 除权因子值
	 * 
	 * @details 获取指定日期的股票除权因子，用于实现前复权或者后复权
	 */
	virtual double		getAdjFactorByDate(const char* stdCode, uint32_t date = 0) override;

	/**
	 * @brief 清理内存数据缓存
	 * 
	 * @details 清除所有已缓存的数据，释放相关内存资源，包括K线、Tick、委托全印、委托队列和成交数据等
	 */
	virtual void		clearCache() override;

private:
	std::string		_base_dir;       ///< 数据文件存放的基础目录
	IBaseDataMgr*	_base_data_mgr;   ///< 基础数据管理器指针
	IHotMgr*		_hot_mgr;        ///< 热点合约管理器指针
	StdThreadPtr	_thrd_check;     ///< 检查线稍指针，用于定期检查数据状态
	bool			_stopped;        ///< 线程是否已停止的标志

	typedef struct _BarsList
	{
		std::string		_exchg;       ///< 交易所代码
		std::string		_code;        ///< 合约代码
		WTSKlinePeriod	_period;      ///< K线周期
		std::string		_raw_code;    ///< 原始合约代码
		double			_factor;      ///< 除权因子

		_BarsList():_factor(1.0){}

		std::vector<WTSBarStruct>	_bars;       ///< 历史K线数据缓存
		std::vector<WTSBarStruct>	_rt_bars;    ///< 实时K线数据缓存，如果是后复权，就需要把实时数据拷贝到这里来
	} BarsList;

	/**
	 * @brief 数据缓存映射类型定义
	 */
	typedef std::unordered_map<std::string, BarsList> BarsCache;
	BarsCache	_bars_cache;      ///< K线数据缓存映射

	//除权因子
	typedef struct _AdjFactor
	{
		uint32_t	_date;       ///< 日期
		double		_factor;     ///< 除权因子值
	} AdjFactor;
	/**
	 * @brief 除权因子列表类型定义
	 */
	typedef std::vector<AdjFactor> AdjFactorList;
	
	/**
	 * @brief 除权因子映射类型定义，每个合约对应一个除权因子列表
	 */
	typedef std::unordered_map<std::string, AdjFactorList>	AdjFactorMap;
	AdjFactorMap	_adj_factors;    ///< 除权因子映射表

	inline const AdjFactorList& getAdjFactors(const char* code, const char* exchg, const char* pid)
	{
		thread_local static char key[20] = { 0 };
		fmtutil::format_to(key, "{}.{}.{}", exchg, pid, code);
		return _adj_factors[key];
	}
};

NS_WTP_END