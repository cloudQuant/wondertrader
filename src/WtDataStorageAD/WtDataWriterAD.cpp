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

#include <boost/filesystem.hpp>
#include <filesystem>
#ifdef WT_DATAWRITERAD_TEST_SEAMS
#include <atomic>
#endif
#include <exception>
#include <limits>
namespace fs = boost::filesystem;

using namespace std;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
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

extern "C"
{
	EXPORT_FLAG IDataWriter* createWriter()
	{
		IDataWriter* ret = new WtDataWriterAD();
		return ret;
	}

	EXPORT_FLAG void deleteWriter(IDataWriter* &writer)
	{
		if (writer != NULL)
		{
			delete writer;
			writer = NULL;
		}
	}
};

static const uint32_t CACHE_SIZE_STEP_AD = 400;

namespace
{
	bool getRTCacheRequiredSize(uint32_t capacity, std::size_t itemSize, uint64_t& requiredSize)
	{
		const uint64_t headerSize = sizeof(RTBlockHeader);
		if (itemSize == 0 || capacity > (std::numeric_limits<uint64_t>::max)() / itemSize)
			return false;

		const uint64_t itemsSize = static_cast<uint64_t>(capacity) * itemSize;
		if (itemsSize > (std::numeric_limits<uint64_t>::max)() - headerSize)
			return false;

		requiredSize = headerSize + itemsSize;
		return requiredSize <= (std::numeric_limits<std::size_t>::max)();
	}

	bool validateRTCacheHeader(const RTBlockHeader& header, uint64_t fileSize,
		std::size_t itemSize, const std::string& filename, const char* cacheName,
		IDataWriterSink* sink)
	{
		if (std::memcmp(header._blk_flag, BLK_FLAG, FLAG_SIZE) != 0)
		{
			pipe_writer_log(sink, LL_ERROR, "Invalid {} cache file {}: bad block flag", cacheName, filename);
			return false;
		}
		if (header._type != BT_RT_Cache)
		{
			pipe_writer_log(sink, LL_ERROR, "Invalid {} cache file {}: unexpected block type {}",
				cacheName, filename, header._type);
			return false;
		}
		if (header._version != 1)
		{
			pipe_writer_log(sink, LL_ERROR, "Invalid {} cache file {}: unsupported version {}",
				cacheName, filename, header._version);
			return false;
		}
		if (header._size > header._capacity)
		{
			pipe_writer_log(sink, LL_ERROR,
				"Invalid {} cache file {}: size {} exceeds capacity {}",
				cacheName, filename, header._size, header._capacity);
			return false;
		}
		if (fileSize > (std::numeric_limits<std::size_t>::max)())
		{
			pipe_writer_log(sink, LL_ERROR, "Invalid {} cache file {}: file length is not representable",
				cacheName, filename);
			return false;
		}

		uint64_t requiredSize = 0;
		if (!getRTCacheRequiredSize(header._capacity, itemSize, requiredSize))
		{
			pipe_writer_log(sink, LL_ERROR, "Invalid {} cache file {}: capacity {} is not representable",
				cacheName, filename, header._capacity);
			return false;
		}
		if (requiredSize > fileSize)
		{
			pipe_writer_log(sink, LL_ERROR,
				"Invalid {} cache file {}: capacity {} requires {} bytes, file has {}",
				cacheName, filename, header._capacity, requiredSize, fileSize);
			return false;
		}
		return true;
	}

	bool validateExistingRTCacheFile(const std::string& filename, const char* cacheName,
		std::size_t itemSize, IDataWriterSink* sink, bool& isNew)
	{
		std::error_code ec;
		const bool exists = std::filesystem::exists(filename, ec);
		if (ec)
		{
			pipe_writer_log(sink, LL_ERROR, "Cannot inspect {} cache file {}: {}",
				cacheName, filename, ec.message());
			return false;
		}
		isNew = !exists;
		if (isNew)
			return true;

		BoostFile file;
		if (!file.open_existing_file(filename.c_str(), bip::read_only))
		{
			pipe_writer_log(sink, LL_ERROR, "Cannot open {} cache file {} for validation", cacheName, filename);
			return false;
		}
		const uint64_t fileSize = file.get_file_size();
		if (fileSize < sizeof(RTBlockHeader))
		{
			pipe_writer_log(sink, LL_ERROR, "Invalid {} cache file {}: header is truncated",
				cacheName, filename);
			return false;
		}

		RTBlockHeader header = {};
		if (!file.read_file(&header, sizeof(header)))
		{
			pipe_writer_log(sink, LL_ERROR, "Cannot read {} cache header from {}", cacheName, filename);
			return false;
		}
		return validateRTCacheHeader(header, fileSize, itemSize, filename, cacheName, sink);
	}
}

