#pragma once
#include "DataDefineAD.h"

#include "../WTSUtils/WtLMDB.hpp"

#include "../Includes/FasterDefs.h"
#include "../Includes/IDataWriter.h"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"

#include <queue>

typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

/**
 * @namespace WTP
 * @brief WonderTrader平台的主命名空间
 */
NS_WTP_BEGIN
/**
 * @class WTSContractInfo
 * @brief 合约信息类，用于存储和管理交易合约的基本信息
 */
class WTSContractInfo;
NS_WTP_END

USING_NS_WTP;

/**
 * @class WtDataWriterAD
 * @brief 基于LMDB的数据存储引擎，负责行情数据的写入和缓存管理
 * @details 该类实现了IDataWriter接口，用于将Tick数据和K线数据写入LMDB数据库，
 *          并管理相关的内存缓存，支持多种周期的数据存储
 */
class WtDataWriterAD : public IDataWriter
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化数据写入器的基本参数和默认设置
	 */
	WtDataWriterAD();

	/**
	 * @brief 析构函数
	 * @details 释放数据写入器占用的资源
	 */
	~WtDataWriterAD();	

private:
	/**
	 * @brief 调整实时数据块的大小
	 * @tparam HeaderType 数据块头部类型
	 * @tparam T 数据项类型
	 * @param mfPtr 内存映射文件指针
	 * @param nCount 需要的数据项数量
	 * @return void* 调整后的内存块指针
	 * @details 当缓存容量不足时，该方法会重新分配内存并调整映射文件的大小
	 */
	template<typename HeaderType, typename T>
	void* resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount);

public:
	/**
	 * @brief 初始化数据写入器
	 * @param params 初始化参数，包含数据存储路径、缓存设置等
	 * @param sink 数据写入器的回调接口，用于输出日志和获取基础数据管理器
	 * @return bool 初始化是否成功
	 * @details 该方法负责创建必要的目录结构、加载缓存文件并准备数据存储环境
	 */
	virtual bool init(WTSVariant* params, IDataWriterSink* sink) override;

	/**
	 * @brief 释放数据写入器资源
	 * @details 终止异步任务处理线程并释放相关资源
	 */
	virtual void release() override;

	/**
	 * @brief 写入Tick数据
	 * @param curTick 当前的Tick数据
	 * @param procFlag 处理标志，控制数据处理方式（0=直接写入，1=预处理，2=自动累加）
	 * @return bool 写入是否成功
	 * @details 将Tick数据写入缓存和数据库，并触发相关的K线生成和存储逻辑
	 */
	virtual bool writeTick(WTSTickData* curTick, uint32_t procFlag) override;

	/**
	 * @brief 获取当前的Tick数据
	 * @param code 合约代码
	 * @param exchg 交易所代码，默认为空字符串
	 * @return WTSTickData* 当前的Tick数据指针，如果不存在则返回NULL
	 * @details 从缓存中获取指定合约的最新Tick数据
	 */
	virtual WTSTickData* getCurTick(const char* code, const char* exchg = "") override;

