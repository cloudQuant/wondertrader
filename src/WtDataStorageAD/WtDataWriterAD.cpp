/**
 * @file WtDataWriterAD.cpp
 * @brief 基于LMDB的数据存储引擎实现文件
 * @details 该文件实现了WtDataWriterAD类，用于将行情数据写入LMDB数据库，
 *          并管理相关的内存缓存，支持多种周期的数据存储
 * @author Wesley
 * @date 2022-01-05
 * @version 1.0
 */

#include "WtDataWriterAD.h"
#include "LMDBKeys.h"

#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/decimal.h"

#include "../Includes/IBaseDataMgr.h"

using namespace std;

/**
 * @brief 引入fmt库处理格式化输出
 * @author Wesley
 * @date 2022-01-05
 */
#include "../Share/fmtlib.h"

/**
 * @brief 将日志输出到数据写入器的接收器
 * @tparam Args 可变参数类型
 * @param sink 数据写入器的接收器指针
 * @param ll 日志级别
 * @param format 格式化字符串
 * @param args 格式化参数
 * @details 使用fmt库将格式化的日志输出到数据写入器的接收器中
 */
template<typename... Args>
inline void pipe_writer_log(IDataWriterSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->outputLog(ll, buffer);
}

/**
 * @brief C接口导出函数定义
 * @details 提供给外部模块调用的C风格接口函数，用于创建和删除数据写入器实例
 */
extern "C"
{
	/**
	 * @brief 创建数据写入器实例
	 * @return IDataWriter* 数据写入器接口指针
	 * @details 创建一个WtDataWriterAD实例并返回其接口指针
	 */
	EXPORT_FLAG IDataWriter* createWriter()
	{
		IDataWriter* ret = new WtDataWriterAD();
		return ret;
	}

	/**
	 * @brief 删除数据写入器实例
	 * @param writer 数据写入器接口指针的引用
	 * @details 删除指定的数据写入器实例并将指针设置为空
	 */
	EXPORT_FLAG void deleteWriter(IDataWriter* &writer)
	{
		if (writer != NULL)
		{
			delete writer;
			writer = NULL;
		}
	}
};

/**
 * @brief 缓存大小增长步长
 * @details 当需要扩展缓存时，每次增加的缓存项数量
 */
static const uint32_t CACHE_SIZE_STEP_AD = 400;

/**
 * @brief WtDataWriterAD类的构造函数
 * @details 初始化数据写入器的各项参数，设置默认值
 */
WtDataWriterAD::WtDataWriterAD()
	: _terminated(false)           // 终止标志初始化为否
	, _log_group_size(1000)        // 日志分组大小默认为1000
	, _disable_day(false)          // 默认启用日K线存储
	, _disable_min1(false)         // 默认启用1分钟K线存储
	, _disable_min5(false)         // 默认启用5分钟K线存储
	, _disable_tick(false)         // 默认启用Tick数据存储
	, _tick_cache_block(nullptr)   // Tick缓存块初始化为空
	, _tick_mapsize(16*1024*1024)  // Tick数据库映射大小默认为16MB
	, _kline_mapsize(8*1024*1024)  // K线数据库映射大小默认为8MB
{
}


/**
 * @brief WtDataWriterAD类的析构函数
 * @details 释放数据写入器占用的资源
 */
WtDataWriterAD::~WtDataWriterAD()
{
	// 由于使用了智能指针管理资源，所以析构函数中无需额外的资源释放操作
}

/**
 * @brief 初始化数据写入器
 * @param params 初始化参数，包含数据存储路径、缓存设置等
 * @param sink 数据写入器的回调接口，用于输出日志和获取基础数据管理器
 * @return bool 初始化是否成功
 * @details 该方法负责创建必要的目录结构、设置缓存文件名称、解析配置参数并加载缓存
 */
bool WtDataWriterAD::init(WTSVariant* params, IDataWriterSink* sink)
{
	// 调用父类初始化方法
	IDataWriter::init(params, sink);

	// 获取基础数据管理器
	_bd_mgr = sink->getBDMgr();

	// 设置数据存储基础目录并确保目录存在
	_base_dir = StrUtil::standardisePath(params->getCString("path"));
	if (!BoostFile::exists(_base_dir.c_str()))
		BoostFile::create_directories(_base_dir.c_str());

	// 设置缓存文件名称
	_cache_file_tick = "cache_tick.dmb";
	_m1_cache._filename = "cache_m1.dmb";
	_m5_cache._filename = "cache_m5.dmb";
	_d1_cache._filename = "cache_d1.dmb";

	// 从参数中获取日志分组大小
	_log_group_size = params->getUInt32("groupsize");

	// 从参数中获取禁用标志
	_disable_tick = params->getBoolean("disabletick");
	_disable_min1 = params->getBoolean("disablemin1");
	_disable_min5 = params->getBoolean("disablemin5");
	_disable_day = params->getBoolean("disableday");

	// 可选参数：Tick数据库映射大小
	if (params->has("tickmapsize"))
		_tick_mapsize = params->getUInt32("tickmapsize");

	// 可选参数：K线数据库映射大小
	if (params->has("klinemapsize"))
		_kline_mapsize = params->getUInt32("klinemapsize");

	// 加载缓存文件
	loadCache();

	return true;
}

/**
 * @brief 释放数据写入器资源
 * @details 终止异步任务处理线程并释放相关资源
 */
void WtDataWriterAD::release()
{
	// 设置终止标志
	_terminated = true;

	// 如果存在任务处理线程，则通知其终止并等待其完成
	if (_task_thrd)
	{
		_task_cond.notify_all();
		_task_thrd->join();
	}
	// 注意：由于使用了智能指针管理资源，其他资源会在析构时自动释放
}

/**
 * @brief 加载缓存文件
 * @details 加载或创建Tick和K线缓存文件，并初始化相关的内存结构
 */