#ifdef WT_DATAWRITERAD_TEST_SEAMS
namespace
{
	// Inert unless explicitly armed by the writer regression tests. This keeps
	// remap failure injectable after truncate without changing normal writer behavior.
	std::atomic<bool> g_failNextRTCacheRemapForTest(false);
	using WriterDbCacheObserverForTest = void (*)(const char*, const char*, const void*, bool);
	std::atomic<WriterDbCacheObserverForTest> g_writerDbCacheObserverForTest(nullptr);
	using RTCachePreMapHookForTest = void (*)(const char*);
	std::atomic<RTCachePreMapHookForTest> g_rtCachePreMapHookForTest(nullptr);

	void observeWriterDbCacheForTest(const char* type, const char* key, const void* handle, bool cacheHit)
	{
		WriterDbCacheObserverForTest observer = g_writerDbCacheObserverForTest.load(std::memory_order_acquire);
		if (observer != nullptr)
			observer(type, key, handle, cacheHit);
	}

	void runRTCachePreMapHookForTest(const char* filename)
	{
		RTCachePreMapHookForTest hook = g_rtCachePreMapHookForTest.exchange(nullptr, std::memory_order_acq_rel);
		if (hook != nullptr)
			hook(filename);
	}
}

extern "C" void wt_data_writer_ad_fail_next_remap_for_test()
{
	g_failNextRTCacheRemapForTest.store(true, std::memory_order_release);
}

extern "C" void wt_data_writer_ad_set_db_cache_observer_for_test(
	void (*observer)(const char*, const char*, const void*, bool))
{
	g_writerDbCacheObserverForTest.store(observer, std::memory_order_release);
}

extern "C" void wt_data_writer_ad_set_cache_pre_map_hook_for_test(void (*hook)(const char*))
{
	g_rtCachePreMapHookForTest.store(hook, std::memory_order_release);
}
#endif


WtDataWriterAD::WtDataWriterAD()
	: _tick_cache_block(nullptr)
	, _async_task(false)
	, _log_group_size(1000)
	, _terminated(false)
	, _disable_tick(false)
	, _disable_min1(false)
	, _disable_min5(false)
	, _disable_day(false)
	, _tick_mapsize(16*1024*1024)
	, _kline_mapsize(8*1024*1024)
{
}


WtDataWriterAD::~WtDataWriterAD()
{
}

bool WtDataWriterAD::init(WTSVariant* params, IDataWriterSink* sink)
{
	IDataWriter::init(params, sink);

	_bd_mgr = sink->getBDMgr();
	_log_group_size = params->getUInt32("groupsize", 1000);
	if (_log_group_size == 0)
	{
		pipe_writer_log(_sink, LL_ERROR, "Invalid writer groupsize: expected a positive value");
		return false;
	}

	_base_dir = StrUtil::standardisePath(params->getCString("path"));
	if (!StdFile::exists(_base_dir.c_str()))
		fs::create_directories(_base_dir.c_str());

	_cache_file_tick = "cache_tick.dmb";
	_m1_cache._filename = "cache_m1.dmb";
	_m5_cache._filename = "cache_m5.dmb";
	_d1_cache._filename = "cache_d1.dmb";

	_disable_tick = params->getBoolean("disabletick");
	_disable_min1 = params->getBoolean("disablemin1");
	_disable_min5 = params->getBoolean("disablemin5");
	_disable_day = params->getBoolean("disableday");
	_async_task = params->getBoolean("async");

	if (params->has("tickmapsize"))
		_tick_mapsize = params->getUInt32("tickmapsize");

	if (params->has("klinemapsize"))
		_kline_mapsize = params->getUInt32("klinemapsize");

	try
	{
		if (!loadCache())
			return false;
	}
	catch (const std::exception& ex)
	{
		pipe_writer_log(_sink, LL_ERROR, "Failed to load AD writer cache: {}", ex.what());
		return false;
	}
	catch (...)
	{
		pipe_writer_log(_sink, LL_ERROR, "Failed to load AD writer cache due to an unknown error");
		return false;
	}
	if (_async_task)
	{
		try
		{
			_task_thrd.reset(new StdThread([this]() {
				for (;;)
				{
					TaskInfo task;
					{
						StdUniqueLock lck(_task_mtx);
						_task_cond.wait(lck, [this]() { return _terminated || !_tasks.empty(); });
						if (_tasks.empty())
						{
							if (_terminated)
								return;
							continue;
						}

						task = std::move(_tasks.front());
						_tasks.pop();
					}

					try
					{
						task();
					}
					catch (const std::exception& ex)
					{
						pipe_writer_log(_sink, LL_ERROR, "Async writer task failed: {}", ex.what());
					}
					catch (...)
					{
						pipe_writer_log(_sink, LL_ERROR, "Async writer task failed with an unknown exception");
					}
				}
			}));
		}
		catch (const std::exception& ex)
		{
			pipe_writer_log(_sink, LL_ERROR, "Async writer thread could not start: {}", ex.what());
			return false;
		}
	}

	return true;
}

void WtDataWriterAD::release()
{
	{
		StdUniqueLock lck(_task_mtx);
		_terminated = true;
	}
	_task_cond.notify_all();
	if (_task_thrd && _task_thrd->joinable())
	{
		_task_thrd->join();
	}
}