private:
	/**
	 * @brief 基础数据管理器指针
	 * @details 用于获取合约信息、交易时间等基础数据
	 */
	IBaseDataMgr*		_bd_mgr;

	/**
	 * @brief Tick数据缓存相关成员
	 * @details 用于存储和管理实时Tick数据的内存缓存
	 */
	//Tick缓存
	/**
	 * @brief Tick缓存互斥锁
	 * @details 保护Tick缓存在多线程环境下的安全访问
	 */
	StdUniqueMutex	_mtx_tick_cache;

	/**
	 * @brief Tick缓存文件名
	 * @details 存储Tick数据缓存的文件名
	 */
	std::string		_cache_file_tick;

	/**
	 * @brief Tick缓存索引映射
	 * @details 将合约标识映射到缓存数组中的索引位置
	 */
	wt_hashmap<std::string, uint32_t> _tick_cache_idx;

	/**
	 * @brief Tick缓存文件指针
	 * @details 指向内存映射的Tick缓存文件
	 */
	BoostMFPtr		_tick_cache_file;

	/**
	 * @brief Tick缓存块指针
	 * @details 指向内存中的Tick数据缓存块
	 */
	RTTickCache*	_tick_cache_block;

	/**
	 * @brief K线数据缓存相关成员
	 * @details 用于存储和管理不同周期的K线数据缓存
	 */
	//m1缓存
	/**
	 * @brief 实时K线缓存包装器结构体
	 * @details 封装了K线数据缓存的相关属性和方法
	 */
	typedef struct _RTBarCacheWrapper
	{
		/**
		 * @brief 互斥锁
		 * @details 保护K线缓存在多线程环境下的安全访问
		 */
		StdUniqueMutex	_mtx;

		/**
		 * @brief 缓存文件名
		 * @details 存储K线数据缓存的文件名
		 */
		std::string		_filename;

		/**
		 * @brief 缓存索引映射
		 * @details 将合约标识映射到缓存数组中的索引位置
		 */
		wt_hashmap<std::string, uint32_t> _idx;

		/**
		 * @brief 缓存文件指针
		 * @details 指向内存映射的K线缓存文件
		 */
		BoostMFPtr		_file_ptr;

		/**
		 * @brief 缓存块指针
		 * @details 指向内存中的K线数据缓存块
		 */
		RTBarCache*		_cache_block;

		/**
		 * @brief 构造函数
		 * @details 初始化缓存块和文件指针为NULL
		 */
		_RTBarCacheWrapper():_cache_block(NULL),_file_ptr(NULL){}

		/**
		 * @brief 检查缓存是否为空
		 * @return bool 如果缓存块为NULL则返回true，否则返回false
		 */
		inline bool empty() const { return _cache_block == NULL; }
	} RTBarCacheWrapper;

	/**
	 * @brief 1分钟K线缓存
	 * @details 存储和管理实时1分钟K线数据
	 */
	RTBarCacheWrapper _m1_cache;

	/**
	 * @brief 5分钟K线缓存
	 * @details 存储和管理实时5分钟K线数据
	 */
	RTBarCacheWrapper _m5_cache;

	/**
	 * @brief 日K线缓存
	 * @details 存储和管理实时日K线数据
	 */
	RTBarCacheWrapper _d1_cache;

	/**
	 * @brief 任务信息类型定义
	 * @details 定义了无参数无返回值的函数对象类型，用于异步任务处理
	 */
	typedef std::function<void()> TaskInfo;

	/**
	 * @brief 任务队列
	 * @details 存储待处理的异步任务
	 */
	std::queue<TaskInfo>	_tasks;

	/**
	 * @brief 任务处理线程指针
	 * @details 指向处理异步任务的工作线程
	 */
	StdThreadPtr			_task_thrd;

	/**
	 * @brief 任务队列互斥锁
	 * @details 保护任务队列在多线程环境下的安全访问
	 */
	StdUniqueMutex			_task_mtx;

	/**
	 * @brief 任务条件变量
	 * @details 用于线程同步，当有新任务时通知处理线程
	 */
	StdCondVariable			_task_cond;

	/**
	 * @brief 异步任务标志
	 * @details 标记是否启用异步任务处理模式
	 */
	bool					_async_task;

	/**
	 * @brief 数据存储基础目录
	 * @details 用于存储缓存文件和数据库文件的根目录
	 */
	std::string		_base_dir;

	/**
	 * @brief 日志分组大小
	 * @details 控制日志输出的分组大小，用于性能统计
	 */
	uint32_t		_log_group_size;

	/**
	 * @brief 终止标志
	 * @details 标记数据写入器是否已终止运行
	 */
	bool			_terminated;

	/**
	 * @brief 禁用Tick数据存储标志
	 * @details 如果为true，则不存储Tick数据
	 */
	bool			_disable_tick;

	/**
	 * @brief 禁用1分钟K线存储标志
	 * @details 如果为true，则不存储1分钟K线数据
	 */
	bool			_disable_min1;

	/**
	 * @brief 禁用5分钟K线存储标志
	 * @details 如果为true，则不存储5分钟K线数据
	 */
	bool			_disable_min5;

	/**
	 * @brief 禁用日K线存储标志
	 * @details 如果为true，则不存储日K线数据
	 */
	bool			_disable_day;

	/**
	 * @brief Tick数据库映射大小
	 * @details 控制LMDB中Tick数据库的内存映射大小，单位为字节
	 */
	uint32_t		_tick_mapsize;

	/**
	 * @brief K线数据库映射大小
	 * @details 控制LMDB中K线数据库的内存映射大小，单位为字节
	 */
	uint32_t		_kline_mapsize;

