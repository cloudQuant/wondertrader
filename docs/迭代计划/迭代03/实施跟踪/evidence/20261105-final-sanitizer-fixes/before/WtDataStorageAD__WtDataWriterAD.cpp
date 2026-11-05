#include "WtDataWriterAD.h"
#include "LMDBKeys.h"
#include "DayBarContractAD.h"
#include "../WtDataStorage/RTBlockPublish.hpp"

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
#include <cstddef>
#include <cstring>
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

	// I03-NEW-038: messages carry configured paths and caller-supplied codes; bound the
	// formatter to the per-thread buffer (same scheme as the AD readers) instead of
	// trusting their size.
	static thread_local char buffer[512] = { 0 };
	try
	{
		const std::size_t max_output_size = sizeof(buffer) - 1;
		const auto formatted = fmt::format_to_n(buffer, max_output_size, format, args...);
		if (formatted.size > max_output_size)
		{
			constexpr char truncation_marker[] = "...[truncated]";
			constexpr std::size_t marker_size = sizeof(truncation_marker) - 1;
			std::memcpy(buffer + max_output_size - marker_size, truncation_marker, marker_size);
			buffer[max_output_size] = '\0';
		}
		else
		{
			buffer[formatted.size] = '\0';
		}
	}
	catch (...)
	{
		constexpr char fallback[] = "WtDataWriterAD log formatting failed";
		std::memcpy(buffer, fallback, sizeof(fallback));
	}

	sink->outputLog(ll, buffer);
}

namespace
{
	// Display at most `capacity` bytes of an identity field; fixed-size tick fields may be
	// unterminated, and contract strings may exceed the LMDB key capacity.
	std::string identityFieldForLog(const char* field, std::size_t capacity)
	{
		if (field == NULL)
			return "(null)";
		std::size_t length = 0;
		while (length < capacity && field[length] != '\0')
			++length;
		std::string text(field, length);
		if (length == capacity && field[length - 1] != '\0')
			text += "...";
		return text;
	}
}

namespace
{
	bool checkedTickSessionDate(IBaseDataMgr* manager, const WTSTickData* tick, uint32_t& date)
	{
		// A zero action date in a tick is not a request to substitute wall time.
		if (manager == NULL || tick == NULL || tick->actiondate() == 0)
			return false;
		WTSSessionInfo* session = manager->getSessionByCode(tick->code(), tick->exchg());
		return session != NULL && session->tryGetOffsetDate(tick->actiondate(), tick->actiontime() / 100000, date);
	}
}

// Writer lifecycle V1 (card 9): admission accounting for this writer's public calls.
namespace
{
	thread_local std::vector<const WtDataWriterAD*> t_ad_operations;

	bool inAdOperation(const WtDataWriterAD* writer)
	{
		for (const WtDataWriterAD* active : t_ad_operations)
		{
			if (active == writer)
				return true;
		}
		return false;
	}

	void popAdOperation(const WtDataWriterAD* writer) noexcept
	{
		for (auto it = t_ad_operations.rbegin(); it != t_ad_operations.rend(); ++it)
		{
			if (*it == writer)
			{
				t_ad_operations.erase(std::next(it).base());
				return;
			}
		}
	}
}

class AdOperationGuard
{
public:
	AdOperationGuard(WtDataWriterAD& writer, bool admitAfterStop) : _writer(writer)
	{
		t_ad_operations.push_back(&writer);
		StdUniqueLock lock(writer._ops_mtx);
		if (writer._stop_requested && !admitAfterStop)
		{
			lock.unlock();
			popAdOperation(&writer);
			return;
		}
		++writer._active_ops;
		_admitted = true;
	}
	~AdOperationGuard()
	{
		if (!_admitted)
			return;
		popAdOperation(&_writer);
		{
			StdUniqueLock lock(_writer._ops_mtx);
			--_writer._active_ops;
		}
		_writer._ops_cond.notify_all();
	}
	AdOperationGuard(const AdOperationGuard&) = delete;
	AdOperationGuard& operator=(const AdOperationGuard&) = delete;
	explicit operator bool() const { return _admitted; }
private:
	WtDataWriterAD&	_writer;
	bool			_admitted = false;
};

namespace
{
	StdUniqueMutex& adWriterRegistryMutex()
	{
		static StdUniqueMutex mtx;
		return mtx;
	}

	std::set<IDataWriter*>& adWriterRegistry()
	{
		static std::set<IDataWriter*> writers;
		return writers;
	}

	WtDataWriterAD* registeredAdWriter(void* handle)
	{
		if (handle == NULL)
			return NULL;
		IDataWriter* writer = static_cast<IDataWriter*>(handle);
		StdUniqueLock lock(adWriterRegistryMutex());
		if (adWriterRegistry().count(writer) == 0)
			return NULL;
		return static_cast<WtDataWriterAD*>(writer);
	}

	WtDataWriterLifecycleV1Status adRequestStop(void* handle) noexcept
	{
		try
		{
			WtDataWriterAD* writer = registeredAdWriter(handle);
			if (writer == NULL)
				return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
			writer->requestStop();
			return WT_DATA_WRITER_LIFECYCLE_V1_OK;
		}
		catch (...)
		{
			return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
		}
	}

	WtDataWriterLifecycleV1Status adCanWait(void* handle) noexcept
	{
		try
		{
			WtDataWriterAD* writer = registeredAdWriter(handle);
			if (writer == NULL)
				return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
			return writer->canWaitDrain() ? WT_DATA_WRITER_LIFECYCLE_V1_OK : WT_DATA_WRITER_LIFECYCLE_V1_WOULD_DEADLOCK;
		}
		catch (...)
		{
			return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
		}
	}

	WtDataWriterLifecycleV1Status adWaitDrained(void* handle, uint32_t timeoutMs) noexcept
	{
		try
		{
			WtDataWriterAD* writer = registeredAdWriter(handle);
			if (writer == NULL)
				return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
			return writer->waitDrained(timeoutMs);
		}
		catch (...)
		{
			return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
		}
	}

	WtDataWriterLifecycleV1Status adAddDumper(void* handle, const char* id, void* dumper) noexcept
	{
		try
		{
			WtDataWriterAD* writer = registeredAdWriter(handle);
			if (writer == nullptr)
				return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
			return writer->addDumperV1(id, static_cast<IHisDataDumper*>(dumper));
		}
		catch (...)
		{
			return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
		}
	}

	WtDataWriterLifecycleV1Status adRemoveDumper(void* handle, const char* id) noexcept
	{
		try
		{
			WtDataWriterAD* writer = registeredAdWriter(handle);
			if (writer == nullptr)
				return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
			return writer->removeDumperV1(id);
		}
		catch (...)
		{
			return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
		}
	}