bool WtDataWriterAD::loadCache()
{
	bool bNewTick = false;
	bool bNewM1 = false;
	bool bNewM5 = false;
	bool bNewD1 = false;
	const std::string tickFilename = _base_dir + _cache_file_tick;
	const std::string m1Filename = _base_dir + _m1_cache._filename;
	const std::string m5Filename = _base_dir + _m5_cache._filename;
	const std::string d1Filename = _base_dir + _d1_cache._filename;

	// Validate all existing files before mapping headers or creating any
	// missing cache. A malformed file therefore fails init without rewriting peers.
	if (!validateExistingRTCacheFile(tickFilename, "tick", sizeof(TickCacheItem), _sink, bNewTick) ||
		!validateExistingRTCacheFile(m1Filename, "m1", sizeof(BarCacheItem), _sink, bNewM1) ||
		!validateExistingRTCacheFile(m5Filename, "m5", sizeof(BarCacheItem), _sink, bNewM5) ||
		!validateExistingRTCacheFile(d1Filename, "day", sizeof(BarCacheItem), _sink, bNewD1))
		return false;

#ifdef WT_DATAWRITERAD_TEST_SEAMS
	runRTCachePreMapHookForTest(tickFilename.c_str());
#endif

	if (_tick_cache_file == NULL)
	{
		if (bNewTick)
		{
			uint64_t uSize = sizeof(RTTickCache) + sizeof(TickCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			if (!bf.create_new_file(tickFilename.c_str()) || !bf.truncate_file((std::size_t)uSize))
			{
				pipe_writer_log(_sink, LL_ERROR, "Cannot create tick cache file {}", tickFilename);
				return false;
			}
			bf.close_file();
		}

		_tick_cache_file.reset(new BoostMappingFile);
		if (!_tick_cache_file->map(tickFilename.c_str()))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot map tick cache file {}", tickFilename);
			_tick_cache_file.reset();
			return false;
		}
		void* mappedAddr = _tick_cache_file->addr();
		const std::size_t mappedSize = _tick_cache_file->size();
		if (mappedAddr == NULL || mappedSize < sizeof(RTBlockHeader) ||
			(bNewTick && mappedSize < sizeof(RTTickCache)))
		{
			pipe_writer_log(_sink, LL_ERROR, "Mapped tick cache file {} is shorter than its RT header", tickFilename);
			_tick_cache_file.reset();
			return false;
		}
		_tick_cache_block = (RTTickCache*)mappedAddr;
		if (!bNewTick && !validateRTCacheHeader(*_tick_cache_block, _tick_cache_file->size(),
			sizeof(TickCacheItem), tickFilename, "tick", _sink))
		{
			_tick_cache_block = NULL;
			_tick_cache_file.reset();
			return false;
		}

		if (bNewTick)
		{
			memset(_tick_cache_block, 0, _tick_cache_file->size());

			_tick_cache_block->_capacity = CACHE_SIZE_STEP_AD;
			_tick_cache_block->_type = BT_RT_Cache;
			_tick_cache_block->_size = 0;
			_tick_cache_block->_version = 1;
			strcpy(_tick_cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			for (uint32_t i = 0; i < _tick_cache_block->_size; i++)
			{
				const TickCacheItem& item = _tick_cache_block->_items[i];
				std::string key = fmtutil::format<64>("{}.{}", item._tick.exchg, item._tick.code);
				_tick_cache_idx[key] = i;
			}
		}
	}

	if (_m1_cache.empty())
	{
		if (bNewM1)
		{
			uint64_t uSize = sizeof(RTBarCache) + sizeof(BarCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			if (!bf.create_new_file(m1Filename.c_str()) || !bf.truncate_file((std::size_t)uSize))
			{
				pipe_writer_log(_sink, LL_ERROR, "Cannot create m1 cache file {}", m1Filename);
				return false;
			}
			bf.close_file();
		}

		_m1_cache._file_ptr.reset(new BoostMappingFile);
		if (!_m1_cache._file_ptr->map(m1Filename.c_str()))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot map m1 cache file {}", m1Filename);
			_m1_cache._file_ptr.reset();
			return false;
		}
		void* mappedAddr = _m1_cache._file_ptr->addr();
		const std::size_t mappedSize = _m1_cache._file_ptr->size();
		if (mappedAddr == NULL || mappedSize < sizeof(RTBlockHeader) ||
			(bNewM1 && mappedSize < sizeof(RTBarCache)))
		{
			pipe_writer_log(_sink, LL_ERROR, "Mapped m1 cache file {} is shorter than its RT header", m1Filename);
			_m1_cache._file_ptr.reset();
			return false;
		}
		_m1_cache._cache_block = (RTBarCache*)mappedAddr;
		if (!bNewM1 && !validateRTCacheHeader(*_m1_cache._cache_block, _m1_cache._file_ptr->size(),
			sizeof(BarCacheItem), m1Filename, "m1", _sink))
		{
			_m1_cache._cache_block = NULL;
			_m1_cache._file_ptr.reset();
			return false;
		}

		if (bNewM1)
		{
			memset(_m1_cache._cache_block, 0, _m1_cache._file_ptr->size());

			_m1_cache._cache_block->_capacity = CACHE_SIZE_STEP_AD;
			_m1_cache._cache_block->_type = BT_RT_Cache;
			_m1_cache._cache_block->_size = 0;
			_m1_cache._cache_block->_version = 1;
			strcpy(_m1_cache._cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			for (uint32_t i = 0; i < _m1_cache._cache_block->_size; i++)
			{
				const BarCacheItem& item = _m1_cache._cache_block->_items[i];
				std::string key = fmtutil::format<64>("{}.{}", item._exchg, item._code);
				_m1_cache._idx[key] = i;
			}
		}
	}

	if (_m5_cache.empty())
	{
		if (bNewM5)
		{
			uint64_t uSize = sizeof(RTBarCache) + sizeof(BarCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			if (!bf.create_new_file(m5Filename.c_str()) || !bf.truncate_file((std::size_t)uSize))
			{
				pipe_writer_log(_sink, LL_ERROR, "Cannot create m5 cache file {}", m5Filename);
				return false;
			}
			bf.close_file();
		}

		_m5_cache._file_ptr.reset(new BoostMappingFile);
		if (!_m5_cache._file_ptr->map(m5Filename.c_str()))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot map m5 cache file {}", m5Filename);
			_m5_cache._file_ptr.reset();
			return false;
		}
		void* mappedAddr = _m5_cache._file_ptr->addr();
		const std::size_t mappedSize = _m5_cache._file_ptr->size();
		if (mappedAddr == NULL || mappedSize < sizeof(RTBlockHeader) ||
			(bNewM5 && mappedSize < sizeof(RTBarCache)))
		{
			pipe_writer_log(_sink, LL_ERROR, "Mapped m5 cache file {} is shorter than its RT header", m5Filename);
			_m5_cache._file_ptr.reset();
			return false;
		}
		_m5_cache._cache_block = (RTBarCache*)mappedAddr;
		if (!bNewM5 && !validateRTCacheHeader(*_m5_cache._cache_block, _m5_cache._file_ptr->size(),
			sizeof(BarCacheItem), m5Filename, "m5", _sink))
		{
			_m5_cache._cache_block = NULL;
			_m5_cache._file_ptr.reset();
			return false;
		}

		if (bNewM5)
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
				std::string key = fmtutil::format<64>("{}.{}", item._exchg, item._code);
				_m5_cache._idx[key] = i;
			}
		}
	}

	if (_d1_cache.empty())
	{
		if (bNewD1)
		{
			uint64_t uSize = sizeof(RTBarCache) + sizeof(BarCacheItem) * CACHE_SIZE_STEP_AD;
			BoostFile bf;
			if (!bf.create_new_file(d1Filename.c_str()) || !bf.truncate_file((std::size_t)uSize))
			{
				pipe_writer_log(_sink, LL_ERROR, "Cannot create day cache file {}", d1Filename);
				return false;
			}
			bf.close_file();
		}

		_d1_cache._file_ptr.reset(new BoostMappingFile);
		if (!_d1_cache._file_ptr->map(d1Filename.c_str()))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot map day cache file {}", d1Filename);
			_d1_cache._file_ptr.reset();
			return false;
		}
		void* mappedAddr = _d1_cache._file_ptr->addr();
		const std::size_t mappedSize = _d1_cache._file_ptr->size();
		if (mappedAddr == NULL || mappedSize < sizeof(RTBlockHeader) ||
			(bNewD1 && mappedSize < sizeof(RTBarCache)))
		{
			pipe_writer_log(_sink, LL_ERROR, "Mapped day cache file {} is shorter than its RT header", d1Filename);
			_d1_cache._file_ptr.reset();
			return false;
		}
		_d1_cache._cache_block = (RTBarCache*)mappedAddr;
		if (!bNewD1 && !validateRTCacheHeader(*_d1_cache._cache_block, _d1_cache._file_ptr->size(),
			sizeof(BarCacheItem), d1Filename, "day", _sink))
		{
			_d1_cache._cache_block = NULL;
			_d1_cache._file_ptr.reset();
			return false;
		}

		if (bNewD1)
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
				std::string key = fmtutil::format<64>("{}.{}", item._exchg, item._code);
				_d1_cache._idx[key] = i;
			}
		}
	}
	return true;
}

