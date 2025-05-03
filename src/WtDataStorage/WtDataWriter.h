/*!
 * \file WtDataWriter.h
 * \brief 数据写入器头文件
 * \author Wesley
 * \date 2020/03/30
 * 
 * \details
 * 该文件定义了数据写入器的接口和实现，用于将行情数据写入到文件中
 */

#pragma once
#include "DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IDataWriter.h"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"
#include "../Share/SpinMutex.hpp"

#include <queue>
#include <map>

/*!
 * \brief Boost内存映射文件智能指针类型
 */
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

NS_WTP_BEGIN
class WTSObject;
class WTSContractInfo;
NS_WTP_END

USING_NS_WTP;

/*!
 * \brief 数据写入器类
 * \details 
 * 实现了IDataWriter接口，负责将行情数据写入到文件中
 * 支持Tick数据、K线数据、委托队列、委托明细和成交明细等多种数据的写入
 * 提供了异步处理和缓存机制，以提高数据处理效率
 */
class WtDataWriter : public IDataWriter
{
public:
	/*!
	 * \brief 构造函数
	 */
	WtDataWriter();

	/*!
	 * \brief 析构函数
	 */
	~WtDataWriter();	

private:
	/*!
	 * \brief 调整实时数据块大小
	 * \tparam HeaderType 数据块头部类型
	 * \tparam T 数据类型
	 * \param mfPtr 内存映射文件指针
	 * \param nCount 需要的数据项数量
	 * \return 调整后的数据块指针
	 * 
	 * \details 根据需要的数据项数量调整内存映射文件的大小
	 */
	template<typename HeaderType, typename T>
	void* resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount);

	/*!
	 * \brief 处理循环
	 * 
	 * \details 异步处理数据的工作线程函数，从任务队列中获取并处理数据
	 */
	void  proc_loop();

	/*!
	 * \brief 检查循环
	 * 
	 * \details 定期检查并处理历史数据的工作线程函数
	 */
	void  check_loop();

	/*!
	 * \brief 将K线数据写入文件
	 * \param ct 合约信息指针
	 * \return 写入的数据数量
	 * 
	 * \details 将合约的K线数据写入到对应的文件中
	 */
	uint32_t  dump_bars_to_file(WTSContractInfo* ct);

	/*!
	 * \brief 通过数据转储器将K线数据写入
	 * \param ct 合约信息指针
	 * \return 写入的数据数量
	 * 
	 * \details 通过外部数据转储器将合约的K线数据写入
	 */
	uint32_t  dump_bars_via_dumper(WTSContractInfo* ct);

private:
	/*!
	 * \brief 写入日线数据
	 * \param ct 合约信息指针
	 * \param newBar 新的日线K线数据
	 * \return 是否成功写入
	 * 
	 * \details 将合约的日线数据写入到文件中
	 */
	bool	dump_day_data(WTSContractInfo* ct, WTSBarStruct* newBar);

	/*!
	 * \brief 处理数据块内容
	 * \param tag 标签，用于日志输出
	 * \param content 数据块内容，传入传出参数
	 * \param isBar 是否为K线数据
	 * \param bKeepHead 是否保留数据块头部
	 * \return 是否成功处理
	 * 
	 * \details 处理数据块的压缩和版本转换
	 */
	bool	proc_block_data(const char* tag, std::string& content, bool isBar, bool bKeepHead = true);

	/*!
	 * \brief 处理Tick数据
	 * \param curTick 当前Tick数据指针
	 * \param procFlag 处理标志
	 * 
	 * \details 处理Tick数据，包括更新缓存、写入文件和生成K线等
	 */
	void	procTick(WTSTickData* curTick, uint32_t procFlag);

	/*!
	 * \brief 处理委托队列数据
	 * \param curOrdQue 当前委托队列数据指针
	 * 
	 * \details 处理委托队列数据，写入到对应的文件中
	 */
	void	procQueue(WTSOrdQueData* curOrdQue);

	/*!
	 * \brief 处理委托明细数据
	 * \param curOrdDetail 当前委托明细数据指针
	 * 
	 * \details 处理委托明细数据，写入到对应的文件中
	 */
	void	procOrder(WTSOrdDtlData* curOrdDetail);

	/*!
	 * \brief 处理成交数据
	 * \param curTrans 当前成交数据指针
	 * 
	 * \details 处理成交数据，写入到对应的文件中
	 */
	void	procTrans(WTSTransData* curTrans);