	const WtDataWriterLifecycleApiV1 g_ad_writer_lifecycle_v1 = {
		(uint32_t)sizeof(WtDataWriterLifecycleApiV1),
		WT_DATA_WRITER_LIFECYCLE_V1_ABI_VERSION,
		WT_DATA_WRITER_LIFECYCLE_V1_CAP_DRAIN | WT_DATA_WRITER_LIFECYCLE_V1_CAP_DUMPERS,
		adRequestStop,
		adCanWait,
		adWaitDrained,
		adAddDumper,
		adRemoveDumper
	};
}

extern "C"
{
	EXPORT_FLAG IDataWriter* createWriter()
	{
		WtDataWriterAD* ret = new WtDataWriterAD();
		try
		{
			StdUniqueLock lock(adWriterRegistryMutex());
			adWriterRegistry().insert(ret);
		}
		catch (...)
		{
			delete ret;
			return NULL;
		}
		return ret;
	}

	EXPORT_FLAG void deleteWriter(IDataWriter* &writer)
	{
		if (writer != NULL)
		{
			{
				StdUniqueLock lock(adWriterRegistryMutex());
				adWriterRegistry().erase(writer);
			}
			delete writer;
			writer = NULL;
		}
	}

	EXPORT_FLAG const WtDataWriterLifecycleApiV1* getDataWriterLifecycleApiV1(uint32_t requestedMajor)
	{
		return requestedMajor == WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR ? &g_ad_writer_lifecycle_v1 : NULL;
	}
};

static const uint32_t CACHE_SIZE_STEP_AD = 400;

namespace
{
	// TickCacheItem is a packed disk record whose WTSTickStruct member is not
	// necessarily aligned. Never construct or bind that packed member; move its
	// bytes to naturally aligned storage while preserving the on-disk layout.
	struct AlignedTickCacheItem
	{
		uint32_t _date = 0;
		WTSTickStruct _tick;
	};
	static_assert(sizeof(RTTickCache) == sizeof(RTBlockHeader), "unexpected tick cache header layout");
	static_assert(offsetof(TickCacheItem, _date) == 0 && offsetof(TickCacheItem, _tick) == sizeof(uint32_t),
		"unexpected tick cache record layout");
	static_assert(sizeof(TickCacheItem) == sizeof(uint32_t) + sizeof(WTSTickStruct),
		"unexpected tick cache record size");

	const unsigned char* tickCacheBytes(const RTTickCache* cache, uint32_t index)
	{
		return reinterpret_cast<const unsigned char*>(cache) + sizeof(RTTickCache)
			+ static_cast<std::size_t>(index) * sizeof(TickCacheItem);
	}

	void loadTickCacheItem(const RTTickCache* cache, uint32_t index, AlignedTickCacheItem& item)
	{
		const unsigned char* bytes = tickCacheBytes(cache, index);
		std::memcpy(&item._date, bytes, sizeof(item._date));
		std::memcpy(&item._tick, bytes + offsetof(TickCacheItem, _tick), sizeof(item._tick));
	}

	void storeTickCacheItem(RTTickCache* cache, uint32_t index, const AlignedTickCacheItem& item)
	{
		unsigned char* bytes = const_cast<unsigned char*>(tickCacheBytes(cache, index));
		std::memcpy(bytes, &item._date, sizeof(item._date));
		std::memcpy(bytes + offsetof(TickCacheItem, _tick), &item._tick, sizeof(item._tick));
	}

	struct AlignedBarCacheItem
	{
		char _exchg[16] = {};
		char _code[32] = {};
		WTSBarStruct _bar;
	};
	static_assert(sizeof(RTBarCache) == sizeof(RTBlockHeader), "unexpected bar cache header layout");
	static_assert(offsetof(BarCacheItem, _exchg) == 0 && offsetof(BarCacheItem, _code) == 16 &&
		offsetof(BarCacheItem, _bar) == 48 && sizeof(BarCacheItem) == 48 + sizeof(WTSBarStruct),
		"unexpected bar cache record layout");

	const unsigned char* barCacheBytes(const RTBarCache* cache, uint32_t index)
	{
		return reinterpret_cast<const unsigned char*>(cache) + sizeof(RTBarCache)
			+ static_cast<std::size_t>(index) * sizeof(BarCacheItem);
	}

	void loadBarCacheItem(const RTBarCache* cache, uint32_t index, AlignedBarCacheItem& item)
	{
		const unsigned char* bytes = barCacheBytes(cache, index);
		std::memcpy(item._exchg, bytes + offsetof(BarCacheItem, _exchg), sizeof(item._exchg));
		std::memcpy(item._code, bytes + offsetof(BarCacheItem, _code), sizeof(item._code));
		std::memcpy(&item._bar, bytes + offsetof(BarCacheItem, _bar), sizeof(item._bar));
	}

	void storeBarCacheItem(RTBarCache* cache, uint32_t index, const AlignedBarCacheItem& item)
	{
		unsigned char* bytes = const_cast<unsigned char*>(barCacheBytes(cache, index));
		std::memcpy(bytes + offsetof(BarCacheItem, _exchg), item._exchg, sizeof(item._exchg));
		std::memcpy(bytes + offsetof(BarCacheItem, _code), item._code, sizeof(item._code));
		std::memcpy(bytes + offsetof(BarCacheItem, _bar), &item._bar, sizeof(item._bar));
	}

	// 卡13：持发布锁时创建 RT 缓存文件；其他 writer 已创建则改为打开已有文件（bNew=false）
	bool createRTCacheFileLocked(const std::string& path, std::size_t recordBytes, bool& bNew, std::string& err,
		uint16_t version = 1)
	{
		if (!bNew)
			return true;
		boost::system::error_code ec;
		if (boost::filesystem::exists(path, ec))
		{
			bNew = false;
			return true;
		}
		RTBlockHeader header;
		memset(&header, 0, sizeof(header));
		memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
		header._type = BT_RT_Cache;
		header._version = version;
		header._size = 0;
		header._capacity = CACHE_SIZE_STEP_AD;
		return wt_rtpub::publishNewBlockFile(path, &header, sizeof(header),
			sizeof(RTBlockHeader) + (uint64_t)recordBytes * CACHE_SIZE_STEP_AD, err);
	}