template<typename HeaderType, typename T>
bool WtDataWriterAD::resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount)
{
	if (mfPtr == NULL || !mfPtr->valid())
		return false;

	//调用该函数之前,应该已经申请了写锁了
	RTBlockHeader* tBlock = (RTBlockHeader*)mfPtr->addr();
	if (tBlock == NULL)
		return false;
	if (tBlock->_capacity >= nCount)
		return true;

	const char* filename = mfPtr->filename();
	try
	{
		const uint64_t uNewSize = sizeof(HeaderType) + static_cast<uint64_t>(sizeof(T)) * nCount;
		if (uNewSize > (uint64_t)(std::numeric_limits<std::size_t>::max)())
		{
			pipe_writer_log(_sink, LL_ERROR, "Expanded RT cache size is not representable for {}[{}]", filename, nCount);
			return false;
		}

		BoostFile f;
		if (!f.open_existing_file(filename) || !f.truncate_file((std::size_t)uNewSize))
		{
			pipe_writer_log(_sink, LL_ERROR, "Failed to extend RT cache file {} to {} bytes", filename, uNewSize);
			return false;
		}
		f.close_file();

		BoostMFPtr newMf(new BoostMappingFile());
#ifdef WT_DATAWRITERAD_TEST_SEAMS
		if (g_failNextRTCacheRemapForTest.exchange(false, std::memory_order_acq_rel))
		{
			pipe_writer_log(_sink, LL_ERROR, "Failed to remap expanded RT cache file {}", filename);
			return false;
		}
#endif
		if (!newMf->map(filename))
		{
			pipe_writer_log(_sink, LL_ERROR, "Failed to remap expanded RT cache file {}", filename);
			return false;
		}

		RTBlockHeader* newBlock = (RTBlockHeader*)newMf->addr();
		if (newBlock == NULL || newMf->size() < (std::size_t)uNewSize)
		{
			pipe_writer_log(_sink, LL_ERROR, "Expanded RT cache mapping is too small for {}[{}]", filename, nCount);
			return false;
		}

		// Publish the new capacity only after the file has been extended and remapped.
		newBlock->_capacity = nCount;
		mfPtr.swap(newMf);
		return true;
	}
	catch (const std::exception& ex)
	{
		pipe_writer_log(_sink, LL_ERROR, "Exception occured while expanding RT cache file {}: {}", filename, ex.what());
		return false;
	}
	catch (...)
	{
		pipe_writer_log(_sink, LL_ERROR, "Unknown exception occured while expanding RT cache file {}", filename);
		return false;
	}
}