private:
	//////////////////////////////////////////////////////////////////////////
	/**
	 * @brief LMDB数据库定义
	 * @details 该部分定义了LMDB数据库的结构和组织方式
	 * 	K线数据，按照每个市场m1/m5/d1三个周期一共三个数据库，路径如./m1/CFFEX
	 * 	Tick数据，每个合约一个数据库，路径如./ticks/CFFEX/IF2101
	 */
	
	/**
	 * @brief WtLMDB智能指针类型定义
	 * @details 使用智能指针管理LMDB数据库实例，自动处理内存释放
	 */
	typedef std::shared_ptr<WtLMDB> WtLMDBPtr;

	/**
	 * @brief LMDB数据库映射类型定义
	 * @details 将字符串键映射到LMDB数据库实例的哈希表
	 */
	typedef wt_hashmap<std::string, WtLMDBPtr> WtLMDBMap;

	/**
	 * @brief 1分钟K线数据库映射
	 * @details 按交易所组织的不同1分钟K线数据库集合
	 */
	WtLMDBMap	_exchg_m1_dbs;

	/**
	 * @brief 5分钟K线数据库映射
	 * @details 按交易所组织的不同5分钟K线数据库集合
	 */
	WtLMDBMap	_exchg_m5_dbs;

	/**
	 * @brief 日K线数据库映射
	 * @details 按交易所组织的不同日K线数据库集合
	 */
	WtLMDBMap	_exchg_d1_dbs;

	/**
	 * @brief Tick数据库映射
	 * @details 用exchg.code作为key，如BINANCE.BTCUSDT，每个合约对应一个数据库
	 */
	WtLMDBMap	_tick_dbs;

	/**
	 * @brief 获取K线数据库
	 * @param exchg 交易所代码
	 * @param period K线周期类型（分钟、1分钟、5分钟等）
	 * @return WtLMDBPtr 数据库实例的智能指针
	 * @details 根据交易所和周期获取或创建相应的K线数据库
	 */
	WtLMDBPtr	get_k_db(const char* exchg, WTSKlinePeriod period);

	/**
	 * @brief 获取Tick数据库
	 * @param exchg 交易所代码
	 * @param code 合约代码
	 * @return WtLMDBPtr 数据库实例的智能指针
	 * @details 根据交易所和合约代码获取或创建相应的Tick数据库
	 */
	WtLMDBPtr	get_t_db(const char* exchg, const char* code);

private:
	/**
	 * @brief 加载缓存
	 * @details 加载或创建Tick和K线缓存文件，并初始化相关的内存结构
	 */
	void loadCache();

	/**
	 * @brief 更新Tick缓存
	 * @param ct 合约信息指针
	 * @param curTick 当前的Tick数据
	 * @param procFlag 处理标志，控制数据处理方式
	 * @return bool 更新是否成功
	 * @details 将新的Tick数据更新到内存缓存中，并根据处理标志进行不同的处理
	 */
	bool updateTickCache(WTSContractInfo* ct, WTSTickData* curTick, uint32_t procFlag);

	/**
	 * @brief 更新K线缓存
	 * @param ct 合约信息指针
	 * @param curTick 当前的Tick数据
	 * @details 根据Tick数据更新不同周期的K线缓存，包括日、1分钟、5分钟K线
	 */
	void updateBarCache(WTSContractInfo* ct, WTSTickData* curTick);

	/**
	 * @brief 将Tick数据写入数据库
	 * @param ct 合约信息指针
	 * @param curTick 当前的Tick数据
	 * @details 将Tick数据写入到LMDB数据库中进行持久化存储
	 */
	void pipeToTicks(WTSContractInfo* ct, WTSTickData* curTick);

	/**
	 * @brief 将日K线数据写入数据库
	 * @param ct 合约信息指针
	 * @param bar 待写入的K线数据结构
	 * @details 将日K线数据写入到LMDB数据库中进行持久化存储
	 */
	void pipeToDayBars(WTSContractInfo* ct, const WTSBarStruct& bar);

	/**
	 * @brief 将1分钟K线数据写入数据库
	 * @param ct 合约信息指针
	 * @param bar 待写入的K线数据结构
	 * @details 将1分钟K线数据写入到LMDB数据库中进行持久化存储
	 */
	void pipeToM1Bars(WTSContractInfo* ct, const WTSBarStruct& bar);

	/**
	 * @brief 将5分钟K线数据写入数据库
	 * @param ct 合约信息指针
	 * @param bar 待写入的K线数据结构
	 * @details 将5分钟K线数据写入到LMDB数据库中进行持久化存储
	 */
	void pipeToM5Bars(WTSContractInfo* ct, const WTSBarStruct& bar);

	/**
	 * @brief 添加异步任务
	 * @param task 任务函数对象
	 * @details 将任务添加到任务队列中，并通知处理线程执行
	 */
	void pushTask(TaskInfo task);
};