	bool validateBarCacheIdentities(const RTBarCache* cache, IDataWriterSink* sink, const char* period)
	{
		for (uint32_t i = 0; i < cache->_size; ++i)
		{
			AlignedBarCacheItem item;
			loadBarCacheItem(cache, i, item);
			if (std::memchr(item._exchg, '\0', sizeof(item._exchg)) == NULL ||
				std::memchr(item._code, '\0', sizeof(item._code)) == NULL)
			{
				pipe_writer_log(sink, LL_ERROR, "Invalid {} bar cache identity at index {}", period, i);
				return false;
			}
		}
		return true;
	}

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
		//卡3 3c=C：日线缓存接受旧版本 1 与 time=0 合同版本 2，其余缓存只接受版本 1
		if (!wt_ad_daybar::isAcceptedRTCacheVersion(header._version, std::strcmp(cacheName, "day") == 0))
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
	// Never destroy a joinable worker: drain from a safe context. A host that deletes the
	// writer from its own callback/worker violates the V1 contract (DRAINED first).
	if (canWaitDrain())
		release();
}

ExtDumpers WtDataWriterAD::snapshotDumpers() const
{
	std::lock_guard<std::mutex> lock(_dumper_mtx);
	return _dumpers;
}

WtDataWriterLifecycleV1Status WtDataWriterAD::addDumperV1(const char* id, IHisDataDumper* dumper)
{
	if (id == NULL || id[0] == '\0' || dumper == NULL)
		return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
	{
		StdUniqueLock opsLock(_ops_mtx);
		if (_stop_requested)
			return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;
	}
	std::lock_guard<std::mutex> lock(_dumper_mtx);
	if (_dumpers.find(id) != _dumpers.end())
		return WT_DATA_WRITER_LIFECYCLE_V1_DUPLICATE;	// the original registration is kept
	_dumpers[id] = dumper;
	return WT_DATA_WRITER_LIFECYCLE_V1_OK;
}

WtDataWriterLifecycleV1Status WtDataWriterAD::removeDumperV1(const char* id)
{
	if (id == NULL || id[0] == '\0')
		return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
	std::lock_guard<std::mutex> lock(_dumper_mtx);
	auto it = _dumpers.find(id);
	if (it == _dumpers.end())
		return WT_DATA_WRITER_LIFECYCLE_V1_INVALID_ARGUMENT;
	_dumpers.erase(it);
	return WT_DATA_WRITER_LIFECYCLE_V1_OK;
}

void WtDataWriterAD::requestStop()
{
	{
		StdUniqueLock opsLock(_ops_mtx);
		_stop_requested = true;
	}
	{
		StdUniqueLock taskLock(_task_mtx);
		_terminated = true;	// the worker finishes its accepted queue, then exits
	}
	_task_cond.notify_all();
	_ops_cond.notify_all();
}

bool WtDataWriterAD::canWaitDrain()
{
	if (inAdOperation(this))
		return false;
	StdUniqueLock taskLock(_task_mtx);
	return _task_worker_id != std::this_thread::get_id();
}

WtDataWriterLifecycleV1Status WtDataWriterAD::waitDrained(uint32_t timeout_ms)
{
	if (!canWaitDrain())
		return WT_DATA_WRITER_LIFECYCLE_V1_WOULD_DEADLOCK;
	const bool forever = (timeout_ms == WT_DATA_WRITER_LIFECYCLE_V1_WAIT_FOREVER);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(forever ? 0 : timeout_ms);

	StdThreadPtr worker;
	{
		StdUniqueLock taskLock(_task_mtx);
		worker = _task_thrd;
	}

	StdUniqueLock opsLock(_ops_mtx);
	if (!_stop_requested)
		return WT_DATA_WRITER_LIFECYCLE_V1_ERROR;	// request_stop must come first
	auto drained = [this, &worker]() {
		return _active_ops == 0 && (worker == NULL || _task_exited || _task_joined);
	};
	if (forever)
		_ops_cond.wait(opsLock, drained);
	else if (!_ops_cond.wait_until(opsLock, deadline, drained))
		return WT_DATA_WRITER_LIFECYCLE_V1_PENDING;
	if (worker != NULL && !_task_joined)
	{
		_task_joined = true;
		opsLock.unlock();
		if (worker->joinable())
			worker->join();	// the loop has returned; this only reaps the thread
	}
	return WT_DATA_WRITER_LIFECYCLE_V1_DRAINED;
}

bool WtDataWriterAD::init(WTSVariant* params, IDataWriterSink* sink)
{
	{
		// Lifecycle V1 / card 9c: a stopped writer is never re-initialized.
		StdUniqueLock opsLock(_ops_mtx);
		if (_stop_requested)
			return false;
	}
	// A fresh instance may be retried after a failed initialization. Until the
	// first init succeeds, no partially loaded mapping may survive and be reused
	// for a later init with a different base directory. Existing initialized
	// instances keep their previous behavior; concurrent init is not supported.
	const bool fresh = !_tick_cache_file && !_m1_cache._file_ptr && !_m5_cache._file_ptr &&
		!_d1_cache._file_ptr && !_task_thrd;
	struct FreshInitRollback
	{
		std::function<void()> cleanup;
		bool committed = false;
		~FreshInitRollback() { if (!committed) cleanup(); }
	} rollback{[this, fresh]() {
		if (!fresh)
			return;
		_tick_cache_idx.clear();
		_tick_cache_scanned = 0;
		_tick_cache_block = NULL;
		_tick_cache_file.reset();
		for (RTBarCacheWrapper* cache : {&_m1_cache, &_m5_cache, &_d1_cache})
		{
			cache->_idx.clear();
			cache->_cache_block = NULL;
			cache->_file_ptr.reset();
		}
		_bd_mgr = NULL;
		_sink = NULL;
	}};

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
				{
					StdUniqueLock taskLock(_task_mtx);
					_task_worker_id = std::this_thread::get_id();
				}
				struct ExitMark
				{
					WtDataWriterAD* writer;
					~ExitMark()
					{
						{
							StdUniqueLock opsLock(writer->_ops_mtx);
							writer->_task_exited = true;
						}
						writer->_ops_cond.notify_all();
					}
				} exitMark{ this };
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

	rollback.committed = true;
	return true;
}