bool WtDataWriterAD::writeTick(WTSTickData* curTick, uint32_t procFlag)
{
	if (_log_group_size == 0)
		return false;

	if (curTick == NULL)
		return false;

	curTick->retain();
	std::shared_ptr<WTSTickData> tick_ref(curTick, [](WTSTickData* tick) { tick->release(); });
	return pushTask([this, tick_ref, procFlag](){
		WTSTickData* curTick = tick_ref.get();

		do
		{
			WTSContractInfo* ct = curTick->getContractInfo();
			if(ct == NULL)
				break;

			WTSCommodityInfo* commInfo = ct->getCommInfo();

			//再根据状态过滤
			if (!_sink->canSessionReceive(commInfo->getSession()))
				break;

			//先更新缓存
			if (!updateTickCache(ct, curTick, procFlag))
				break;

			//写到tick缓存
			if(!_disable_tick)
				pipeToTicks(ct, curTick);

			//写到K线缓存
			updateBarCache(ct, curTick);

			_sink->broadcastTick(curTick);

			static wt_hashmap<std::string, uint64_t> _tcnt_map;
			static StdUniqueMutex _tcnt_mtx;
			uint64_t tick_count = 0;
			{
				StdUniqueLock lock(_tcnt_mtx);
				tick_count = ++_tcnt_map[curTick->exchg()];
			}
			if (tick_count % _log_group_size == 0)
			{
				pipe_writer_log(_sink, LL_INFO, "{} ticks received from exchange {}", tick_count, curTick->exchg());
			}
		} while (false);
	});
}

bool WtDataWriterAD::pushTask(TaskInfo task)
{
	if (!_async_task)
	{
		task();
		return true;
	}

	{
		StdUniqueLock lck(_task_mtx);
		if (_terminated || !_task_thrd)
			return false;
		_tasks.push(std::move(task));
	}
	_task_cond.notify_one();
	return true;
}

void WtDataWriterAD::pipeToTicks(WTSContractInfo* ct, WTSTickData* curTick)
{
	//直接落地
	WtLMDBPtr db = get_t_db(ct->getExchg(), ct->getCode());
	if (db)
	{
		//这里要把时间转成便宜时间，并用交易日作为date
		//这样可以根据交易日筛选历史tick数据
		uint32_t actTime = curTick->actiontime();
		uint32_t offTime = ct->getCommInfo()->getSessionInfo()->offsetTime(actTime / 100000, true) * 100000 + actTime % 100000;

		LMDBHftKey key(ct->getExchg(), ct->getCode(), curTick->tradingdate(), offTime);
		WtLMDBQuery query(*db);
		if (!query.put_and_commit((void*)&key, sizeof(key), &curTick->getTickStruct(), sizeof(WTSTickStruct)))
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe tick of {} to db failed: {}", ct->getFullCode(), db->errmsg());
		}
	}

	for(auto& item : _dumpers)
	{
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		bool bSucc = dumper->dumpHisTicks(ct->getFullCode(), curTick->tradingdate(), &curTick->getTickStruct(), 1);
		if (!bSucc)
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe tick data of {} via extended dumper {} failed", ct->getFullCode(), id);
		}
	}
}