public:
	/*!
	 * \brief 初始化数据写入器
	 * \param params 配置参数
	 * \param sink 数据写入器回调接口
	 * \return 是否初始化成功
	 * 
	 * \details 根据配置参数初始化数据写入器，设置目录和各种处理标志
	 */
	virtual bool init(WTSVariant* params, IDataWriterSink* sink) override;
	/*!
	 * \brief 释放数据写入器资源
	 * 
	 * \details 清理并释放数据写入器占用的所有资源
	 */
	virtual void release() override;

	/*!
	 * \brief 写入Tick数据
	 * \param curTick 当前Tick数据指针
	 * \param procFlag 处理标志
	 * \return 是否成功写入
	 * 
	 * \details 将Tick数据写入到文件中，并根据标志进行相应的处理
	 */
	virtual bool writeTick(WTSTickData* curTick, uint32_t procFlag) override;

	/*!
	 * \brief 写入委托队列数据
	 * \param curOrdQue 当前委托队列数据指针
	 * \return 是否成功写入
	 * 
	 * \details 将委托队列数据写入到文件中
	 */
	virtual bool writeOrderQueue(WTSOrdQueData* curOrdQue) override;

	/*!
	 * \brief 写入委托明细数据
	 * \param curOrdDetail 当前委托明细数据指针
	 * \return 是否成功写入
	 * 
	 * \details 将委托明细数据写入到文件中
	 */
	virtual bool writeOrderDetail(WTSOrdDtlData* curOrdDetail) override;

	/*!
	 * \brief 写入成交数据
	 * \param curTrans 当前成交数据指针
	 * \return 是否成功写入
	 * 
	 * \details 将成交数据写入到文件中
	 */
	virtual bool writeTransaction(WTSTransData* curTrans) override;

	/*!
	 * \brief 转换历史数据
	 * \param sid 交易日ID
	 * 
	 * \details 将指定交易日的实时数据转换为历史数据
	 */
	virtual void transHisData(const char* sid) override;
	
	/*!
	 * \brief 检查交易日是否已处理
	 * \param sid 交易日ID
	 * \return 是否已处理
	 * 
	 * \details 检查指定交易日的数据是否已经转换处理
	 */
	virtual bool isSessionProceeded(const char* sid) override;

	/*!
	 * \brief 获取当前Tick数据
	 * \param code 合约代码
	 * \param exchg 交易所代码，默认为空
	 * \return 当前Tick数据指针
	 * 
	 * \details 根据合约代码和交易所获取当前缓存的Tick数据
	 */
	virtual WTSTickData* getCurTick(const char* code, const char* exchg = "") override;