void WtDataWriterAD::loadCache()
{
	// 加载Tick缓存文件
	if (_tick_cache_file == NULL)
	{
		bool bNew = false;
		std::string filename = _base_dir + _cache_file_tick;
		// 如果缓存文件不存在，创建新文件
		if (!BoostFile::exists(filename.c_str()))
		{
			// 计算文件大小：头部 + 数据项 * 初始容量
			uint64_t uSize = sizeof(RTTickCache) + sizeof(TickCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			bf.create_new_file(filename.c_str());
			bf.truncate_file((uint32_t)uSize);
			bf.close_file();
			bNew = true;
		}

		// 创建内存映射文件对象并映射缓存文件
		_tick_cache_file.reset(new BoostMappingFile);
		_tick_cache_file->map(filename.c_str());
		// 获取缓存块指针
		_tick_cache_block = (RTTickCache*)_tick_cache_file->addr();

		// 确保缓存大小不超过容量
		_tick_cache_block->_size = min(_tick_cache_block->_size, _tick_cache_block->_capacity);

		if (bNew)
		{
			// 如果是新创建的缓存文件，初始化缓存块
			memset(_tick_cache_block, 0, _tick_cache_file->size());

			// 设置缓存块的基本属性
			_tick_cache_block->_capacity = CACHE_SIZE_STEP_AD;
			_tick_cache_block->_type = BT_RT_Cache;
			_tick_cache_block->_size = 0;
			_tick_cache_block->_version = 1;
			strcpy(_tick_cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			// 如果是加载现有缓存文件，构建索引映射
			for (uint32_t i = 0; i < _tick_cache_block->_size; i++)
			{
				const TickCacheItem& item = _tick_cache_block->_items[i];
				// 使用交易所和合约代码组合作为索引键
				std::string key = StrUtil::printf("%s.%s", item._tick.exchg, item._tick.code);
				_tick_cache_idx[key] = i;
			}
		}
	}

	// 加载1分钟K线缓存文件
	if (_m1_cache.empty())
	{
		bool bNew = false;
		std::string filename = _base_dir + _m1_cache._filename;
		// 如果缓存文件不存在，创建新文件
		if (!BoostFile::exists(filename.c_str()))
		{
			// 计算文件大小：头部 + 数据项 * 初始容量
			uint64_t uSize = sizeof(RTBarCache) + sizeof(BarCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			bf.create_new_file(filename.c_str());
			bf.truncate_file((uint32_t)uSize);
			bf.close_file();
			bNew = true;
		}

		// 创建内存映射文件对象并映射缓存文件
		_m1_cache._file_ptr.reset(new BoostMappingFile);
		_m1_cache._file_ptr->map(filename.c_str());
		// 获取缓存块指针
		_m1_cache._cache_block = (RTBarCache*)_m1_cache._file_ptr->addr();

		// 确保缓存大小不超过容量
		_m1_cache._cache_block->_size = min(_m1_cache._cache_block->_size, _m1_cache._cache_block->_capacity);

		if (bNew)
		{
			// 如果是新创建的缓存文件，初始化缓存块
			memset(_m1_cache._cache_block, 0, _m1_cache._file_ptr->size());

			// 设置缓存块的基本属性
			_m1_cache._cache_block->_capacity = CACHE_SIZE_STEP_AD;
			_m1_cache._cache_block->_type = BT_RT_Cache;
			_m1_cache._cache_block->_size = 0;
			_m1_cache._cache_block->_version = 1;
			strcpy(_m1_cache._cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			// 如果是加载现有缓存文件，构建索引映射
			for (uint32_t i = 0; i < _m1_cache._cache_block->_size; i++)
			{
				const BarCacheItem& item = _m1_cache._cache_block->_items[i];
				// 使用交易所和合约代码组合作为索引键
				std::string key = StrUtil::printf("%s.%s", item._exchg, item._code);
				_m1_cache._idx[key] = i;
			}
		}
	}

	if (_m5_cache.empty())
	{
		bool bNew = false;
		std::string filename = _base_dir + _m5_cache._filename;
		if (!BoostFile::exists(filename.c_str()))
		{
			uint64_t uSize = sizeof(RTBarCache) + sizeof(BarCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			bf.create_new_file(filename.c_str());
			bf.truncate_file((uint32_t)uSize);
			bf.close_file();
			bNew = true;
		}

		_m5_cache._file_ptr.reset(new BoostMappingFile);
		_m5_cache._file_ptr->map(filename.c_str());
		_m5_cache._cache_block = (RTBarCache*)_m5_cache._file_ptr->addr();

		_m5_cache._cache_block->_size = min(_m5_cache._cache_block->_size, _m5_cache._cache_block->_capacity);

		if (bNew)
		{
			memset(_m5_cache._cache_block, 0, _m5_cache._file_ptr->size());

			_m5_cache._cache_block->_capacity = CACHE_SIZE_STEP_AD;
			_m5_cache._cache_block->_type = BT_RT_Cache;
			_m5_cache._cache_block->_size = 0;
			_m5_cache._cache_block->_version = 1;
			strcpy(_m5_cache._cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			for (uint32_t i = 0; i < _m5_cache._cache_block->_size; i++)
			{
				const BarCacheItem& item = _m5_cache._cache_block->_items[i];
				std::string key = StrUtil::printf("%s.%s", item._exchg, item._code);
				_m5_cache._idx[key] = i;
			}
		}
	}

	if (_d1_cache.empty())
	{
		bool bNew = false;
		std::string filename = _base_dir + _d1_cache._filename;
		if (!BoostFile::exists(filename.c_str()))
		{
			uint64_t uSize = sizeof(RTBarCache) + sizeof(BarCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			bf.create_new_file(filename.c_str());
			bf.truncate_file((uint32_t)uSize);
			bf.close_file();
			bNew = true;
		}

		_d1_cache._file_ptr.reset(new BoostMappingFile);
		_d1_cache._file_ptr->map(filename.c_str());
		_d1_cache._cache_block = (RTBarCache*)_d1_cache._file_ptr->addr();

		_d1_cache._cache_block->_size = min(_d1_cache._cache_block->_size, _d1_cache._cache_block->_capacity);

		if (bNew)
		{
			memset(_d1_cache._cache_block, 0, _d1_cache._file_ptr->size());

			_d1_cache._cache_block->_capacity = CACHE_SIZE_STEP_AD;
			_d1_cache._cache_block->_type = BT_RT_Cache;
			_d1_cache._cache_block->_size = 0;
			_d1_cache._cache_block->_version = 1;
			strcpy(_d1_cache._cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			for (uint32_t i = 0; i < _d1_cache._cache_block->_size; i++)
			{
				const BarCacheItem& item = _d1_cache._cache_block->_items[i];
				std::string key = StrUtil::printf("%s.%s", item._exchg, item._code);
				_d1_cache._idx[key] = i;
			}
		}
	}
}

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
void* WtDataWriterAD::resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount)
{
	// 检查映射文件指针是否有效
	if (mfPtr == NULL)
		return NULL;

	//调用该函数之前,应该已经申请了写锁了
	// 获取块头部指针
	RTBlockHeader* tBlock = (RTBlockHeader*)mfPtr->addr();
	// 如果当前容量足够，直接返回
	if (tBlock->_capacity >= nCount)
		return mfPtr->addr();

	// 获取文件名和计算新旧大小
	const char* filename = mfPtr->filename();
	uint64_t uOldSize = sizeof(HeaderType) + sizeof(T)*tBlock->_capacity;
	uint64_t uNewSize = sizeof(HeaderType) + sizeof(T)*nCount;
	// 创建空数据块用于扩展文件
	std::string data;
	data.resize((std::size_t)(uNewSize - uOldSize), 0);
	try
	{
		// 打开文件并在尾部添加空间
		BoostFile f;
		f.open_existing_file(filename);
		f.seek_to_end();
		f.write_file(data.c_str(), data.size());
		f.close_file();
	}
	catch(std::exception& ex)
	{
		// 扩展文件失败时记录错误并返回原指针
		pipe_writer_log(_sink, LL_ERROR, "Exception occured while expanding RT cache file of {}[{}]: {}", filename, uNewSize, ex.what());
		return mfPtr->addr();
	}

	// 创建新的映射文件对象
	BoostMappingFile* pNewMf = new BoostMappingFile();
	if (!pNewMf->map(filename))
	{
		// 映射失败时清理资源并返回NULL
		delete pNewMf;
		return NULL;
	}

	// 更新智能指针
	mfPtr.reset(pNewMf);

	// 更新块头部并返回新的内存地址
	tBlock = (RTBlockHeader*)mfPtr->addr();
	tBlock->_capacity = nCount;
	return mfPtr->addr();
}

/**
 * @brief 写入Tick数据
 * @param curTick 当前的Tick数据
 * @param procFlag 处理标志，控制数据处理方式（0=直接写入，1=预处理，2=自动累加）
 * @return bool 写入是否成功
 * @details 将Tick数据写入缓存和数据库，并触发相关的K线生成和存储逻辑
 */
bool WtDataWriterAD::writeTick(WTSTickData* curTick, uint32_t procFlag)
{
	// 检查输入参数是否有效
	if (curTick == NULL)
		return false;

	// 增加引用计数，确保在异步任务中不会被释放
	curTick->retain();
	// 将写入任务添加到异步任务队列
	pushTask([this, curTick, procFlag](){

		do
		{
			// 获取合约信息
			WTSContractInfo* ct = curTick->getContractInfo();
			if(ct == NULL)
				break;

			// 获取商品信息
			WTSCommodityInfo* commInfo = ct->getCommInfo();

			// 根据交易时段过滤
			if (!_sink->canSessionReceive(commInfo->getSession()))
				break;

			// 先更新Tick缓存，如果更新失败则终止处理
			if (!updateTickCache(ct, curTick, procFlag))
				break;

			// 如果未禁用Tick存储，则将数据写入数据库
			if(!_disable_tick)
				pipeToTicks(ct, curTick);

			// 更新K线缓存，生成各周期K线
			updateBarCache(ct, curTick);

			// 广播Tick数据给监听器
			_sink->broadcastTick(curTick);

			// 统计并定期输出日志
			static wt_hashmap<std::string, uint64_t> _tcnt_map;
			_tcnt_map[curTick->exchg()]++;
			if (_tcnt_map[curTick->exchg()] % _log_group_size == 0)
			{
				pipe_writer_log(_sink, LL_INFO, "{} ticks received from exchange {}",_tcnt_map[curTick->exchg()], curTick->exchg());
			}
		} while (false);

		// 释放Tick数据的引用
		curTick->release();
	});
	return true;
}

/**
 * @brief 添加异步任务
 * @param task 任务函数对象
 * @details 将任务添加到任务队列中，并根据配置决定是否异步执行。
 *          如果开启异步模式，则由任务线程处理；否则直接同步执行。
 */
void WtDataWriterAD::pushTask(TaskInfo task)
{
	// 如果开启了异步任务模式
	if(_async_task)
	{
		// 加锁并将任务添加到队列
		StdUniqueLock lck(_task_mtx);
		_tasks.push(task);
		// 通知等待中的任务处理线程
		_task_cond.notify_all();
	}
	else
	{
		// 如果不是异步模式，直接执行任务
		task();
		return;
	}

	// 如果任务线程还没有创建，则创建一个新的任务处理线程
	if(_task_thrd == NULL)
	{
		_task_thrd.reset(new StdThread([this](){
			// 线程主循环，直到收到终止信号
			while (!_terminated)
			{
				// 如果任务队列为空，等待新任务
				if(_tasks.empty())
				{
					StdUniqueLock lck(_task_mtx);
					_task_cond.wait(_task_mtx);
					continue;
				}

				// 创建临时队列并交换任务，减少锁的持有时间
				std::queue<TaskInfo> tempQueue;
				{
					StdUniqueLock lck(_task_mtx);
					tempQueue.swap(_tasks);
				}

				// 处理临时队列中的所有任务
				while(!tempQueue.empty())
				{
					TaskInfo& curTask = tempQueue.front();
					curTask();
					tempQueue.pop();
				}
			}
		}));
	}
}

/**
 * @brief 将Tick数据写入数据库
 * @param ct 合约信息指针
 * @param curTick 当前的Tick数据
 * @details 将Tick数据写入到LMDB数据库中进行持久化存储，并调用外部数据导出器
 */
void WtDataWriterAD::pipeToTicks(WTSContractInfo* ct, WTSTickData* curTick)
{
	// 获取对应的Tick数据库
	WtLMDBPtr db = get_t_db(ct->getExchg(), ct->getCode());
	if (db)
	{
		// 将时间转换为偏移时间，便于根据交易日筛选历史数据
		// 这里将时间转成偏移时间，并用交易日作为日期
		uint32_t actTime = curTick->actiontime();
		// 计算偏移时间：将小时分钟部分转换为偏移时间，保留毫秒部分
		uint32_t offTime = ct->getCommInfo()->getSessionInfo()->offsetTime(actTime / 100000, true) * 100000 + actTime % 100000;

		// 创建LMDB键值，包含交易所、合约代码、交易日和偏移时间
		LMDBHftKey key(ct->getExchg(), ct->getCode(), curTick->tradingdate(), offTime);
		// 创建查询对象并将数据写入数据库
		WtLMDBQuery query(*db);
		if (!query.put_and_commit((void*)&key, sizeof(key), &curTick->getTickStruct(), sizeof(WTSTickStruct)))
		{
			// 写入失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe tick of {} to db failed: {}", ct->getFullCode(), db->errmsg());
		}
	}

	// 遍历所有外部数据导出器
	for(auto& item : _dumpers)
	{
		// 获取导出器ID和对象
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		// 调用导出器将Tick数据导出
		bool bSucc = dumper->dumpHisTicks(ct->getFullCode(), curTick->tradingdate(), &curTick->getTickStruct(), 1);
		if (!bSucc)
		{
			// 导出失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe tick data of {} via extended dumper {} failed", ct->getFullCode(), id);
		}
	}
}

/**
 * @brief 将日K线数据写入数据库
 * @param ct 合约信息指针
 * @param bar 待写入的K线数据结构
 * @details 将日K线数据写入到LMDB数据库中进行持久化存储，并调用外部数据导出器
 */
void WtDataWriterAD::pipeToDayBars(WTSContractInfo* ct, const WTSBarStruct& bar)
{
	// 获取对应的日K线数据库
	WtLMDBPtr db = get_k_db(ct->getExchg(), KP_DAY);
	if (db)
	{
		// 创建LMDB键值，包含交易所、合约代码和日期
		LMDBBarKey key(ct->getExchg(), ct->getCode(), bar.date);
		// 创建查询对象并将数据写入数据库
		WtLMDBQuery query(*db);
		if (!query.put_and_commit((void*)&key, sizeof(key), (void*)&bar, sizeof(WTSBarStruct)))
		{
			// 写入失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe day bar @ {} of {} to db failed", bar.date, ct->getFullCode());
		}
		else
		{
			// 写入成功时记录调试日志
			pipe_writer_log(_sink, LL_DEBUG, "day bar @ {} of {} piped to db", bar.date, ct->getFullCode());
		}
	}

	// 遍历所有外部数据导出器
	for (auto& item : _dumpers)
	{
		// 获取导出器ID和对象
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		// 调用导出器将日K线数据导出，周期标识为"d1"
		bool bSucc = dumper->dumpHisBars(ct->getFullCode(), "d1", (WTSBarStruct*)&bar, 1);
		if (!bSucc)
		{
			// 导出失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe day bar @ {} of {} via extended dumper {} failed", bar.date, ct->getFullCode(), id);
		}
	}
}

/**
 * @brief 将1分钟K线数据写入数据库
 * @param ct 合约信息指针
 * @param bar 待写入的K线数据结构
 * @details 将1分钟K线数据写入到LMDB数据库中进行持久化存储，并调用外部数据导出器
 */
void WtDataWriterAD::pipeToM1Bars(WTSContractInfo* ct, const WTSBarStruct& bar)
{
	// 获取对应的1分钟K线数据库
	WtLMDBPtr db = get_k_db(ct->getExchg(), KP_Minute1);
	if(db)
	{
		// 创建LMDB键值，包含交易所、合约代码和时间戳
		LMDBBarKey key(ct->getExchg(), ct->getCode(), (uint32_t)bar.time);
		// 创建查询对象并将数据写入数据库
		WtLMDBQuery query(*db);
		if(!query.put_and_commit((void*)&key, sizeof(key), (void*)&bar, sizeof(WTSBarStruct)))
		{
			// 写入失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe m1 bar @ {} of {} to db failed", bar.time, ct->getFullCode());
		}
		else
		{
			// 写入成功时记录调试日志
			pipe_writer_log(_sink, LL_DEBUG, "m1 bar @ {} of {} piped to db", bar.time, ct->getFullCode());
		}
	}

	// 遍历所有外部数据导出器
	for (auto& item : _dumpers)
	{
		// 获取导出器ID和对象
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		// 调用导出器将1分钟K线数据导出，周期标识为"m1"
		bool bSucc = dumper->dumpHisBars(ct->getFullCode(), "m1", (WTSBarStruct*)&bar, 1);
		if (!bSucc)
		{
			// 导出失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe m1 bar @ {} of {} via extended dumper {} failed", bar.time, ct->getFullCode(), id);
		}
	}
}

/**
 * @brief 将5分钟K线数据写入数据库
 * @param ct 合约信息指针
 * @param bar 待写入的K线数据结构
 * @details 将5分钟K线数据写入到LMDB数据库中进行持久化存储，并调用外部数据导出器
 */
void WtDataWriterAD::pipeToM5Bars(WTSContractInfo* ct, const WTSBarStruct& bar)
{
	// 获取对应的5分钟K线数据库
	WtLMDBPtr db = get_k_db(ct->getExchg(), KP_Minute5);
	if (db)
	{
		// 创建LMDB键值，包含交易所、合约代码和时间戳
		LMDBBarKey key(ct->getExchg(), ct->getCode(), (uint32_t)bar.time);
		// 创建查询对象并将数据写入数据库
		WtLMDBQuery query(*db);
		if (!query.put_and_commit((void*)&key, sizeof(key), (void*)&bar, sizeof(bar)))
		{
			// 写入失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe m5 bar @ {} of {} to db failed", bar.time, ct->getFullCode());
		}
		else
		{
			// 写入成功时记录调试日志
			pipe_writer_log(_sink, LL_DEBUG, "m5 bar @ {} of {} piped to db", bar.time, ct->getFullCode());
		}
	}

	// 遍历所有外部数据导出器
	for (auto& item : _dumpers)
	{
		// 获取导出器ID和对象
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		// 调用导出器将5分钟K线数据导出，周期标识为"m5"
		bool bSucc = dumper->dumpHisBars(ct->getFullCode(), "m5", (WTSBarStruct*)&bar, 1);
		if (!bSucc)
		{
			// 导出失败时记录错误日志
			pipe_writer_log(_sink, LL_ERROR, "pipe m5 bar @ {} of {} via extended dumper {} failed", bar.time, ct->getFullCode(), id);
		}
	}
}

/**
 * @brief 更新K线缓存
 * @param ct 合约信息指针
 * @param curTick 当前的Tick数据
 * @details 根据Tick数据更新不同周期的K线缓存，包括日、1分钟、5分钟K线
 */
void WtDataWriterAD::updateBarCache(WTSContractInfo* ct, WTSTickData* curTick)
{
	// 获取操作日期
	uint32_t uDate = curTick->actiondate();
	// 获取合约对应的交易时段信息
	WTSSessionInfo* sInfo = _bd_mgr->getSessionByCode(curTick->code(), curTick->exchg());
	// 获取当前时间（去除毫秒部分）
	uint32_t curTime = curTick->actiontime() / 100000;

	// 将时间转换为从交易时段开始的分钟数
	uint32_t minutes = sInfo->timeToMinutes(curTime, false);
	// 如果时间无效，直接返回
	if (minutes == INVALID_UINT32)
		return;

	// 当秒数为0时的特殊处理，比如091500000这笔tick要算作0915的
	// 如果是小节结束，要算作小节结束那一分钟
	// 因为经常会有超过结束时间的价格进来，如113000500
	if (sInfo->isLastOfSection(curTime))
	{
		// 如果是小节结束时间，则分钟数减1
		minutes--;
	}

	// 根据交易所和合约代码生成唯一键
	std::string key = StrUtil::printf("%s.%s", curTick->exchg(), curTick->code());

	// 更新日线K线缓存
	if (!_disable_day && _d1_cache._cache_block)
	{
		// 锁定日线缓存以确保线程安全
		StdUniqueLock lock(_d1_cache._mtx);
		// 初始化索引和新合约标志
		uint32_t idx = 0;
		bool bNewCode = false;
		// 检查当前合约是否已存在于缓存中
		if (_d1_cache._idx.find(key) == _d1_cache._idx.end())
		{
			// 新合约，分配新的缓存索引
			idx = _d1_cache._cache_block->_size;
			_d1_cache._idx[key] = _d1_cache._cache_block->_size;
			_d1_cache._cache_block->_size += 1;
			// 检查缓存容量是否足够，如果不足则扩容
			if (_d1_cache._cache_block->_size >= _d1_cache._cache_block->_capacity)
			{
				// 调用重新分配函数扩大缓存块
				_d1_cache._cache_block = (RTBarCache*)resizeRTBlock<RTBarCache, BarCacheItem>(_d1_cache._file_ptr, _d1_cache._cache_block->_capacity + CACHE_SIZE_STEP_AD);
				pipe_writer_log(_sink, LL_INFO, "day cache resized to {} items", _d1_cache._cache_block->_capacity);
			}
			// 标记为新合约
			bNewCode = true;
		}
		else
		{
			// 已存在的合约，获取其索引
			idx = _d1_cache._idx[key];
		}

		// 获取缓存项
		BarCacheItem& item = (BarCacheItem&)_d1_cache._cache_block->_items[idx];
		// 如果是新合约，初始化交易所和合约代码
		if (bNewCode)
		{
			strcpy(item._exchg, curTick->exchg());
			strcpy(item._code, curTick->code());
		}
		// 获取上一条K线数据
		WTSBarStruct* lastBar = &item._bar;

		// 获取当前交易日期
		uint32_t barDate = curTick->tradingdate();

		// 判断是否需要创建新的K线
		bool bNewBar = false;
		// 如果没有上一条K线或者交易日期已变化，需要创建新的K线
		if (lastBar == NULL || barDate > lastBar->date)
		{
			bNewBar = true;
		}

		// 指向当前要更新的K线
		WTSBarStruct* newBar = lastBar;
		// 如果需要创建新的K线
		if (bNewBar)
		{
			// 将上一条完成的K线写入数据库
			// 如果是新的合约，说明还没有历史数据，不需要写入
			if (!bNewCode)
			{
				// 调用写入日线K线数据的方法
				pipeToDayBars(ct, *lastBar);
			}

			// 初始化新的K线数据
			newBar->date = curTick->tradingdate();
			newBar->time = barDate;
			newBar->open = curTick->price();
			newBar->high = curTick->price();
			newBar->low = curTick->price();
			newBar->close = curTick->price();

			newBar->vol = curTick->volume();
			newBar->money = curTick->turnover();
			newBar->hold = curTick->openinterest();
			newBar->add = curTick->additional();
		}
		else
		{
			/*
			*	By Wesley @ 2023.07.05
			*	发现某些品种，开盘时可能会推送price为0的tick进来
			*	会导致open和low都是0，所以要再做一个判断
			*/
			// 如果开盘价为0，使用当前tick的价格
			if (decimal::eq(newBar->open, 0))
				newBar->open = curTick->price();

			// 如果最低价为0，使用当前tick的价格，否则取最小值
			if (decimal::eq(newBar->low, 0))
				newBar->low = curTick->price();
			else
				newBar->low = std::min(curTick->price(), newBar->low);

			// 更新收盘价和最高价
			newBar->close = curTick->price();
			newBar->high = max(curTick->price(), newBar->high);

			// 累加成交量和成交金额
			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			// 注意：这里有一个重复累加的问题，可能是代码错误
			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			// 更新持仓量和附加数据
			newBar->hold = curTick->openinterest();
			newBar->add += curTick->additional();
		}
	}

	// 更新1分钟K线缓存
	if (!_disable_min1 && _m1_cache._cache_block)
	{
		// 锁定1分钟线缓存以确保线程安全
		StdUniqueLock lock(_m1_cache._mtx);
		// 初始化索引和新合约标志
		uint32_t idx = 0;
		bool bNewCode = false;
		// 检查当前合约是否已存在于1分钟线缓存中
		if (_m1_cache._idx.find(key) == _m1_cache._idx.end())
		{
			// 新合约，分配新的缓存索引
			idx = _m1_cache._cache_block->_size;
			_m1_cache._idx[key] = _m1_cache._cache_block->_size;
			_m1_cache._cache_block->_size += 1;
			// 检查缓存容量是否足够，如果不足则扩容
			if (_m1_cache._cache_block->_size >= _m1_cache._cache_block->_capacity)
			{
				// 调用重新分配函数扩大缓存块
				_m1_cache._cache_block = (RTBarCache*)resizeRTBlock<RTBarCache, BarCacheItem>(_m1_cache._file_ptr, _m1_cache._cache_block->_capacity + CACHE_SIZE_STEP_AD);
				pipe_writer_log(_sink, LL_INFO, "m1 cache resized to {} items", _m1_cache._cache_block->_capacity);
			}
			// 标记为新合约
			bNewCode = true;
		}
		else
		{
			// 已存在的合约，获取其索引
			idx = _m1_cache._idx[key];
		}

		// 获取缓存项
		BarCacheItem& item = (BarCacheItem&)_m1_cache._cache_block->_items[idx];
		// 如果是新合约，初始化交易所和合约代码
		if(bNewCode)
		{
			strcpy(item._exchg, curTick->exchg());
			strcpy(item._code, curTick->code());
		}
		// 获取上一条K线数据
		WTSBarStruct* lastBar = &item._bar;


		// 计算当前1分钟K线的时间
		// 当前分钟数+1得到下一分钟的开始
		uint32_t barMins = minutes + 1;
		// 将分钟数转换为时间格式
		uint64_t barTime = sInfo->minuteToTime(barMins);
		// 默认使用当前日期
		uint32_t barDate = uDate;
		// 如果时间为0，表示跨天，需要将日期设置为下一交易日
		if (barTime == 0)
		{
			barDate = TimeUtils::getNextDate(barDate);
		}
		// 将日期和时间转换为分钟K线时间戳
		barTime = TimeUtils::timeToMinBar(barDate, (uint32_t)barTime);

		// 判断是否需要创建新的K线
		bool bNewBar = false;
		// 如果没有上一条K线或者时间已变化，需要创建新的K线
		if (lastBar == NULL || barTime > lastBar->time)
		{
			bNewBar = true;
		}

		// 指向当前要更新的K线
		WTSBarStruct* newBar = lastBar;
		// 如果需要创建新的K线
		if (bNewBar)
		{
			// 将上一条完成的K线写入数据库
			// 如果是新的合约，说明还没有历史数据，不需要写入
			if (!bNewCode)
			{
				// 调用写入1分钟K线数据的方法
				pipeToM1Bars(ct, *lastBar);

				// 检查是否跨交易日
				uint32_t lastMins = sInfo->timeToMinutes(lastBar->time % 10000, false);
				if(lastMins > barMins)
				{
					// 如果上一条K线的分钟数大于当前K线的分钟数
					// 说明交易日发生了变化
					// 需要保存日线数据
				}
			}

			// 初始化新的K线数据
			newBar->date = curTick->tradingdate();
			newBar->time = barTime;
			newBar->open = curTick->price();
			newBar->high = curTick->price();
			newBar->low = curTick->price();
			newBar->close = curTick->price();

			newBar->vol = curTick->volume();
			newBar->money = curTick->turnover();
			newBar->hold = curTick->openinterest();
			newBar->add = curTick->additional();
		}
		else
		{
			/*
			*	By Wesley @ 2023.07.05
			*	发现某些品种，开盘时可能会推送price为0的tick进来
			*	会导致open和low都是0，所以要再做一个判断
			*/
			// 如果开盘价为0，使用当前tick的价格
			if (decimal::eq(newBar->open, 0))
				newBar->open = curTick->price();

			// 如果最低价为0，使用当前tick的价格，否则取最小值
			if (decimal::eq(newBar->low, 0))
				newBar->low = curTick->price();
			else
				newBar->low = std::min(curTick->price(), newBar->low);

			// 更新收盘价和最高价
			newBar->close = curTick->price();
			newBar->high = max(curTick->price(), newBar->high);

			// 累加成交量和成交金额
			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			// 更新持仓量和附加数据
			newBar->hold = curTick->openinterest();
			newBar->add += curTick->additional();
		}
	}

	// 更新5分钟K线缓存
	if (!_disable_min5 && _m5_cache._cache_block)
	{
		// 锁定5分钟线缓存以确保线程安全
		StdUniqueLock lock(_m5_cache._mtx);
		// 初始化索引和新合约标志
		uint32_t idx = 0;
		bool bNewCode = false;
		// 检查当前合约是否已存在于5分钟线缓存中
		if (_m5_cache._idx.find(key) == _m5_cache._idx.end())
		{
			// 新合约，分配新的缓存索引
			idx = _m5_cache._cache_block->_size;
			_m5_cache._idx[key] = _m5_cache._cache_block->_size;
			_m5_cache._cache_block->_size += 1;
			// 检查缓存容量是否足够，如果不足则扩容
			if (_m5_cache._cache_block->_size >= _m5_cache._cache_block->_capacity)
			{
				// 调用重新分配函数扩大缓存块
				_m5_cache._cache_block = (RTBarCache*)resizeRTBlock<RTBarCache, BarCacheItem>(_m5_cache._file_ptr, _m5_cache._cache_block->_capacity + CACHE_SIZE_STEP_AD);
				pipe_writer_log(_sink, LL_INFO, "m5 cache resized to {} items", _m5_cache._cache_block->_capacity);
			}
			// 标记为新合约
			bNewCode = true;
		}
		else
		{
			// 已存在的合约，获取其索引
			idx = _m5_cache._idx[key];
		}

		// 获取缓存项
		BarCacheItem& item = (BarCacheItem&)_m5_cache._cache_block->_items[idx];
		// 如果是新合约，初始化交易所和合约代码
		if (bNewCode)
		{
			strcpy(item._exchg, curTick->exchg());
			strcpy(item._code, curTick->code());
		}
		WTSBarStruct* lastBar = &item._bar;

		//拼接5分钟线
		uint32_t barMins = (minutes / 5) * 5 + 5;
		uint64_t barTime = sInfo->minuteToTime(barMins);
		uint32_t barDate = uDate;
		if (barTime == 0)
		{
			barDate = TimeUtils::getNextDate(barDate);
		}
		barTime = TimeUtils::timeToMinBar(barDate, (uint32_t)barTime);

		bool bNewBar = false;
		if (lastBar == NULL || barTime > lastBar->time)
		{
			bNewBar = true;
		}

		WTSBarStruct* newBar = lastBar;
		if (bNewBar)
		{
			//这里要将lastBar往外写
			//如果是新的合约，说明还没数据，不往外写
			if(!bNewCode)
				pipeToM5Bars(ct, *lastBar);

			newBar->date = curTick->tradingdate();
			newBar->time = barTime;
			newBar->open = curTick->price();
			newBar->high = curTick->price();
			newBar->low = curTick->price();
			newBar->close = curTick->price();

			newBar->vol = curTick->volume();
			newBar->money = curTick->turnover();
			newBar->hold = curTick->openinterest();
			newBar->add = curTick->additional();
		}
		else
		{
			newBar->close = curTick->price();
			newBar->high = max(curTick->price(), newBar->high);
			newBar->low = min(curTick->price(), newBar->low);

			// 累加成交量和成交金额
			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			// 更新持仓量和附加数据
			newBar->hold = curTick->openinterest();
			newBar->add += curTick->additional();
		}
	}
}

WTSTickData* WtDataWriterAD::getCurTick(const char* code, const char* exchg/* = ""*/)
{
	if (strlen(code) == 0)
		return NULL;

	WTSContractInfo* ct = _bd_mgr->getContract(code, exchg);
	if (ct == NULL)
		return NULL;

	std::string key = StrUtil::printf("%s.%s", ct->getExchg(), ct->getCode());
	StdUniqueLock lock(_mtx_tick_cache);
	auto it = _tick_cache_idx.find(key);
	if (it == _tick_cache_idx.end())
		return NULL;

	uint32_t idx = it->second;
	TickCacheItem& item = _tick_cache_block->_items[idx];
	return WTSTickData::create(item._tick);
}

bool WtDataWriterAD::updateTickCache(WTSContractInfo* ct, WTSTickData* curTick, uint32_t procFlag)
{
	if (curTick == NULL || _tick_cache_block == NULL)
	{
		pipe_writer_log(_sink, LL_ERROR, "Tick cache data not initialized");
		return false;
	}

	StdUniqueLock lock(_mtx_tick_cache);
	std::string key = StrUtil::printf("%s.%s", curTick->exchg(), curTick->code());
	uint32_t idx = 0;
	if (_tick_cache_idx.find(key) == _tick_cache_idx.end())
	{
		idx = _tick_cache_block->_size;
		_tick_cache_idx[key] = _tick_cache_block->_size;
		_tick_cache_block->_size += 1;
		if(_tick_cache_block->_size >= _tick_cache_block->_capacity)
		{
			_tick_cache_block = (RTTickCache*)resizeRTBlock<RTTickCache, TickCacheItem>(_tick_cache_file, _tick_cache_block->_capacity + CACHE_SIZE_STEP_AD);
			pipe_writer_log(_sink, LL_INFO, "Tick Cache resized to {} items", _tick_cache_block->_capacity);
		}
	}
	else
	{
		idx = _tick_cache_idx[key];
	}


	TickCacheItem& item = _tick_cache_block->_items[idx];
	if (curTick->tradingdate() < item._date)
	{
		pipe_writer_log(_sink, LL_INFO, "Tradingday[{}] of {} is less than cached tradingday[{}]", curTick->tradingdate(), curTick->code(), item._date);
		return false;
	}

	WTSTickStruct& newTick = curTick->getTickStruct();

	if (curTick->tradingdate() > item._date)
	{
		item._date = curTick->tradingdate();
		
		if(procFlag == 0)
		{
			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));
		}
		else if (procFlag == 1)
		{
			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));

			item._tick.volume = item._tick.total_volume;
			item._tick.turn_over = item._tick.total_turnover;
			item._tick.diff_interest = item._tick.open_interest - item._tick.pre_interest;

			newTick.volume = newTick.total_volume;
			newTick.turn_over = newTick.total_turnover;
			newTick.diff_interest = newTick.open_interest - newTick.pre_interest;
		}
		else if(procFlag == 2)
		{
			double pre_close = item._tick.price;
			double pre_interest = item._tick.open_interest;

			if (decimal::eq(newTick.total_volume, 0))
				newTick.total_volume = newTick.volume + item._tick.total_volume;

			if (decimal::eq(newTick.total_turnover, 0))
				newTick.total_turnover = newTick.turn_over + item._tick.total_turnover;

			if (decimal::eq(newTick.open, 0))
				newTick.open = newTick.price;

			if (decimal::eq(newTick.high, 0))
				newTick.high = newTick.price;

			if (decimal::eq(newTick.low, 0))
				newTick.low =newTick.price;

			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));
			item._tick.pre_close = pre_close;
			item._tick.pre_interest = pre_interest;
		}

		//	newTick.trading_date, curTick->exchg(), curTick->code(), curTick->volume(),
		//	curTick->turnover(), curTick->openinterest(), curTick->additional());
		pipe_writer_log(_sink, LL_INFO, "First tick of new tradingday {},{}.{},{},{},{},{},{}", 
			newTick.trading_date, curTick->exchg(), curTick->code(), curTick->price(), curTick->volume(),
			curTick->turnover(), curTick->openinterest(), curTick->additional());
	}
	else
	{
		//如果缓存里的数据日期大于最新行情的日期
		//或者缓存里的时间大于等于最新行情的时间,数据就不需要处理
		WTSSessionInfo* sInfo = _bd_mgr->getSessionByCode(curTick->code(), curTick->exchg());
		uint32_t tdate = sInfo->getOffsetDate(curTick->actiondate(), curTick->actiontime() / 100000);
		if (tdate > curTick->tradingdate())
		{
			pipe_writer_log(_sink, LL_ERROR, "Last tick of {}.{} with time {}.{} has an exception, abandoned", curTick->exchg(), curTick->code(), curTick->actiondate(), curTick->actiontime());
			return false;
		}
		else if (curTick->totalvolume() < item._tick.total_volume && procFlag != 2)
		{
			pipe_writer_log(_sink, LL_ERROR, "Last tick of {}.{} with time {}.{}, volume {} is less than cached volume {}, abandoned", 
				curTick->exchg(), curTick->code(), curTick->actiondate(), curTick->actiontime(), curTick->totalvolume(), item._tick.total_volume);
			return false;
		}

		//时间戳相同,但是成交量大于等于原来的,这种情况一般是郑商所,这里的处理方式就是时间戳+200毫秒
		//By Wesley @ 2021.12.21
		//今天发现居然一秒出现了4笔，实在是有点无语
		//只能把500毫秒的变化量改成200，并且改成发生时间小于等于上一笔的判断
		if(newTick.action_date == item._tick.action_date && newTick.action_time <= item._tick.action_time && newTick.total_volume >= item._tick.total_volume)
		{
			newTick.action_time += 200;
		}

		//这里就要看需不需要预处理了
		if(procFlag == 0)
		{
			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));
		}
		else if (procFlag == 1)
		{
			newTick.volume = newTick.total_volume - item._tick.total_volume;
			newTick.turn_over = newTick.total_turnover - item._tick.total_turnover;
			newTick.diff_interest = newTick.open_interest - item._tick.open_interest;

			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));
		}
		else if (procFlag == 2)
		{
			//自动累加
			//如果总成交量为0，则需要累加上一笔的总成交量
			if(decimal::eq(newTick.total_volume, 0))
				newTick.total_volume = newTick.volume + item._tick.total_volume;

			if (decimal::eq(newTick.total_turnover, 0))
				newTick.total_turnover = newTick.turn_over + item._tick.total_turnover;

			if (decimal::eq(newTick.open, 0))
				newTick.open = newTick.price;

			if (decimal::eq(newTick.high, 0))
				newTick.high = max(newTick.price, item._tick.high);

			if (decimal::eq(newTick.low, 0))
				newTick.low = max(newTick.price, item._tick.low);

			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));
		}
	}

	return true;
}