void WtDataWriterAD::pipeToDayBars(WTSContractInfo* ct, const WTSBarStruct& bar)
{
	//直接落地
	WtLMDBPtr db = get_k_db(ct->getExchg(), KP_DAY);
	if (db)
	{
		LMDBBarKey key(ct->getExchg(), ct->getCode(), bar.date);
		WtLMDBQuery query(*db);
		if (!query.put_and_commit((void*)&key, sizeof(key), (void*)&bar, sizeof(WTSBarStruct)))
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe day bar @ {} of {} to db failed", bar.date, ct->getFullCode());
		}
		else
		{
			pipe_writer_log(_sink, LL_DEBUG, "day bar @ {} of {} piped to db", bar.date, ct->getFullCode());
		}
	}

	for (auto& item : _dumpers)
	{
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		bool bSucc = dumper->dumpHisBars(ct->getFullCode(), "d1", (WTSBarStruct*)&bar, 1);
		if (!bSucc)
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe day bar @ {} of {} via extended dumper {} failed", bar.date, ct->getFullCode(), id);
		}
	}
}

void WtDataWriterAD::pipeToM1Bars(WTSContractInfo* ct, const WTSBarStruct& bar)
{
	//直接落地
	WtLMDBPtr db = get_k_db(ct->getExchg(), KP_Minute1);
	if(db)
	{
		LMDBBarKey key(ct->getExchg(), ct->getCode(), (uint32_t)bar.time);
		WtLMDBQuery query(*db);
		if(!query.put_and_commit((void*)&key, sizeof(key), (void*)&bar, sizeof(WTSBarStruct)))
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe m1 bar @ {} of {} to db failed", bar.time, ct->getFullCode());
		}
		else
		{
			pipe_writer_log(_sink, LL_DEBUG, "m1 bar @ {} of {} piped to db", bar.time, ct->getFullCode());
		}
	}

	for (auto& item : _dumpers)
	{
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		bool bSucc = dumper->dumpHisBars(ct->getFullCode(), "m1", (WTSBarStruct*)&bar, 1);
		if (!bSucc)
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe m1 bar @ {} of {} via extended dumper {} failed", bar.time, ct->getFullCode(), id);
		}
	}
}

void WtDataWriterAD::pipeToM5Bars(WTSContractInfo* ct, const WTSBarStruct& bar)
{
	//直接落地
	WtLMDBPtr db = get_k_db(ct->getExchg(), KP_Minute5);
	if (db)
	{
		LMDBBarKey key(ct->getExchg(), ct->getCode(), (uint32_t)bar.time);
		WtLMDBQuery query(*db);
		if (!query.put_and_commit((void*)&key, sizeof(key), (void*)&bar, sizeof(bar)))
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe m5 bar @ {} of {} to db failed", bar.time, ct->getFullCode());
		}
		else
		{
			pipe_writer_log(_sink, LL_DEBUG, "m5 bar @ {} of {} piped to db", bar.time, ct->getFullCode());
		}
	}

	for (auto& item : _dumpers)
	{
		const char* id = item.first.c_str();
		IHisDataDumper* dumper = item.second;
		if (dumper == NULL)
			continue;

		bool bSucc = dumper->dumpHisBars(ct->getFullCode(), "m5", (WTSBarStruct*)&bar, 1);
		if (!bSucc)
		{
			pipe_writer_log(_sink, LL_ERROR, "pipe m5 bar @ {} of {} via extended dumper {} failed", bar.time, ct->getFullCode(), id);
		}
	}
}