private:
	IBaseDataMgr*		_bd_mgr;			///< 基础数据管理器指针

	/*!
	 * \brief K线数据块对结构
	 * \details 用于管理K线数据块和相关的内存映射文件
	 */
	typedef struct _KBlockPair
	{
		RTKlineBlock*	_block;			///< K线数据块指针
		BoostMFPtr		_file;			///< 内存映射文件指针
		SpinMutex		_mutex;			///< 自旋锁，用于线程同步
		uint64_t		_lasttime;		///< 最后更新时间

		/*!
		 * \brief 构造函数
		 * \details 初始化成员变量
		 */
		_KBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}

	} KBlockPair;
	typedef wt_hashmap<std::string, KBlockPair*>	KBlockFilesMap;

	/*!
	 * \brief Tick数据块对结构
	 * \details 用于管理Tick数据块和相关的内存映射文件
	 */
	typedef struct _TickBlockPair
	{
		RTTickBlock*	_block;			///< Tick数据块指针
		BoostMFPtr		_file;			///< 内存映射文件指针
		SpinMutex		_mutex;			///< 自旋锁，用于线程同步
		uint64_t		_lasttime;		///< 最后更新时间

		std::shared_ptr< std::ofstream>	_fstream;	///< 文件流指针，用于写入日志

		/*!
		 * \brief 构造函数
		 * \details 初始化成员变量
		 */
		_TickBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_fstream = NULL;
			_lasttime = 0;
		}
	} TickBlockPair;
	typedef wt_hashmap<std::string, TickBlockPair*>	TickBlockFilesMap;

	/*!
	 * \brief 成交数据块对结构
	 * \details 用于管理成交数据块和相关的内存映射文件
	 */
	typedef struct _TransBlockPair
	{
		RTTransBlock*	_block;			///< 成交数据块指针
		BoostMFPtr		_file;			///< 内存映射文件指针
		SpinMutex		_mutex;			///< 自旋锁，用于线程同步
		uint64_t		_lasttime;		///< 最后更新时间

		/*!
		 * \brief 构造函数
		 * \details 初始化成员变量
		 */
		_TransBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}
	} TransBlockPair;
	typedef wt_hashmap<std::string, TransBlockPair*>	TransBlockFilesMap;

	/*!
	 * \brief 委托明细数据块对结构
	 * \details 用于管理委托明细数据块和相关的内存映射文件
	 */
	typedef struct _OdeDtlBlockPair
	{
		RTOrdDtlBlock*	_block;			///< 委托明细数据块指针
		BoostMFPtr		_file;			///< 内存映射文件指针
		SpinMutex		_mutex;			///< 自旋锁，用于线程同步
		uint64_t		_lasttime;		///< 最后更新时间

		/*!
		 * \brief 构造函数
		 * \details 初始化成员变量
		 */
		_OdeDtlBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}
	} OrdDtlBlockPair;
	typedef wt_hashmap<std::string, OrdDtlBlockPair*>	OrdDtlBlockFilesMap;

	/*!
	 * \brief 委托队列数据块对结构
	 * \details 用于管理委托队列数据块和相关的内存映射文件
	 */
	typedef struct _OdeQueBlockPair
	{
		RTOrdQueBlock*	_block;			///< 委托队列数据块指针
		BoostMFPtr		_file;			///< 内存映射文件指针
		SpinMutex		_mutex;			///< 自旋锁，用于线程同步
		uint64_t		_lasttime;		///< 最后更新时间

		/*!
		 * \brief 构造函数
		 * \details 初始化成员变量
		 */
		_OdeQueBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}
	} OrdQueBlockPair;
	typedef wt_hashmap<std::string, OrdQueBlockPair*>	OrdQueBlockFilesMap;
	

	/*!
	 * \brief 1分钟K线数据块映射表
	 * \details 以合约标识为键，存储实时1分钟K线数据
	 */
	KBlockFilesMap	_rt_min1_blocks;

	/*!
	 * \brief 5分钟K线数据块映射表
	 * \details 以合约标识为键，存储实时5分钟K线数据
	 */
	KBlockFilesMap	_rt_min5_blocks;

	/*!
	 * \brief Tick数据块映射表
	 * \details 以合约标识为键，存储实时Tick行情数据
	 */
	TickBlockFilesMap	_rt_ticks_blocks;

	/*!
	 * \brief 成交数据块映射表
	 * \details 以合约标识为键，存储实时逐笔成交数据
	 */
	TransBlockFilesMap	_rt_trans_blocks;

	/*!
	 * \brief 委托明细数据块映射表
	 * \details 以合约标识为键，存储实时逐笔委托明细数据
	 */
	OrdDtlBlockFilesMap _rt_orddtl_blocks;

	/*!
	 * \brief 委托队列数据块映射表
	 * \details 以合约标识为键，存储实时委托队列数据
	 */
	OrdQueBlockFilesMap _rt_ordque_blocks;

	/*!
	 * \brief Tick缓存锁
	 * \details 用于保护Tick缓存的自旋锁，确保多线程访问安全
	 */
	SpinMutex		_lck_tick_cache;

	/*!
	 * \brief Tick缓存索引表
	 * \details 以合约标识为键，缓存索引为值的哈希映射，用于快速查找指定合约的Tick数据
	 */
	wt_hashmap<std::string, uint32_t> _tick_cache_idx;

	/*!
	 * \brief Tick缓存文件
	 * \details 内存映射文件智能指针，用于存储Tick缓存数据
	 */
	BoostMFPtr		_tick_cache_file;

	/*!
	 * \brief Tick缓存数据块
	 * \details 指向Tick缓存数据块的指针，用于快速访问和更新Tick数据
	 */
	RTTickCache*	_tick_cache_block;

	//typedef std::function<void()> TaskInfo;
	/*!
	 * \brief 任务信息结构
	 * \details 用于存储异步处理的数据任务信息，对齐到64字节以提高缓存效率
	 */
	typedef struct alignas(64) _TaskInfo
	{
		WTSObject*	_obj;			///< 数据对象指针
		uint64_t	_type;			///< 任务类型
		uint32_t	_flag;			///< 处理标志

		/*!
		 * \brief 构造函数
		 * \param data 数据对象指针
		 * \param dtype 数据类型
		 * \param flag 处理标志，默认为0
		 */
		_TaskInfo(WTSObject* data, uint64_t dtype, uint32_t flag = 0);

		/*!
		 * \brief 拷贝构造函数
		 * \param rhs 源任务信息对象
		 */
		_TaskInfo(const _TaskInfo& rhs);

		/*!
		 * \brief 析构函数
		 */
		~_TaskInfo();

	} TaskInfo;
	/*!
	 * \brief 任务队列
	 * \details 存储待处理的数据任务，由任务处理线程异步处理
	 */
	std::queue<TaskInfo>	_tasks;

	/*!
	 * \brief 任务处理线程
	 * \details 用于异步处理任务队列中的数据任务
	 */
	StdThreadPtr			_task_thrd;

	/*!
	 * \brief 任务队列互斥锁
	 * \details 用于保护任务队列的互斥访问
	 */
	StdUniqueMutex			_task_mtx;

	/*!
	 * \brief 任务队列条件变量
	 * \details 用于任务线程的等待和通知机制
	 */
	StdCondVariable			_task_cond;

	/*!
	 * \brief 基础目录
	 * \details 数据存储的根目录路径
	 */
	std::string		_base_dir;

	/*!
	 * \brief 缓存文件路径
	 * \details Tick数据缓存文件的完整路径
	 */
	std::string		_cache_file;

	/*!
	 * \brief 日志分组大小
	 * \details 日志文件按大小分组的参数，单位为KB
	 */
	uint32_t		_log_group_size;

	/*!
	 * \brief 是否异步处理
	 * \details 标记数据处理是否使用异步方式，true表示使用异步处理
	 */
	bool			_async_proc;

	/*!
	 * \brief 处理队列条件变量
	 * \details 用于处理线程的等待和通知机制
	 */
	StdCondVariable	_proc_cond;

	/*!
	 * \brief 处理队列互斥锁
	 * \details 用于保护处理队列的互斥访问
	 */
	StdUniqueMutex	_proc_mtx;

	/*!
	 * \brief 处理队列
	 * \details 存储待处理的文件路径或任务标识
	 */
	std::queue<std::string> _proc_que;

	/*!
	 * \brief 处理线程
	 * \details 用于异步处理数据的工作线程
	 */
	StdThreadPtr	_proc_thrd;

	/*!
	 * \brief 检查线程
	 * \details 用于定期检查并处理历史数据的工作线程
	 */
	StdThreadPtr	_proc_chk;

	/*!
	 * \brief 是否终止标志
	 * \details 标记线程是否应该终止，true表示需要终止线程
	 */
	bool			_terminated;

	/*!
	 * \brief 是否保存Tick日志
	 * \details 标记是否将Tick数据保存到日志文件，true表示保存
	 */
	bool			_save_tick_log;

	/*!
	 * \brief 是否跳过非交易的Tick
	 * \details 标记是否忽略非交易时段的Tick数据，true表示跳过
	 */
	bool			_skip_notrade_tick;

	/*!
	 * \brief 是否跳过非交易的K线
	 * \details 标记是否忽略非交易时段的K线数据，true表示跳过
	 */
	bool			_skip_notrade_bar;

	/*!
	 * \brief 是否禁用历史数据处理
	 * \details 标记是否禁用历史数据的处理功能，true表示禁用
	 */
	bool			_disable_his;

	/*!
	 * \brief 是否禁用Tick数据处理
	 * \details 标记是否禁用Tick数据的处理功能，true表示禁用
	 */
	bool			_disable_tick;

	/*!
	 * \brief 是否禁用1分钟线处理
	 * \details 标记是否禁用1分钟K线的处理功能，true表示禁用
	 */
	bool			_disable_min1;

	/*!
	 * \brief 是否禁用5分钟线处理
	 * \details 标记是否禁用5分钟K线的处理功能，true表示禁用
	 */
	bool			_disable_min5;

	/*!
	 * \brief 是否禁用日线处理
	 * \details 标记是否禁用日线的处理功能，true表示禁用
	 */
	bool			_disable_day;

	/*!
	 * \brief 是否禁用成交数据处理
	 * \details 标记是否禁用逐笔成交数据的处理功能，true表示禁用
	 */
	bool			_disable_trans;

	/*!
	 * \brief 是否禁用委托队列处理
	 * \details 标记是否禁用委托队列数据的处理功能，true表示禁用
	 */
	bool			_disable_ordque;

	/*!
	 * \brief 是否禁用委托明细处理
	 * \details 标记是否禁用委托明细数据的处理功能，true表示禁用
	 */
	bool			_disable_orddtl;

	/*!
	 * \brief 分钟线价格模式
	 * \details 控制分钟线价格的记录模式：
	 *  - 0: 常规模式，只记录OHLC
	 *  - 1: 增强模式，额外记录买卖价，主要用于期权等不活跃品种
	 * \author Wesley @ 2023.05.04
	 */
	uint32_t		_min_price_mode;
	
	std::map<std::string, uint32_t> _proc_date;	///< 已处理的交易日映射表