void WtDataWriterAD::release()
{
	requestStop();
	// From a sink/dumper callback or the worker itself only request the stop; the owner
	// drains later from a safe context (no self-join).
	if (!canWaitDrain())
		return;
	waitDrained(WT_DATA_WRITER_LIFECYCLE_V1_WAIT_FOREVER);
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
		// 卡13：tick 缓存的创建、映射与首次扫描在跨进程发布锁与结构性日志内完成
		wt_rtpub::PublishLock publishLock;
		if (!publishLock.acquire(tickFilename))
		{
			pipe_writer_log(_sink, LL_ERROR, "{}", publishLock.error());
			return false;
		}
		wt_rtpub::StructuralGuard structural(publishLock, CACHE_SIZE_STEP_AD);
		if (!structural.begun())
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot journal structural change of {}", tickFilename);
			return false;
		}
		if (bNewTick)
		{
			boost::system::error_code existsEc;
			if (boost::filesystem::exists(tickFilename, existsEc))
				bNewTick = false;	// another writer created it while this one validated
		}
		if (bNewTick)
		{
			uint64_t uSize = sizeof(RTTickCache) + sizeof(TickCacheItem) * CACHE_SIZE_STEP_AD;
			RTBlockHeader header;
			memset(&header, 0, sizeof(header));
			memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
			header._type = BT_RT_Cache;
			header._version = 1;
			header._size = 0;
			header._capacity = CACHE_SIZE_STEP_AD;
			std::string publishErr;
			if (!wt_rtpub::publishNewBlockFile(tickFilename, &header, sizeof(header), uSize, publishErr))
			{
				pipe_writer_log(_sink, LL_ERROR, "Cannot create tick cache file {}: {}", tickFilename, publishErr);
				return false;
			}
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
			// Reject malformed identities before publishing any rebuilt index.
			for (uint32_t i = 0; i < _tick_cache_block->_size; i++)
			{
				AlignedTickCacheItem item;
				loadTickCacheItem(_tick_cache_block, i, item);
				if (std::memchr(item._tick.exchg, '\0', sizeof(item._tick.exchg)) == NULL ||
					std::memchr(item._tick.code, '\0', sizeof(item._tick.code)) == NULL)
				{
					pipe_writer_log(_sink, LL_ERROR, "Invalid tick cache identity at index {}", i);
					_tick_cache_block = NULL;
					_tick_cache_file.reset();
					return false;
				}
			}
			for (uint32_t i = 0; i < _tick_cache_block->_size; i++)
			{
				AlignedTickCacheItem item;
				loadTickCacheItem(_tick_cache_block, i, item);
				std::string key = fmtutil::format<64>("{}.{}", item._tick.exchg, item._tick.code);
				_tick_cache_idx[key] = i;
			}
		}
		_tick_cache_scanned = wt_rtpub::publishedSize(_tick_cache_block);
	}

	if (_m1_cache.empty())
	{
		// 卡13：创建/映射/首次扫描在跨进程发布锁与结构性日志内完成，不截断其他 writer 已创建的文件
		wt_rtpub::PublishLock m1Lock;
		if (!m1Lock.acquire(m1Filename))
		{
			pipe_writer_log(_sink, LL_ERROR, "{}", m1Lock.error());
			return false;
		}
		wt_rtpub::StructuralGuard m1Structural(m1Lock, CACHE_SIZE_STEP_AD);
		std::string m1Err;
		if (!m1Structural.begun() || !createRTCacheFileLocked(m1Filename, sizeof(BarCacheItem), bNewM1, m1Err))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot create m1 cache file {} {}", m1Filename, m1Err);
			return false;
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
			if (!validateBarCacheIdentities(_m1_cache._cache_block, _sink, "m1"))
			{
				_m1_cache._cache_block = NULL;
				_m1_cache._file_ptr.reset();
				return false;
			}
			for (uint32_t i = 0; i < _m1_cache._cache_block->_size; i++)
			{
				AlignedBarCacheItem item;
				loadBarCacheItem(_m1_cache._cache_block, i, item);
				std::string key = fmtutil::format<64>("{}.{}", item._exchg, item._code);
				_m1_cache._idx[key] = i;
			}
		}
		_m1_cache._scanned = wt_rtpub::publishedSize(_m1_cache._cache_block);
	}

	if (_m5_cache.empty())
	{
		// 卡13：创建/映射/首次扫描在跨进程发布锁与结构性日志内完成，不截断其他 writer 已创建的文件
		wt_rtpub::PublishLock m5Lock;
		if (!m5Lock.acquire(m5Filename))
		{
			pipe_writer_log(_sink, LL_ERROR, "{}", m5Lock.error());
			return false;
		}
		wt_rtpub::StructuralGuard m5Structural(m5Lock, CACHE_SIZE_STEP_AD);
		std::string m5Err;
		if (!m5Structural.begun() || !createRTCacheFileLocked(m5Filename, sizeof(BarCacheItem), bNewM5, m5Err))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot create m5 cache file {} {}", m5Filename, m5Err);
			return false;
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
			if (!validateBarCacheIdentities(_m5_cache._cache_block, _sink, "m5"))
			{
				_m5_cache._cache_block = NULL;
				_m5_cache._file_ptr.reset();
				return false;
			}
			for (uint32_t i = 0; i < _m5_cache._cache_block->_size; i++)
			{
				AlignedBarCacheItem item;
				loadBarCacheItem(_m5_cache._cache_block, i, item);
				std::string key = fmtutil::format<64>("{}.{}", item._exchg, item._code);
				_m5_cache._idx[key] = i;
			}
		}
		_m5_cache._scanned = wt_rtpub::publishedSize(_m5_cache._cache_block);
	}

	if (_d1_cache.empty())
	{
		// 卡13：创建/映射/首次扫描在跨进程发布锁与结构性日志内完成，不截断其他 writer 已创建的文件
		wt_rtpub::PublishLock d1Lock;
		if (!d1Lock.acquire(d1Filename))
		{
			pipe_writer_log(_sink, LL_ERROR, "{}", d1Lock.error());
			return false;
		}
		wt_rtpub::StructuralGuard d1Structural(d1Lock, CACHE_SIZE_STEP_AD);
		std::string d1Err;
		if (!d1Structural.begun() || !createRTCacheFileLocked(d1Filename, sizeof(BarCacheItem), bNewD1, d1Err, BLOCK_VERSION_DAY_T0))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot create day cache file {} {}", d1Filename, d1Err);
			return false;
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
			_d1_cache._cache_block->_version = BLOCK_VERSION_DAY_T0;
			strcpy(_d1_cache._cache_block->_blk_flag, BLK_FLAG);
		}
		else
		{
			if (!validateBarCacheIdentities(_d1_cache._cache_block, _sink, "day"))
			{
				_d1_cache._cache_block = NULL;
				_d1_cache._file_ptr.reset();
				return false;
			}
			//卡3 3c=C：版本 1 的旧日线缓存在发布锁与结构性日志内原地迁移为 time=0，最后提升版本标记
			const bool migrateDay = _d1_cache._cache_block->_version != BLOCK_VERSION_DAY_T0;
			for (uint32_t i = 0; i < _d1_cache._cache_block->_size; i++)
			{
				AlignedBarCacheItem item;
				loadBarCacheItem(_d1_cache._cache_block, i, item);
				if (migrateDay && item._bar.time != 0)
				{
					wt_ad_daybar::normalize(item._bar);
					storeBarCacheItem(_d1_cache._cache_block, i, item);
				}
				std::string key = fmtutil::format<64>("{}.{}", item._exchg, item._code);
				_d1_cache._idx[key] = i;
			}
			if (migrateDay)
			{
				_d1_cache._cache_block->_version = BLOCK_VERSION_DAY_T0;
				pipe_writer_log(_sink, LL_INFO, "Day cache {} migrated from version 1 to {} (day.time=0), {} items",
					d1Filename, BLOCK_VERSION_DAY_T0, _d1_cache._cache_block->_size);
			}
		}
		_d1_cache._scanned = wt_rtpub::publishedSize(_d1_cache._cache_block);
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
		// 卡13：以 release 存储发布，供其他进程按协议读取
		wt_rtpub::publishCapacity(newBlock, nCount);
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
	AdOperationGuard operation(*this, false);
	if (!operation)
		return false;	// stopped: admission closed (lifecycle V1)

	if (_log_group_size == 0)
		return false;

	if (curTick == NULL)
		return false;

	// Reject an unverifiable action date before retaining the tick or accepting
	// an async task. Preserve the existing no-contract no-op behavior.
	if (curTick->getContractInfo() != NULL)
	{
		WTSContractInfo* ct = curTick->getContractInfo();
		if (!lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()) ||
			!lmdbKeyIdentityValid(curTick->exchg(), curTick->code()))
		{
			// L04-004: admission rejection is observable; nothing was cached, stored or queued.
			pipe_writer_log(_sink, LL_ERROR, "Rejected tick before any side effect: storage identity {}.{} (tick {}.{}) "
				"does not fit the LMDB key (exchange < {} bytes, code < {} bytes, both non-empty)",
				identityFieldForLog(ct->getExchg(), 64), identityFieldForLog(ct->getCode(), 64),
				identityFieldForLog(curTick->exchg(), MAX_EXCHANGE_LENGTH), identityFieldForLog(curTick->code(), MAX_INSTRUMENT_LENGTH),
				MAX_EXCHANGE_LENGTH, MAX_INSTRUMENT_LENGTH);
			return false;
		}
		uint32_t checkedDate = 0;
		if (_sink == NULL || !checkedTickSessionDate(_bd_mgr, curTick, checkedDate))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot verify session date for tick {}.{}", curTick->exchg(), curTick->code());
			return false;
		}
	}

	curTick->retain();
	std::shared_ptr<WTSTickData> tick_ref(curTick, [](WTSTickData* tick) { tick->release(); });
	// 卡13：同步模式把持久化结果回传给调用方；异步模式只表示已接收，失败经计数与日志观察
	auto persisted = std::make_shared<std::atomic<bool>>(true);
	const bool accepted = pushTask([this, tick_ref, procFlag, persisted](){
		WTSTickData* curTick = tick_ref.get();

		do
		{
			WTSContractInfo* ct = curTick->getContractInfo();
			if(ct == NULL)
				break;
			if (!lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()) ||
				!lmdbKeyIdentityValid(curTick->exchg(), curTick->code()))
			{
				// L04-004: accepted (async true) but not processed -> counted and logged, never silent.
				_tick_rejected_after_accept.fetch_add(1, std::memory_order_acq_rel);
				persisted->store(false, std::memory_order_release);
				pipe_writer_log(_sink, LL_ERROR, "Dropped accepted tick: storage identity {}.{} (tick {}.{}) no longer fits the LMDB key",
					identityFieldForLog(ct->getExchg(), 64), identityFieldForLog(ct->getCode(), 64),
					identityFieldForLog(curTick->exchg(), MAX_EXCHANGE_LENGTH), identityFieldForLog(curTick->code(), MAX_INSTRUMENT_LENGTH));
				break;
			}

			// A caller may have retained and changed the queued tick. Recheck
			// immediately before consulting the sink or touching writer state.
			uint32_t checkedDate = 0;
			if (!checkedTickSessionDate(_bd_mgr, curTick, checkedDate))
			{
				_tick_rejected_after_accept.fetch_add(1, std::memory_order_acq_rel);
				persisted->store(false, std::memory_order_release);
				pipe_writer_log(_sink, LL_ERROR, "Cannot verify session date for queued tick {}.{}",
					identityFieldForLog(curTick->exchg(), MAX_EXCHANGE_LENGTH), identityFieldForLog(curTick->code(), MAX_INSTRUMENT_LENGTH));
				break;
			}

			WTSCommodityInfo* commInfo = ct->getCommInfo();

			//再根据状态过滤
			if (!_sink->canSessionReceive(commInfo->getSession()))
				break;

			// 卡13（13a=A）：先在缓存锁内准备缓存项，LMDB 提交成功后才发布缓存；
			// 提交失败时缓存、K线、广播与外部 dumper 均不推进，同一 tick 可原样重试
			bool persistFailed = false;
			auto commit = [this, ct, curTick, &persistFailed]() -> bool {
				if (_disable_tick)
					return true;
				if (commitTickToDb(ct, curTick))
					return true;
				persistFailed = true;
				return false;
			};
			if (!updateTickCache(ct, curTick, procFlag, commit))
			{
				if (persistFailed)
				{
					_tick_persist_failures.fetch_add(1, std::memory_order_acq_rel);
					persisted->store(false, std::memory_order_release);
				}
				break;
			}

			//LMDB 已提交，再写外部 dumper
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
	if (!accepted)
		return false;
	return _async_task ? true : persisted->load(std::memory_order_acquire);
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

bool WtDataWriterAD::commitTickToDb(WTSContractInfo* ct, WTSTickData* curTick)
{
	if (ct == NULL || curTick == NULL || !lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()))
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot persist tick with invalid LMDB identity");
		return false;
	}
	//直接落地
	WtLMDBPtr db = get_t_db(ct->getExchg(), ct->getCode());
	if (!db)
	{
		pipe_writer_log(_sink, LL_ERROR, "Tick db of {} unavailable; tick is not published", ct->getFullCode());
		return false;
	}

	//这里要把时间转成便宜时间，并用交易日作为date
	//这样可以根据交易日筛选历史tick数据
	uint32_t actTime = curTick->actiontime();
	uint32_t offTime = ct->getCommInfo()->getSessionInfo()->offsetTime(actTime / 100000, true) * 100000 + actTime % 100000;

	LMDBHftKey key(ct->getExchg(), ct->getCode(), curTick->tradingdate(), offTime);
	WtLMDBQuery query(*db);
	if (!query.put_and_commit((void*)&key, sizeof(key), &curTick->getTickStruct(), sizeof(WTSTickStruct)))
	{
		pipe_writer_log(_sink, LL_ERROR, "pipe tick of {} to db failed: {}", ct->getFullCode(), db->errmsg());
		return false;
	}
	return true;
}