void WtDataWriterAD::updateBarCache(WTSContractInfo* ct, WTSTickData* curTick)
{
	uint32_t uDate = curTick->actiondate();
	WTSSessionInfo* sInfo = _bd_mgr->getSessionByCode(curTick->code(), curTick->exchg());
	uint32_t curTime = curTick->actiontime() / 100000;

	uint32_t minutes = sInfo->timeToMinutes(curTime, false);
	if (minutes == INVALID_UINT32)
		return;

	//当秒数为0,要专门处理,比如091500000,这笔tick要算作0915的
	//如果是小节结束,要算作小节结束那一分钟,因为经常会有超过结束时间的价格进来,如113000500
	//不能同时处理,所以用or	
	if (sInfo->isLastOfSection(curTime))
	{
		minutes--;
	}

	std::string key = fmtutil::format<64>("{}.{}", curTick->exchg(), curTick->code());
	auto prepareBarCacheItem = [this, &key](RTBarCacheWrapper& cache, const char* period,
		uint32_t& idx, bool& bNewCode) -> bool {
		bNewCode = false;
		wt_hashmap<std::string, uint32_t>::iterator found = cache._idx.find(key);
		if (found != cache._idx.end())
		{
			idx = found->second;
			return true;
		}

		idx = cache._cache_block->_size;
		if (idx == UINT32_MAX)
		{
			pipe_writer_log(_sink, LL_ERROR, "{} bar cache size is exhausted; skipping {}", period, key);
			return false;
		}

		const uint32_t requiredSize = idx + 1;
		const uint32_t capacity = cache._cache_block->_capacity;
		if (requiredSize >= capacity)
		{
			if (capacity > UINT32_MAX - CACHE_SIZE_STEP_AD)
			{
				pipe_writer_log(_sink, LL_ERROR, "{} bar cache capacity overflow; skipping {}", period, key);
				return false;
			}

			const uint32_t newCapacity = capacity + CACHE_SIZE_STEP_AD;
			if (!resizeRTBlock<RTBarCache, BarCacheItem>(cache._file_ptr, newCapacity))
			{
				pipe_writer_log(_sink, LL_ERROR, "{} bar cache expansion failed; skipping {}", period, key);
				return false;
			}

			cache._cache_block = (RTBarCache*)cache._file_ptr->addr();
			pipe_writer_log(_sink, LL_INFO, "{} cache resized to {} items", period, cache._cache_block->_capacity);
		}

		// Commit the index only after any required file extension and remap succeeded.
		cache._idx[key] = idx;
		bNewCode = true;
		return true;
	};

	//更新日线
	if (!_disable_day && _d1_cache._cache_block)
	{
		StdUniqueLock lock(_d1_cache._mtx);
		uint32_t idx = 0;
		bool bNewCode = false;
		if (!prepareBarCacheItem(_d1_cache, "day", idx, bNewCode))
		{
			// Keep this stream unchanged and continue updating the other bar caches.
		}
		else
		{
		BarCacheItem& item = (BarCacheItem&)_d1_cache._cache_block->_items[idx];
		if (bNewCode)
		{
			strcpy(item._exchg, curTick->exchg());
			strcpy(item._code, curTick->code());
		}
		WTSBarStruct* lastBar = &item._bar;

		//检查日期是否一致
		uint32_t barDate = curTick->tradingdate();

		bool bNewBar = false;
		if (lastBar == NULL || barDate > lastBar->date)
		{
			bNewBar = true;
		}

		WTSBarStruct* newBar = lastBar;
		if (bNewBar)
		{
			//这里要将lastBar往外写
			//如果是新的合约，说明还没数据，不往外写
			if (!bNewCode)
			{
				pipeToDayBars(ct, *lastBar);
			}

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
			if (decimal::eq(newBar->open, 0))
				newBar->open = curTick->price();

			if (decimal::eq(newBar->low, 0))
				newBar->low = curTick->price();
			else
				newBar->low = std::min(curTick->price(), newBar->low);

			newBar->close = curTick->price();
			newBar->high = max(curTick->price(), newBar->high);

			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			newBar->hold = curTick->openinterest();
			newBar->add += curTick->additional();
		}

		if (bNewCode)
			_d1_cache._cache_block->_size = idx + 1;
		}
	}

	//更新1分钟线
	if (!_disable_min1 && _m1_cache._cache_block)
	{
		StdUniqueLock lock(_m1_cache._mtx);
		uint32_t idx = 0;
		bool bNewCode = false;
		if (!prepareBarCacheItem(_m1_cache, "m1", idx, bNewCode))
		{
			// Keep this stream unchanged and continue updating the other bar caches.
		}
		else
		{
		BarCacheItem& item = (BarCacheItem&)_m1_cache._cache_block->_items[idx];
		if(bNewCode)
		{
			strcpy(item._exchg, curTick->exchg());
			strcpy(item._code, curTick->code());
		}
		WTSBarStruct* lastBar = &item._bar;


		//拼接1分钟线
		uint32_t barMins = minutes + 1;
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
			if (!bNewCode)
			{
				pipeToM1Bars(ct, *lastBar);

				uint32_t lastMins = sInfo->timeToMinutes(lastBar->time % 10000, false);
				if(lastMins > barMins)
				{
					//如果上一条K线的分钟数大于当前K线的分钟数
					//说明交易日换了
					//需要保存日线了
				}
			}

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
			if (decimal::eq(newBar->open, 0))
				newBar->open = curTick->price();

			if (decimal::eq(newBar->low, 0))
				newBar->low = curTick->price();
			else
				newBar->low = std::min(curTick->price(), newBar->low);

			newBar->close = curTick->price();
			newBar->high = max(curTick->price(), newBar->high);

			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			newBar->hold = curTick->openinterest();
			newBar->add += curTick->additional();
		}

		if (bNewCode)
			_m1_cache._cache_block->_size = idx + 1;
		}
	}

	//更新5分钟线
	if (!_disable_min5 && _m5_cache._cache_block)
	{
		StdUniqueLock lock(_m5_cache._mtx);
		uint32_t idx = 0;
		bool bNewCode = false;
		if (!prepareBarCacheItem(_m5_cache, "m5", idx, bNewCode))
		{
			// Keep this stream unchanged and continue processing the current tick.
		}
		else
		{
		BarCacheItem& item = (BarCacheItem&)_m5_cache._cache_block->_items[idx];
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

			newBar->vol += curTick->volume();
			newBar->money += curTick->turnover();
			newBar->hold = curTick->openinterest();
			newBar->add += curTick->additional();
		}

		if (bNewCode)
			_m5_cache._cache_block->_size = idx + 1;
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

	std::string key = fmtutil::format<64>("{}.{}", ct->getExchg(), ct->getCode());
	StdUniqueLock lock(_mtx_tick_cache);
	auto it = _tick_cache_idx.find(key);
	if (it == _tick_cache_idx.end())
		return NULL;

	uint32_t idx = it->second;
	TickCacheItem& item = _tick_cache_block->_items[idx];
	return WTSTickData::create(item._tick);
}