WtDataWriterAD::WtLMDBPtr WtDataWriterAD::get_k_db(const char* exchg, WTSKlinePeriod period)
{
	WtLMDBMap* the_map = NULL;
	std::string subdir;
	if (period == KP_Minute1)
	{
		the_map = &_exchg_m1_dbs;
		subdir = "min1";
	}
	else if (period == KP_Minute5)
	{
		the_map = &_exchg_m5_dbs;
		subdir = "min5";
	}
	else if (period == KP_DAY)
	{
		the_map = &_exchg_d1_dbs;
		subdir = "day";
	}
	else
		return std::move(WtLMDBPtr());

	auto it = the_map->find(exchg);
	if (it != the_map->end())
		return std::move(it->second);

	WtLMDBPtr dbPtr(new WtLMDB(false));
	std::string path = StrUtil::printf("%s%s/%s/", _base_dir.c_str(), subdir.c_str(), exchg);
	boost::filesystem::create_directories(path);
	if(!dbPtr->open(path.c_str(), _kline_mapsize))
	{
		if (_sink) pipe_writer_log(_sink, LL_ERROR, "Opening {} db at {} failed: {}", subdir, path, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}

	(*the_map)[exchg] = dbPtr;
	return std::move(dbPtr);
}

WtDataWriterAD::WtLMDBPtr WtDataWriterAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		return std::move(it->second);

	WtLMDBPtr dbPtr(new WtLMDB(false));
	std::string path = StrUtil::printf("%sticks/%s/%s", _base_dir.c_str(), exchg, code);
	boost::filesystem::create_directories(path);
	if (!dbPtr->open(path.c_str(), _tick_mapsize))
	{
		if (_sink) pipe_writer_log(_sink, LL_ERROR, "Opening tick db at {} failed: %s", path, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}

	_tick_dbs[key] = dbPtr;
	return std::move(dbPtr);
}