void WtDataWriterAD::pipeToTicks(WTSContractInfo* ct, WTSTickData* curTick)
{
	if (ct == NULL || curTick == NULL || !lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()))
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot persist tick with invalid LMDB identity");
		return;
	}

	const ExtDumpers dumperSnapshot = snapshotDumpers();	// K20: call dumpers outside the registry lock
	for (auto& item : dumperSnapshot)
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
	if (ct == NULL || !lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()))
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot persist day bar with invalid LMDB identity");
		return;
	}
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

	const ExtDumpers dumperSnapshot = snapshotDumpers();	// K20: call dumpers outside the registry lock
	for (auto& item : dumperSnapshot)
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
	if (ct == NULL || !lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()))
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot persist m1 bar with invalid LMDB identity");
		return;
	}
	//直接落地
	// 卡13/R32-003/R100-003：v1 LMDB 分钟键为 uint32，迁移合同落地前拒绝不可表示的时间，不得静默回绕
	WtLMDBPtr db;
	if (bar.time > (uint64_t)UINT32_MAX)
		pipe_writer_log(_sink, LL_ERROR, "m1 bar time {} of {} is not representable in the v1 LMDB key; db write refused", bar.time, ct->getFullCode());
	else
		db = get_k_db(ct->getExchg(), KP_Minute1);
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

	const ExtDumpers dumperSnapshot = snapshotDumpers();	// K20: call dumpers outside the registry lock
	for (auto& item : dumperSnapshot)
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
	if (ct == NULL || !lmdbKeyIdentityValid(ct->getExchg(), ct->getCode()))
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot persist m5 bar with invalid LMDB identity");
		return;
	}
	//直接落地
	// 卡13/R32-003/R100-003：v1 LMDB 分钟键为 uint32，迁移合同落地前拒绝不可表示的时间，不得静默回绕
	WtLMDBPtr db;
	if (bar.time > (uint64_t)UINT32_MAX)
		pipe_writer_log(_sink, LL_ERROR, "m5 bar time {} of {} is not representable in the v1 LMDB key; db write refused", bar.time, ct->getFullCode());
	else
		db = get_k_db(ct->getExchg(), KP_Minute5);
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

	const ExtDumpers dumperSnapshot = snapshotDumpers();	// K20: call dumpers outside the registry lock
	for (auto& item : dumperSnapshot)
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
		uint32_t& idx, bool& bNewCode, wt_rtpub::PublishLock& publishLock) -> bool {
		bNewCode = false;
		// 卡13：持跨进程发布锁；同步其他 writer 的扩容映射并补扫其新增缓存项
		if (cache._file_ptr != NULL)
		{
			if (!publishLock.acquire(cache._file_ptr->filename()))
			{
				pipe_writer_log(_sink, LL_ERROR, "{} bar cache publish lock unavailable: {}", period, publishLock.error());
				return false;
			}
			if (publishLock.recovered())
			{
				boost::system::error_code sizeEc;
				const uintmax_t physical = boost::filesystem::file_size(cache._file_ptr->filename(), sizeEc);
				if (sizeEc)
					return false;
				const uint32_t physRecords = wt_rtpub::mappedRecords((uint64_t)physical, sizeof(RTBarCache), sizeof(BarCacheItem));
				if (wt_rtpub::publishedCapacity(cache._cache_block) > physRecords)
					wt_rtpub::publishCapacity(cache._cache_block, physRecords);
				if (wt_rtpub::publishedSize(cache._cache_block) > wt_rtpub::publishedCapacity(cache._cache_block))
					wt_rtpub::publishSize(cache._cache_block, wt_rtpub::publishedCapacity(cache._cache_block));
				pipe_writer_log(_sink, LL_WARN, "{} bar cache recovered after an interrupted structural change by pid {}", period, publishLock.recoveredPid());
				if (!publishLock.endStructural())
					return false;
			}
			if (wt_rtpub::publishedCapacity(cache._cache_block) >
				wt_rtpub::mappedRecords(cache._file_ptr->size(), sizeof(RTBarCache), sizeof(BarCacheItem)))
			{
				BoostMFPtr fresh(new BoostMappingFile());
				if (!fresh->map(cache._file_ptr->filename()) || fresh->addr() == NULL ||
					wt_rtpub::mappedRecords(fresh->size(), sizeof(RTBarCache), sizeof(BarCacheItem)) < wt_rtpub::publishedCapacity(fresh->addr()))
				{
					pipe_writer_log(_sink, LL_ERROR, "Cannot remap {} bar cache grown by another writer", period);
					return false;
				}
				cache._file_ptr.swap(fresh);
				cache._cache_block = (RTBarCache*)cache._file_ptr->addr();
			}
			const uint32_t published = std::min(wt_rtpub::publishedSize(cache._cache_block), wt_rtpub::publishedCapacity(cache._cache_block));
			for (uint32_t i = cache._scanned; i < published; i++)
			{
				AlignedBarCacheItem foreign;
				loadBarCacheItem(cache._cache_block, i, foreign);
				if (std::memchr(foreign._exchg, '\0', sizeof(foreign._exchg)) == NULL ||
					std::memchr(foreign._code, '\0', sizeof(foreign._code)) == NULL)
				{
					pipe_writer_log(_sink, LL_ERROR, "Invalid {} bar cache identity at index {} published by another writer", period, i);
					return false;
				}
				std::string foreignKey = fmtutil::format<64>("{}.{}", foreign._exchg, foreign._code);
				if (cache._idx.find(foreignKey) == cache._idx.end())
					cache._idx[foreignKey] = i;
			}
			if (published > cache._scanned)
				cache._scanned = published;
		}
		wt_hashmap<std::string, uint32_t>::iterator found = cache._idx.find(key);
		if (found != cache._idx.end())
		{
			idx = found->second;
			if (idx >= cache._cache_block->_size || idx >= cache._cache_block->_capacity)
			{
				pipe_writer_log(_sink, LL_ERROR, "Invalid {} bar cache index {} for {}", period, idx, key);
				return false;
			}
			return true;
		}

		idx = wt_rtpub::publishedSize(cache._cache_block);
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
			wt_rtpub::StructuralGuard structural(publishLock, newCapacity);
			if (!structural.begun() || !resizeRTBlock<RTBarCache, BarCacheItem>(cache._file_ptr, newCapacity))
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
		wt_rtpub::PublishLock barPublishLock;
		uint32_t idx = 0;
		bool bNewCode = false;
		if (!prepareBarCacheItem(_d1_cache, "day", idx, bNewCode, barPublishLock))
		{
			// Keep this stream unchanged and continue updating the other bar caches.
		}
		else
		{
		AlignedBarCacheItem item;
		if (!bNewCode)
			loadBarCacheItem(_d1_cache._cache_block, idx, item);
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
			newBar->time = 0;	//卡3 3c=C：日线 time=0，date 为交易日
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

		storeBarCacheItem(_d1_cache._cache_block, idx, item);
		if (bNewCode)
		{
			wt_rtpub::publishSize(_d1_cache._cache_block, idx + 1);
			_d1_cache._scanned = idx + 1;
		}
		}
	}

	//更新1分钟线
	if (!_disable_min1 && _m1_cache._cache_block)
	{
		StdUniqueLock lock(_m1_cache._mtx);
		wt_rtpub::PublishLock barPublishLock;
		uint32_t idx = 0;
		bool bNewCode = false;
		if (!prepareBarCacheItem(_m1_cache, "m1", idx, bNewCode, barPublishLock))
		{
			// Keep this stream unchanged and continue updating the other bar caches.
		}
		else
		{
		AlignedBarCacheItem item;
		if (!bNewCode)
			loadBarCacheItem(_m1_cache._cache_block, idx, item);
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

		storeBarCacheItem(_m1_cache._cache_block, idx, item);
		if (bNewCode)
		{
			wt_rtpub::publishSize(_m1_cache._cache_block, idx + 1);
			_m1_cache._scanned = idx + 1;
		}
		}
	}

	//更新5分钟线
	if (!_disable_min5 && _m5_cache._cache_block)
	{
		StdUniqueLock lock(_m5_cache._mtx);
		wt_rtpub::PublishLock barPublishLock;
		uint32_t idx = 0;
		bool bNewCode = false;
		if (!prepareBarCacheItem(_m5_cache, "m5", idx, bNewCode, barPublishLock))
		{
			// Keep this stream unchanged and continue processing the current tick.
		}
		else
		{
		AlignedBarCacheItem item;
		if (!bNewCode)
			loadBarCacheItem(_m5_cache._cache_block, idx, item);
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

		storeBarCacheItem(_m5_cache._cache_block, idx, item);
		if (bNewCode)
		{
			wt_rtpub::publishSize(_m5_cache._cache_block, idx + 1);
			_m5_cache._scanned = idx + 1;
		}
		}
	}
}

