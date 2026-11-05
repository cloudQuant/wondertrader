/*!
 * \file ShmCaster.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 共享内存行情广播器实现
 */

#include "ShmCaster.h"
#include <atomic>
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../WTSTools/WTSLogger.h"

#include <exception>
#include <new>

/**
 * @brief 初始化共享内存广播器
 * @details 根据配置项创建或重置共享内存队列，并进行内存映射
 * @param cfg 配置项指针，包含"active"和"path"字段
 * @return bool 初始化是否成功
 */
bool ShmCaster::init(WTSVariant* cfg)
{
	// 检查配置项指针是否有效
	if (cfg == NULL)
		return false;

	// 检查广播器是否激活
	if (!cfg->getBoolean("active"))
		return false;

	const char* path = cfg->getCString("path");
	if (path == NULL || path[0] == '\0')
	{
		WTSLogger::error("ShmCaster initialization failed: path is empty");
		return false;
	}

	std::string newPath;
	MappedFilePtr mapfile;
	CastQueue* queue = NULL;
	try
	{
		newPath = path;
		// 每次启动都重置队列，但必须确认文件创建和扩容成功后再映射。
		BoostFile bf;
		if (!bf.create_or_open_file(path))
		{
			WTSLogger::error("ShmCaster initialization failed: cannot open {}", path);
			return false;
		}
		if (!bf.truncate_file(sizeof(CastQueue)))
		{
			WTSLogger::error("ShmCaster initialization failed: cannot size {}", path);
			return false;
		}
		bf.close_file();

		// 先完整建立候选映射。失败时保留当前有效映射，避免悬垂的 _queue。
		mapfile.reset(new BoostMappingFile);
		if (!mapfile->map(path) || mapfile->addr() == NULL
			|| mapfile->size() < sizeof(CastQueue))
		{
			WTSLogger::error("ShmCaster initialization failed: cannot map {}", path);
			return false;
		}

		queue = new(mapfile->addr()) CastQueue();

		// 设置进程标识，根据不同平台选择不同的获取方式
#ifdef _MSC_VER
		queue->_pid = _getpid();
#else
		queue->_pid = getpid();
#endif

	}
	catch (const std::exception& e)
	{
		WTSLogger::error("ShmCaster initialization failed for {}: {}", path, e.what());
	}
	catch (...)
	{
		WTSLogger::error("ShmCaster initialization failed for {}", path);
	}

	if (mapfile == NULL || queue == NULL)
		return false;

	_path.swap(newPath);
	_mapfile = mapfile;
	_queue = queue;
	_inited = true;
	WTSLogger::info("ShmCaster initialized @ {}", _path.c_str());
	return true;
}

/**
 * @brief 广播股票行情数据
 * @details 将Tick数据写入共享内存队列中，并更新可读指针
 * @param curTick Tick数据指针
 */
void ShmCaster::broadcast(WTSTickData* curTick)
{
	// 参数有效性检查，确保广播器已初始化且数据非空
	if (curTick == NULL || _queue == NULL || !_inited)
		return;

	/*
	 *	先移动写的下标，然后写入数据
	 *	写完了以后，再移动读的下标
	 */
	// 原子操作获取并且递增写指针
	std::lock_guard<std::mutex> writeLock(_write_mtx);	// K21: serialize in-process producers
	uint64_t wIdx = _queue->_writable++;
	// 计算实际的环形缓冲区下标
	uint64_t realIdx = wIdx % _queue->_capacity;
	// 设置数据类型为Tick数据
	_queue->_items[realIdx]._type = 0;
	// 将Tick数据复制到队列中的相应位置
	memcpy(&_queue->_items[realIdx]._tick, &curTick->getTickStruct(), sizeof(WTSTickStruct));
	// 更新可读指针，让其他进程知道有新数据可读
	// K21: the payload must be visible before the readable index is published.
	std::atomic_thread_fence(std::memory_order_release);
	_queue->_readable = wIdx;
}