private:
	/*!
	 * \brief 加载Tick缓存
	 * 
	 * \details 从缓存文件中加载最新的Tick数据到内存中
	 */
	void loadCache();

	/*!
	 * \brief 更新Tick缓存
	 * \param ct 合约信息指针
	 * \param curTick 当前Tick数据指针
	 * \param procFlag 处理标志
	 * \return 是否成功更新缓存
	 * 
	 * \details 将最新的Tick数据更新到缓存中
	 */
	bool updateCache(WTSContractInfo* ct, WTSTickData* curTick, uint32_t procFlag);

	/*!
	 * \brief 将Tick数据写入到实时数据文件
	 * \param ct 合约信息指针
	 * \param curTick 当前Tick数据指针
	 * 
	 * \details 将Tick数据写入到对应的实时数据文件中
	 */
	void pipeToTicks(WTSContractInfo* ct, WTSTickData* curTick);

	/*!
	 * \brief 根据Tick数据生成并写入K线数据
	 * \param ct 合约信息指针
	 * \param curTick 当前Tick数据指针
	 * 
	 * \details 根据Tick数据生成分钟线K线数据，并写入到对应的文件中
	 */
	void pipeToKlines(WTSContractInfo* ct, WTSTickData* curTick);

	/*!
	 * \brief 获取K线数据块
	 * \param ct 合约信息指针
	 * \param period K线周期
	 * \param bAutoCreate 如果不存在是否自动创建，默认为true
	 * \return K线数据块对指针
	 * 
	 * \details 根据合约信息和K线周期获取对应的K线数据块
	 */
	KBlockPair* getKlineBlock(WTSContractInfo* ct, WTSKlinePeriod period, bool bAutoCreate = true);

	/*!
	 * \brief 获取Tick数据块
	 * \param ct 合约信息指针
	 * \param curDate 当前日期
	 * \param bAutoCreate 如果不存在是否自动创建，默认为true
	 * \return Tick数据块对指针
	 * 
	 * \details 根据合约信息和日期获取对应的Tick数据块
	 */
	TickBlockPair* getTickBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);

	/*!
	 * \brief 获取成交数据块
	 * \param ct 合约信息指针
	 * \param curDate 当前日期
	 * \param bAutoCreate 如果不存在是否自动创建，默认为true
	 * \return 成交数据块对指针
	 * 
	 * \details 根据合约信息和日期获取对应的成交数据块
	 */
	TransBlockPair* getTransBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);

	/*!
	 * \brief 获取委托明细数据块
	 * \param ct 合约信息指针
	 * \param curDate 当前日期
	 * \param bAutoCreate 如果不存在是否自动创建，默认为true
	 * \return 委托明细数据块对指针
	 * 
	 * \details 根据合约信息和日期获取对应的委托明细数据块
	 */
	OrdDtlBlockPair* getOrdDtlBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);

	/*!
	 * \brief 获取委托队列数据块
	 * \param ct 合约信息指针
	 * \param curDate 当前日期
	 * \param bAutoCreate 如果不存在是否自动创建，默认为true
	 * \return 委托队列数据块对指针
	 * 
	 * \details 根据合约信息和日期获取对应的委托队列数据块
	 */
	OrdQueBlockPair* getOrdQueBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);

	/*!
	 * \brief 释放数据块资源
	 * \tparam T 数据块类型
	 * \param block 数据块指针
	 * 
	 * \details 释放指定类型的数据块资源
	 */
	template<typename T>
	void	releaseBlock(T* block);

	/*!
	 * \brief 将任务添加到任务队列
	 * \param task 任务信息
	 * 
	 * \details 将数据处理任务添加到异步处理队列中
	 */
	void pushTask(const TaskInfo& task);
};