WTSTickData* WtDataWriterAD::getCurTick(const char* code, const char* exchg/* = ""*/)
{
	// Reads stay available after stop (mappings live until destruction) but are counted.
	AdOperationGuard operation(*this, true);
	if (code == NULL || strlen(code) == 0)
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
	if (_tick_cache_block == NULL || idx >= _tick_cache_block->_size || idx >= _tick_cache_block->_capacity)
		return NULL;
	AlignedTickCacheItem item;
	loadTickCacheItem(_tick_cache_block, idx, item);
	return WTSTickData::create(item._tick);
}

bool WtDataWriterAD::checkLmdbKeyLayout(WtLMDB& db, const std::string& path, std::size_t keySize, bool isTick)
{
	MDB_txn* txn = NULL;
	int rc = mdb_txn_begin(db.env(), NULL, MDB_RDONLY, &txn);
	if (rc != MDB_SUCCESS)
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot probe LMDB key layout at {}: {}", path, mdb_strerror(rc));
		return false;
	}
	const MDB_dbi dbi = db.update_dbi(txn);
	if (db.has_error())
	{
		mdb_txn_abort(txn);
		pipe_writer_log(_sink, LL_ERROR, "Cannot open LMDB at {} for key layout probe: {}", path, db.errmsg());
		return false;
	}

	bool ok = true;
	std::string reason;
	MDB_cursor* cursor = NULL;
	rc = mdb_cursor_open(txn, dbi, &cursor);
	if (rc != MDB_SUCCESS)
	{
		ok = false;
		reason = mdb_strerror(rc);
	}
	else
	{
		const MDB_cursor_op ops[2] = { MDB_FIRST, MDB_LAST };
		for (const MDB_cursor_op op : ops)
		{
			MDB_val key, val;
			rc = mdb_cursor_get(cursor, &key, &val, op);
			if (rc == MDB_NOTFOUND)
				break;	// empty database: nothing to migrate
			if (rc != MDB_SUCCESS)
			{
				ok = false;
				reason = mdb_strerror(rc);
				break;
			}
			if (key.mv_size != keySize)
			{
				ok = false;
				reason = "key size " + std::to_string(key.mv_size) + " differs from current " + std::to_string(keySize);
				break;
			}
			if (isTick && op == MDB_FIRST)
			{
				// Keys of one contract sort by big-endian trading date; a zero date sorts first.
				uint32_t date = 0;
				memcpy(&date, static_cast<const char*>(key.mv_data) + MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH, sizeof(date));
				if (date == 0)
				{
					ok = false;
					reason = "legacy tick key with zero trading date (records may have overwritten each other)";
					break;
				}
			}
		}
		mdb_cursor_close(cursor);
	}
	// A read-only commit keeps the dbi handle opened above for later writes.
	mdb_txn_commit(txn);
	if (!ok)
		pipe_writer_log(_sink, LL_ERROR, "LMDB at {} quarantined, unknown key format (13c fail-closed): {}", path, reason);
	return ok;
}