/**
 * @brief 广播委托队列数据
 * @details 将委托队列数据写入共享内存队列中，并更新可读指针
 * @param curOrdQue 委托队列数据指针
 */
void ShmCaster::broadcast(WTSOrdQueData* curOrdQue)
{
	// 参数有效性检查，确保广播器已初始化且数据非空
	if (curOrdQue == NULL || _queue == NULL || !_inited)
		return;

	/*
	 *	先移动写的下标，然后写入数据
	 *	写完了以后，再移动读的下标
	 */
	// 原子操作获取并且递增写指针
	std::lock_guard<std::mutex> writeLock(_write_mtx);	// K21: serialize in-process producers
	uint64_t wIdx = _queue->_writable++;
	// 计算实际的环形缓冲区下标
	uint64_t realIdx = wIdx % _queue->_capacity;
	// 设置数据类型为委托队列数据
	_queue->_items[realIdx]._type = 1;
	// 将委托队列数据复制到队列中的相应位置
	memcpy(&_queue->_items[realIdx]._queue, &curOrdQue->getOrdQueStruct(), sizeof(WTSOrdQueStruct));
	// 更新可读指针，让其他进程知道有新数据可读
	// K21: the payload must be visible before the readable index is published.
	std::atomic_thread_fence(std::memory_order_release);
	_queue->_readable = wIdx;
}

/**
 * @brief 广播逆序委托明细数据
 * @details 将逆序委托明细数据写入共享内存队列中，并更新可读指针
 * @param curOrdDtl 逆序委托明细数据指针
 */
void ShmCaster::broadcast(WTSOrdDtlData* curOrdDtl)
{
	// 参数有效性检查，确保广播器已初始化且数据非空
	if (curOrdDtl == NULL || _queue == NULL || !_inited)
		return;

	/*
	 *	先移动写的下标，然后写入数据
	 *	写完了以后，再移动读的下标
	 */
	// 原子操作获取并且递增写指针
	std::lock_guard<std::mutex> writeLock(_write_mtx);	// K21: serialize in-process producers
	uint64_t wIdx = _queue->_writable++;
	// 计算实际的环形缓冲区下标
	uint64_t realIdx = wIdx % _queue->_capacity;
	// 设置数据类型为逆序委托明细数据
	_queue->_items[realIdx]._type = 2;
	// 将逆序委托明细数据复制到队列中的相应位置
	memcpy(&_queue->_items[realIdx]._order, &curOrdDtl->getOrdDtlStruct(), sizeof(WTSOrdDtlStruct));
	// 更新可读指针，让其他进程知道有新数据可读
	// K21: the payload must be visible before the readable index is published.
	std::atomic_thread_fence(std::memory_order_release);
	_queue->_readable = wIdx;
}

/**
 * @brief 广播逆序成交数据
 * @details 将逆序成交数据写入共享内存队列中，并更新可读指针
 * @param curTrans 逆序成交数据指针
 */
void ShmCaster::broadcast(WTSTransData* curTrans)
{
	// 参数有效性检查，确保广播器已初始化且数据非空
	if (curTrans == NULL || _queue == NULL || !_inited)
		return;

	/*
	 *	先移动写的下标，然后写入数据
	 *	写完了以后，再移动读的下标
	 */
	// 原子操作获取并且递增写指针
	std::lock_guard<std::mutex> writeLock(_write_mtx);	// K21: serialize in-process producers
	uint64_t wIdx = _queue->_writable++;
	// 计算实际的环形缓冲区下标
	uint64_t realIdx = wIdx % _queue->_capacity;
	// 设置数据类型为逆序成交数据
	_queue->_items[realIdx]._type = 3;
	// 将逆序成交数据复制到队列中的相应位置
	memcpy(&_queue->_items[realIdx]._trans, &curTrans->getTransStruct(), sizeof(WTSTransStruct));
	// 更新可读指针，让其他进程知道有新数据可读
	// K21: the payload must be visible before the readable index is published.
	std::atomic_thread_fence(std::memory_order_release);
	_queue->_readable = wIdx;
}