bool WtDataWriterAD::updateTickCache(WTSContractInfo* /*ct*/, WTSTickData* curTick, uint32_t procFlag)
{
	if (curTick == NULL || _tick_cache_block == NULL)
	{
		pipe_writer_log(_sink, LL_ERROR, "Tick cache data not initialized");
		return false;
	}

	StdUniqueLock lock(_mtx_tick_cache);
	std::string key = fmtutil::format<64>("{}.{}", curTick->exchg(), curTick->code());
	uint32_t idx = 0;
	bool bNewCode = false;
	TickCacheItem pendingItem = {};
	wt_hashmap<std::string, uint32_t>::iterator found = _tick_cache_idx.find(key);
	if (found == _tick_cache_idx.end())
	{
		idx = _tick_cache_block->_size;
		if (idx == UINT32_MAX)
		{
			pipe_writer_log(_sink, LL_ERROR, "Tick cache size is exhausted; skipping {}", key);
			return false;
		}

		const uint32_t requiredSize = idx + 1;
		const uint32_t capacity = _tick_cache_block->_capacity;
		if (requiredSize >= capacity)
		{
			if (capacity > UINT32_MAX - CACHE_SIZE_STEP_AD)
			{
				pipe_writer_log(_sink, LL_ERROR, "Tick cache capacity overflow; skipping {}", key);
				return false;
			}

			if (!resizeRTBlock<RTTickCache, TickCacheItem>(_tick_cache_file, capacity + CACHE_SIZE_STEP_AD))
			{
				pipe_writer_log(_sink, LL_ERROR, "Tick cache expansion failed; skipping {}", key);
				return false;
			}

			_tick_cache_block = (RTTickCache*)_tick_cache_file->addr();
			pipe_writer_log(_sink, LL_INFO, "Tick Cache resized to {} items", _tick_cache_block->_capacity);
		}
		bNewCode = true;
	}
	else
	{
		idx = found->second;
	}

	TickCacheItem& item = bNewCode ? pendingItem : _tick_cache_block->_items[idx];
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
				newTick.low = decimal::eq(item._tick.low, 0)
					? newTick.price
					: min(newTick.price, item._tick.low);

			memcpy(&item._tick, &newTick, sizeof(WTSTickStruct));
		}
	}

	if (bNewCode)
	{
		// Commit data, index, and size only after the cache entry has been fully prepared.
		_tick_cache_idx[key] = idx;
		_tick_cache_block->_items[idx] = pendingItem;
		_tick_cache_block->_size = idx + 1;
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
		return WtLMDBPtr();

	auto it = the_map->find(exchg);
	if (it != the_map->end())
	{
#ifdef WT_DATAWRITERAD_TEST_SEAMS
		observeWriterDbCacheForTest(subdir.c_str(), exchg, it->second.get(), true);
#endif
		return it->second;
	}

	WtLMDBPtr dbPtr(new WtLMDB(false));
	std::string path = fmtutil::format("{}{}/{}/", _base_dir.c_str(), subdir.c_str(), exchg);
	boost::filesystem::create_directories(path);
	if(!dbPtr->open(path.c_str(), _kline_mapsize))
	{
		if (_sink) pipe_writer_log(_sink, LL_ERROR, "Opening {} db at {} failed: {}", subdir, path, dbPtr->errmsg());
		return WtLMDBPtr();
	}

	(*the_map)[exchg] = dbPtr;
#ifdef WT_DATAWRITERAD_TEST_SEAMS
	observeWriterDbCacheForTest(subdir.c_str(), exchg, dbPtr.get(), false);
#endif
	return dbPtr;
}

WtDataWriterAD::WtLMDBPtr WtDataWriterAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = fmtutil::format<64>("{}.{}", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
	{
#ifdef WT_DATAWRITERAD_TEST_SEAMS
		observeWriterDbCacheForTest("tick", key.c_str(), it->second.get(), true);
#endif
		return it->second;
	}

	WtLMDBPtr dbPtr(new WtLMDB(false));
	std::string path = fmtutil::format("{}ticks/{}/{}", _base_dir.c_str(), exchg, code);
	boost::filesystem::create_directories(path);
	if (!dbPtr->open(path.c_str(), _tick_mapsize))
	{
		if (_sink) pipe_writer_log(_sink, LL_ERROR, "Opening tick db at {} failed: {}", path, dbPtr->errmsg());
		return WtLMDBPtr();
	}

	_tick_dbs[key] = dbPtr;
#ifdef WT_DATAWRITERAD_TEST_SEAMS
	observeWriterDbCacheForTest("tick", key.c_str(), dbPtr.get(), false);
#endif
	return dbPtr;
}