bool WtDataWriterAD::updateTickCache(WTSContractInfo* /*ct*/, WTSTickData* curTick, uint32_t procFlag,
	const std::function<bool()>& commitBeforePublish)
{
	if (curTick == NULL || _tick_cache_block == NULL)
	{
		pipe_writer_log(_sink, LL_ERROR, "Tick cache data not initialized");
		return false;
	}
	uint32_t tdate = 0;
	if (!checkedTickSessionDate(_bd_mgr, curTick, tdate))
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot verify session date for tick {}.{}", curTick->exchg(), curTick->code());
		return false;
	}

	StdUniqueLock lock(_mtx_tick_cache);
	// 卡13：缓存读改写在跨进程发布锁内完成；先同步其他 writer 的扩容并补扫其新增的缓存项
	wt_rtpub::PublishLock publishLock;
	if (_tick_cache_file == NULL || !publishLock.acquire(_tick_cache_file->filename()))
	{
		pipe_writer_log(_sink, LL_ERROR, "Tick cache publish lock unavailable: {}", publishLock.error());
		return false;
	}
	if (publishLock.recovered())
	{
		boost::system::error_code sizeEc;
		const uintmax_t physical = boost::filesystem::file_size(_tick_cache_file->filename(), sizeEc);
		if (sizeEc)
			return false;
		const uint32_t physRecords = wt_rtpub::mappedRecords((uint64_t)physical, sizeof(RTTickCache), sizeof(TickCacheItem));
		if (wt_rtpub::publishedCapacity(_tick_cache_block) > physRecords)
			wt_rtpub::publishCapacity(_tick_cache_block, physRecords);
		if (wt_rtpub::publishedSize(_tick_cache_block) > wt_rtpub::publishedCapacity(_tick_cache_block))
			wt_rtpub::publishSize(_tick_cache_block, wt_rtpub::publishedCapacity(_tick_cache_block));
		pipe_writer_log(_sink, LL_WARN, "Tick cache recovered after an interrupted structural change by pid {}", publishLock.recoveredPid());
		if (!publishLock.endStructural())
			return false;
	}
	if (wt_rtpub::publishedCapacity(_tick_cache_block) >
		wt_rtpub::mappedRecords(_tick_cache_file->size(), sizeof(RTTickCache), sizeof(TickCacheItem)))
	{
		BoostMFPtr fresh(new BoostMappingFile());
		if (!fresh->map(_tick_cache_file->filename()) || fresh->addr() == NULL ||
			wt_rtpub::mappedRecords(fresh->size(), sizeof(RTTickCache), sizeof(TickCacheItem)) < wt_rtpub::publishedCapacity(fresh->addr()))
		{
			pipe_writer_log(_sink, LL_ERROR, "Cannot remap tick cache grown by another writer");
			return false;
		}
		_tick_cache_file.swap(fresh);
		_tick_cache_block = (RTTickCache*)_tick_cache_file->addr();
	}
	{
		const uint32_t published = std::min(wt_rtpub::publishedSize(_tick_cache_block), wt_rtpub::publishedCapacity(_tick_cache_block));
		for (uint32_t i = _tick_cache_scanned; i < published; i++)
		{
			AlignedTickCacheItem foreign;
			loadTickCacheItem(_tick_cache_block, i, foreign);
			if (std::memchr(foreign._tick.exchg, '\0', sizeof(foreign._tick.exchg)) == NULL ||
				std::memchr(foreign._tick.code, '\0', sizeof(foreign._tick.code)) == NULL)
			{
				pipe_writer_log(_sink, LL_ERROR, "Invalid tick cache identity at index {} published by another writer", i);
				return false;
			}
			std::string foreignKey = fmtutil::format<64>("{}.{}", foreign._tick.exchg, foreign._tick.code);
			if (_tick_cache_idx.find(foreignKey) == _tick_cache_idx.end())
				_tick_cache_idx[foreignKey] = i;
		}
		if (published > _tick_cache_scanned)
			_tick_cache_scanned = published;
	}
	// 保存预处理前的 tick；持久化失败时恢复，保证同一 tick 可原样重试
	const WTSTickStruct originalTick = curTick->getTickStruct();

	std::string key = fmtutil::format<64>("{}.{}", curTick->exchg(), curTick->code());
	uint32_t idx = 0;
	bool bNewCode = false;
	AlignedTickCacheItem item;
	wt_hashmap<std::string, uint32_t>::iterator found = _tick_cache_idx.find(key);
	if (found == _tick_cache_idx.end())
	{
		idx = wt_rtpub::publishedSize(_tick_cache_block);
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

			wt_rtpub::StructuralGuard structural(publishLock, (uint64_t)capacity + CACHE_SIZE_STEP_AD);
			if (!structural.begun() || !resizeRTBlock<RTTickCache, TickCacheItem>(_tick_cache_file, capacity + CACHE_SIZE_STEP_AD))
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

	if (!bNewCode)
	{
		if (idx >= _tick_cache_block->_size || idx >= _tick_cache_block->_capacity)
		{
			pipe_writer_log(_sink, LL_ERROR, "Invalid tick cache index {} for {}", idx, key);
			return false;
		}
		loadTickCacheItem(_tick_cache_block, idx, item);
	}
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

	// 卡13（13a=A）：缓存项已完整准备；先由调用方提交持久化，成功后才发布索引/内容/_size
	if (commitBeforePublish && !commitBeforePublish())
	{
		curTick->getTickStruct() = originalTick;
		return false;
	}
	if (bNewCode)
	{
		// Commit data, index, and size only after the cache entry has been fully prepared.
		_tick_cache_idx[key] = idx;
	}
	storeTickCacheItem(_tick_cache_block, idx, item);
	if (bNewCode)
	{
		wt_rtpub::publishSize(_tick_cache_block, idx + 1);
		_tick_cache_scanned = idx + 1;
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
	if (_quarantined_dbs.count(path) != 0)
		return WtLMDBPtr();
	boost::system::error_code ec;
	boost::filesystem::create_directories(path, ec);
	if (ec)
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot create {} db directory {}: {}", subdir, path, ec.message());
		return WtLMDBPtr();
	}
	if(!dbPtr->open(path.c_str(), _kline_mapsize))
	{
		if (_sink) pipe_writer_log(_sink, LL_ERROR, "Opening {} db at {} failed: {}", subdir, path, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	if (!checkLmdbKeyLayout(*dbPtr, path, sizeof(LMDBBarKey), false))
	{
		_quarantined_dbs.insert(path);
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
	if (_quarantined_dbs.count(path) != 0)
		return WtLMDBPtr();
	boost::system::error_code ec;
	boost::filesystem::create_directories(path, ec);
	if (ec)
	{
		pipe_writer_log(_sink, LL_ERROR, "Cannot create tick db directory {}: {}", path, ec.message());
		return WtLMDBPtr();
	}
	if (!dbPtr->open(path.c_str(), _tick_mapsize))
	{
		if (_sink) pipe_writer_log(_sink, LL_ERROR, "Opening tick db at {} failed: {}", path, dbPtr->errmsg());
		return WtLMDBPtr();
	}
	if (!checkLmdbKeyLayout(*dbPtr, path, sizeof(LMDBHftKey), true))
	{
		_quarantined_dbs.insert(path);
		return WtLMDBPtr();
	}

	_tick_dbs[key] = dbPtr;
#ifdef WT_DATAWRITERAD_TEST_SEAMS
	observeWriterDbCacheForTest("tick", key.c_str(), dbPtr.get(), false);
#endif
	return dbPtr;
}
