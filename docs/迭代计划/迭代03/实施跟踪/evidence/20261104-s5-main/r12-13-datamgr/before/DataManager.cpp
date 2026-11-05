/*!
 * \file DataManager.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器实现
 * \details 该文件实现了WonderTrader数据管理器DataManager类，负责行情数据的接收、处理、存储和广播
 * 实现了IDataWriterSink接口，作为数据写入器的回调接口，处理各类行情数据并进行相应操作
 *
 * Writer lifecycle (iteration 03, decision card 9: 9a=A+C, 9b=A, 9c=A, 9d=A; BUG-I02-NEW-038,
 * BUG-R18-002, BUG-R23-001): the writer, its remover/module and the sink handed to the
 * writer live in a heap WriterHost. Writer calls and writer callbacks pin the host and
 * are tracked per thread; release requested from inside such a frame is queued and
 * delivered at a safe point. Only a lifecycle-V1 DRAINED result authorizes deleteWriter
 * and unloading; anything that cannot be proven is quarantined until process exit.
 */
#include "DataManager.h"
#include "StateMonitor.h"
#include "UDPCaster.h"
#include "WtHelper.h"
#include "IDataCaster.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/DLLHelper.hpp"

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <vector>

namespace
{
	enum HostFrameKind
	{
		HFK_Outbound = 1,	// DataManager -> writer call
		HFK_Inbound = 2		// writer -> sink callback
	};

	struct HostFrame
	{
		const void*	_host;
		int			_kind;
	};

	// Frames of every WriterHost active on this thread (innermost last).
	thread_local std::vector<HostFrame> t_host_frames;

	bool threadInFrame(const void* host)
	{
		for (const HostFrame& frame : t_host_frames)
		{
			if (frame._host == host)
				return true;
		}
		return false;
	}

	void popFrame(const void* host, int kind) noexcept
	{
		for (auto it = t_host_frames.rbegin(); it != t_host_frames.rend(); ++it)
		{
			if (it->_host == host && it->_kind == kind)
			{
				t_host_frames.erase(std::next(it).base());
				return;
			}
		}
	}

	const uint32_t DRAIN_SLICE_MS = 1000;
	const uint32_t DRAIN_PROGRESS_LOG_MS = 10000;
}

//////////////////////////////////////////////////////////////////////////
// WriterHost
class DataManager::WriterHost final : public IDataWriterSink, public std::enable_shared_from_this<DataManager::WriterHost>
{
public:
	typedef DataManager::WriterLifecycleState State;

	// ---- IDataWriterSink (the object handed to the writer) ----
	virtual IBaseDataMgr* getBDMgr() override
	{
		StdUniqueLock lock(_mtx);
		if (_detached.load())
			return NULL;
		return _bd_mgr;
	}

	virtual bool canSessionReceive(const char* sid) override
	{
		Inbound in(*this);
		if (!in)
			return false;
		//By Wesley @ 2021.12.27
		//如果状态机为NULL，说明是全天候模式，直接返回true即可
		if (in._state_mon == NULL)
			return true;
		return in._state_mon->isInState(sid, SS_RECEIVING);
	}

	virtual void broadcastTick(WTSTickData* curTick) override { fanout(curTick, "tick"); }
	virtual void broadcastOrdQue(WTSOrdQueData* curOrdQue) override { fanout(curOrdQue, "order queue"); }
	virtual void broadcastOrdDtl(WTSOrdDtlData* curOrdDtl) override { fanout(curOrdDtl, "order detail"); }
	virtual void broadcastTrans(WTSTransData* curTrans) override { fanout(curTrans, "transaction"); }

	virtual CodeSet* getSessionComms(const char* sid) override
	{
		Inbound in(*this);
		if (!in || in._bd_mgr == NULL)
			return NULL;
		return in._bd_mgr->getSessionComms(sid);
	}

	virtual uint32_t getTradingDate(const char* pid) override
	{
		Inbound in(*this);
		if (!in || in._bd_mgr == NULL)
			return 0;
		return in._bd_mgr->getTradingDate(pid);
	}

	virtual void outputLog(WTSLogLevel ll, const char* message) override
	{
		WTSLogger::log_raw(ll, message);
	}

public:
	// Pins the host for one writer callback; snapshots borrowed objects.
	class Inbound
	{
	public:
		explicit Inbound(WriterHost& host) : _host(host)
		{
			t_host_frames.push_back(HostFrame{ &host, HFK_Inbound });
			StdUniqueLock lock(host._mtx);
			if (host._detached.load())
			{
				lock.unlock();
				popFrame(&host, HFK_Inbound);
				return;
			}
			++host._inbound;
			_entered = true;
			_casters = host._casters;
			_bd_mgr = host._bd_mgr;
			_state_mon = host._state_mon;
		}
		~Inbound()
		{
			if (!_entered)
				return;
			popFrame(&_host, HFK_Inbound);
			{
				StdUniqueLock lock(_host._mtx);
				--_host._inbound;
			}
			_host._cond.notify_all();
		}
		Inbound(const Inbound&) = delete;
		Inbound& operator=(const Inbound&) = delete;
		explicit operator bool() const { return _entered; }

		std::shared_ptr<const std::vector<IDataCaster*>> _casters;
		WTSBaseDataMgr*	_bd_mgr = NULL;
		StateMonitor*	_state_mon = NULL;

	private:
		WriterHost&	_host;
		bool		_entered = false;
	};

	// Pins the host and its writer for one DataManager -> writer call.
	class Outbound
	{
	public:
		explicit Outbound(const std::shared_ptr<WriterHost>& host) : _host(host)
		{
			if (!_host)
				return;
			t_host_frames.push_back(HostFrame{ _host.get(), HFK_Outbound });
			StdUniqueLock lock(_host->_mtx);
			if (_host->_state != DataManager::WLS_Ready || _host->_writer == NULL)
			{
				lock.unlock();
				popFrame(_host.get(), HFK_Outbound);
				return;
			}
			++_host->_outbound;
			_writer = _host->_writer;
		}
		~Outbound()
		{
			if (_writer == NULL)
				return;
			popFrame(_host.get(), HFK_Outbound);
			_host->leaveOutbound();
		}
		Outbound(const Outbound&) = delete;
		Outbound& operator=(const Outbound&) = delete;
		explicit operator bool() const { return _writer != NULL; }
		IDataWriter* writer() const { return _writer; }

	private:
		std::shared_ptr<WriterHost>	_host;	// keeps the host alive through the epilogue
		IDataWriter*				_writer = NULL;
	};

	template<typename T>
	void fanout(T* data, const char* what)
	{
		Inbound in(*this);
		if (!in || !in._casters)
			return;
		for (IDataCaster* caster : *in._casters)
		{
			// A callback may have destroyed the owner of the casters (quarantine/destruction).
			if (_detached.load())
				break;
			try
			{
				caster->broadcast(data);
			}
			catch (const std::exception& ex)
			{
				WTSLogger::error("Data caster failed to broadcast {}: {}", what, ex.what());
			}
			catch (...)
			{
				WTSLogger::error("Data caster failed to broadcast {} with an unknown exception", what);
			}
		}
	}

	void addCaster(IDataCaster* caster)
	{
		StdUniqueLock lock(_mtx);
		std::shared_ptr<std::vector<IDataCaster*>> casters = std::make_shared<std::vector<IDataCaster*>>(*_casters);
		casters->emplace_back(caster);
		_casters = casters;
	}

	State state() const
	{
		StdUniqueLock lock(_mtx);
		return _state;
	}

	// Last pinned writer call: deliver a queued release at this safe point.
	void leaveOutbound() noexcept
	{
		bool deliver = false;
		{
			StdUniqueLock lock(_mtx);
			if (_outbound > 0)
				--_outbound;
			deliver = (_state == DataManager::WLS_ReleasePending && _outbound == 0 && !threadInFrame(this));
		}
		_cond.notify_all();
		if (deliver)
			finalize();
	}

	/*
	 * release()/destructor entry. ownerGone: the DataManager is being destroyed, so the
	 * borrowed casters/state monitor/base data may disappear with it.
	 */
	void requestRelease(bool ownerGone) noexcept
	{
		try
		{
			if (threadInFrame(this))
			{
				// Card 9d: never release/delete/unload on a frame the writer may return into.
				IDataWriter* writer = NULL;
				const WtDataWriterLifecycleApiV1* lifecycle = NULL;
				bool queued = false;
				{
					StdUniqueLock lock(_mtx);
					if (_state == DataManager::WLS_Ready || _state == DataManager::WLS_Initializing)
						_state = DataManager::WLS_ReleasePending;
					queued = (_state == DataManager::WLS_ReleasePending);
					if (queued && _lifecycle != NULL && !_stop_requested)
					{
						_stop_requested = true;
						writer = _writer;
						lifecycle = _lifecycle;
					}
				}
				if (lifecycle != NULL)
					lifecycle->request_stop(static_cast<void*>(writer));
				if (ownerGone)
				{
					_detached.store(true);	// the owner's casters/monitor/base data are going away
					if (queued)
						hold();	// keep the host for the pending safe point (or until exit)
				}
				if (queued)
					WTSLogger::warn("Data writer release requested from inside a writer call or callback; deferred to a safe point");
				return;
			}

			{
				StdUniqueLock lock(_mtx);
				if (_state == DataManager::WLS_Ready || _state == DataManager::WLS_Initializing)
					_state = DataManager::WLS_ReleasePending;
			}
			finalize();
			if (ownerGone)
				_detached.store(true);
		}
		catch (...)
		{
			quarantine("release threw an exception");
		}
	}

	// Runs only on a thread with no frame of this host.
	void finalize() noexcept
	{
		IDataWriter* writer = NULL;
		const WtDataWriterLifecycleApiV1* lifecycle = NULL;
		FuncDeleteWriter remover = NULL;
		DllHandle module = NULL;
		uint32_t timeoutMs = 0;
		bool sendStop = false;
		{
			StdUniqueLock lock(_mtx);
			// Another thread is finalizing: wait for its outcome (it never waits for us).
			_cond.wait(lock, [this]() { return _state != DataManager::WLS_Finalizing; });
			if (_state != DataManager::WLS_ReleasePending)
				return;
			_state = DataManager::WLS_Finalizing;
			writer = _writer;
			lifecycle = _lifecycle;
			remover = _remover;
			module = _module;
			timeoutMs = _release_timeout_ms;
			sendStop = (lifecycle != NULL && !_stop_requested);
			_stop_requested = _stop_requested || sendStop;
		}

		try
		{
			if (sendStop)
				lifecycle->request_stop(static_cast<void*>(writer));

			// No writer call made through this manager may still be on any stack.
			if (!waitFor([this]() { return _outbound == 0; }, timeoutMs))
			{
				quarantine("writer calls are still in flight after release_timeout_ms");
				return;
			}

			if (lifecycle == NULL)
			{
				// Legacy writer: release from this safe context, then retain (card 9a).
				try
				{
					writer->release();
				}
				catch (...)
				{
					WTSLogger::error("Legacy data writer release threw an exception");
				}
				quarantine("legacy writer exports no lifecycle V1 and cannot prove callback quiescence");
				return;
			}

			const WtDataWriterLifecycleV1Status drained = drain(lifecycle, writer, timeoutMs);
			if (drained != WT_DATA_WRITER_LIFECYCLE_V1_DRAINED)
			{
				quarantine(drained == WT_DATA_WRITER_LIFECYCLE_V1_PENDING
					? "writer did not drain within release_timeout_ms"
					: "writer could not prove drain");
				return;
			}

			// DRAINED: release is a synchronous, callback-free cleanup.
			try
			{
				writer->release();
			}
			catch (...)
			{
				quarantine("data writer release threw after drain");
				return;
			}

			// A callback that started before DRAINED may still be unwinding through the host.
			if (!waitFor([this]() { return _inbound == 0; }, timeoutMs))
			{
				quarantine("writer callbacks are still unwinding after drain");
				return;
			}

			IDataWriter* doomed = writer;
			bool destroyed = false;
			if (remover != NULL)
			{
				try
				{
					remover(doomed);
					destroyed = (doomed == NULL);
				}
				catch (...)
				{
					WTSLogger::error("Destroying data writer failed with an exception");
				}
			}
			if (!destroyed)
			{
				quarantine("data writer was not destroyed by deleteWriter");
				return;
			}

			{
				StdUniqueLock lock(_mtx);
				_writer = NULL;
				_remover = NULL;
				_lifecycle = NULL;
				_module = NULL;
				_bd_mgr = NULL;
				_state_mon = NULL;
				_stop_requested = false;
				_state = DataManager::WLS_Released;
			}
			if (module != NULL)
				DLLHelper::free_library(module);
			unhold();
			_cond.notify_all();
			WTSLogger::info("Data writer drained, released and unloaded");
		}
		catch (...)
		{
			quarantine("writer finalization threw an exception");
		}
	}

	WtDataWriterLifecycleV1Status drain(const WtDataWriterLifecycleApiV1* lifecycle, IDataWriter* writer, uint32_t timeoutMs)
	{
		const auto started = std::chrono::steady_clock::now();
		uint32_t lastProgressLog = 0;
		for (;;)
		{
			WtDataWriterLifecycleV1Status status = lifecycle->can_wait(static_cast<void*>(writer));
			if (status != WT_DATA_WRITER_LIFECYCLE_V1_OK)
				return status;

			const uint32_t elapsed = (uint32_t)std::min<int64_t>(UINT32_MAX - 1,
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
			uint32_t slice = DRAIN_SLICE_MS;
			if (timeoutMs != 0)
			{
				if (elapsed >= timeoutMs)
					slice = 0;
				else
					slice = std::min(slice, timeoutMs - elapsed);
			}

			status = lifecycle->wait_drained(static_cast<void*>(writer), slice);
			if (status != WT_DATA_WRITER_LIFECYCLE_V1_PENDING)
				return status;
			if (timeoutMs != 0 && elapsed >= timeoutMs)
				return status;
			if (elapsed - lastProgressLog >= DRAIN_PROGRESS_LOG_MS)
			{
				lastProgressLog = elapsed;
				WTSLogger::warn("Data writer still draining accepted work after {} ms", elapsed);
			}
		}
	}

	// Retain writer, module and this sink until process exit (card 9a).
	void quarantine(const char* reason) noexcept
	{
		try
		{
			{
				StdUniqueLock lock(_mtx);
				_state = DataManager::WLS_Quarantined;
			}
			_detached.store(true);	// later callbacks no longer reach the owner's objects
			_cond.notify_all();
			hold();
			if (!threadInFrame(this))
				waitFor([this]() { return _inbound == 0; }, _release_timeout_ms);
			WTSLogger::error("Data writer quarantined until process exit: {}", reason);
		}
		catch (...)
		{
		}
	}

	template<typename Pred>
	bool waitFor(Pred pred, uint32_t timeoutMs)
	{
		StdUniqueLock lock(_mtx);
		if (timeoutMs == 0)
		{
			_cond.wait(lock, pred);
			return true;
		}
		return _cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred);
	}

	void hold();
	void unhold() noexcept;

public:
	mutable StdUniqueMutex	_mtx;
	StdCondVariable			_cond;

	// Borrowed from the owner of the DataManager.
	WTSBaseDataMgr*		_bd_mgr = NULL;
	StateMonitor*		_state_mon = NULL;
	std::shared_ptr<const std::vector<IDataCaster*>> _casters = std::make_shared<std::vector<IDataCaster*>>();
	std::atomic<bool>	_detached{ false };

	// Writer slot.
	IDataWriter*		_writer = NULL;
	FuncDeleteWriter	_remover = NULL;
	DllHandle			_module = NULL;
	const WtDataWriterLifecycleApiV1* _lifecycle = NULL;
	State				_state = DataManager::WLS_None;
	uint32_t			_outbound = 0;
	uint32_t			_inbound = 0;
	uint32_t			_release_timeout_ms = 0;
	bool				_stop_requested = false;
};

namespace
{
	// Process-wide owners of hosts that are pending or quarantined. Intentionally leaked:
	// a retained writer may call its sink until the process exits.
	StdUniqueMutex& holdMutex()
	{
		static StdUniqueMutex* mtx = new StdUniqueMutex();
		return *mtx;
	}

	std::vector<std::shared_ptr<DataManager::WriterHost>>& holdList()
	{
		static auto* hosts = new std::vector<std::shared_ptr<DataManager::WriterHost>>();
		return *hosts;
	}
}

void DataManager::WriterHost::hold()
{
	std::shared_ptr<WriterHost> self = shared_from_this();
	StdUniqueLock lock(holdMutex());
	auto& hosts = holdList();
	if (std::find(hosts.begin(), hosts.end(), self) == hosts.end())
		hosts.emplace_back(self);
}

void DataManager::WriterHost::unhold() noexcept
{
	try
	{
		std::shared_ptr<WriterHost> self;
		{
			StdUniqueLock lock(holdMutex());
			auto& hosts = holdList();
			auto it = std::find_if(hosts.begin(), hosts.end(),
				[this](const std::shared_ptr<WriterHost>& host) { return host.get() == this; });
			if (it == hosts.end())
				return;
			self = *it;	// drop the registry reference outside the registry lock
			hosts.erase(it);
		}
	}
	catch (...)
	{
	}
}

size_t DataManager::quarantinedWriterCount()
{
	StdUniqueLock lock(holdMutex());
	size_t count = 0;
	for (const auto& host : holdList())
	{
		if (host->state() == WLS_Quarantined)
			++count;
	}
	return count;
}

//////////////////////////////////////////////////////////////////////////
// DataManager

/**
 * @brief 构造函数
 * @details 创建写入器宿主；宿主在 DataManager 析构后仍可能被隔离保留
 */
DataManager::DataManager()
	: _host(std::make_shared<WriterHost>())
{
}


/**
 * @brief 析构函数
 * @details 以“所有者即将销毁”语义释放写入器：安全上下文内排空并销毁，或隔离保留到进程退出
 */
DataManager::~DataManager()
{
	_host->requestRelease(true);
	if (_host->state() == WLS_Quarantined)
	{
		WTSLogger::error("DataManager destroyed while its data writer is quarantined; the writer, its module "
			"and sink are retained and no longer reach casters, state monitor or base data");
	}
}

DataManager::WriterLifecycleState DataManager::writerLifecycleState() const
{
	return _host->state();
}

void DataManager::add_caster(IDataCaster* caster)
{
	if (caster == NULL)
		return;
	_host->addCaster(caster);
}

/**
 * @brief 检查会话是否已处理
 * @details 检查指定标识符的会话是否已经进行了数据处理
 * @param sid 会话标识符
 * @return bool 是否已处理，true表示已处理，false表示未处理或数据写入器不可用
 */
bool DataManager::isSessionProceeded(const char* sid)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return false;
	return call.writer()->isSessionProceeded(sid);
}

/**
 * @brief 初始化数据管理器
 * @details 加载数据存储模块；仅在模块与工厂可用后才登记基础数据管理器和状态监控器。
 * 重复初始化（含隔离保留的旧实例）被拒绝且保留原实例（卡9c）。
 * @param params 配置参数，包含数据存储模块等配置
 * @param bdMgr 基础数据管理器指针
 * @param stMonitor 状态监控器指针
 * @return bool 初始化是否成功
 */
bool DataManager::init(WTSVariant* params, WTSBaseDataMgr* bdMgr, StateMonitor* stMonitor)
{
	if (params == NULL)
	{
		WTSLogger::error("Initializing of data writer failed: configuration is null");
		return false;
	}

	std::shared_ptr<WriterHost> host = _host;
	WriterLifecycleState previous = WLS_None;
	{
		StdUniqueLock lock(host->_mtx);
		if (host->_state != WLS_None && host->_state != WLS_Released)
		{
			WTSLogger::error("Initializing of data writer failed: data writer is already initialized or retained");
			return false;
		}
		previous = host->_state;
		host->_state = WLS_Initializing;	// reserve against concurrent init
	}
	auto restore = [&host, previous]() {
		StdUniqueLock lock(host->_mtx);
		host->_state = previous;
	};

	const char* configuredModule = params->getCString("module");
	std::string module = configuredModule ? configuredModule : "";
	if (module.empty())
		module = WtHelper::get_module_dir() + DLLHelper::wrap_module("WtDataStorage");
	else
		module = WtHelper::get_module_dir() + DLLHelper::wrap_module(module.c_str());
	
	DllHandle libWriter = DLLHelper::load_library(module.c_str());
	if (libWriter == NULL)
	{
		WTSLogger::error("Initializing of data writer failed: loading module {} failed...", module.c_str());
		restore();
		return false;
	}

	FuncCreateWriter pFuncCreateWriter = (FuncCreateWriter)DLLHelper::get_symbol(libWriter, "createWriter");
	FuncDeleteWriter pFuncDeleteWriter = (FuncDeleteWriter)DLLHelper::get_symbol(libWriter, "deleteWriter");
	if (pFuncCreateWriter == NULL || pFuncDeleteWriter == NULL)
	{
		WTSLogger::error("Initializing of data writer failed: module {} is missing {}", module.c_str(),
			pFuncCreateWriter == NULL ? "createWriter" : "deleteWriter");
		DLLHelper::free_library(libWriter);
		restore();
		return false;
	}

	// Optional lifecycle V1. A getter that is present but invalid fails closed before
	// createWriter; an absent getter marks a legacy writer (quarantined on release).
	const WtDataWriterLifecycleApiV1* lifecycle = NULL;
	WtDataWriterLifecycleV1GetterFn getter = (WtDataWriterLifecycleV1GetterFn)DLLHelper::get_symbol(libWriter,
		WT_DATA_WRITER_LIFECYCLE_V1_GETTER_SYMBOL);
	if (getter != NULL)
	{
		try
		{
			lifecycle = getter(WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR);
		}
		catch (...)
		{
			lifecycle = NULL;
		}
		const bool valid = lifecycle != NULL
			&& lifecycle->struct_size >= sizeof(WtDataWriterLifecycleApiV1)
			&& (lifecycle->abi_version >> 16) == WT_DATA_WRITER_LIFECYCLE_V1_ABI_MAJOR
			&& (lifecycle->capabilities & WT_DATA_WRITER_LIFECYCLE_V1_CAP_DRAIN) != 0
			&& lifecycle->request_stop != NULL && lifecycle->can_wait != NULL && lifecycle->wait_drained != NULL;
		if (!valid)
		{
			WTSLogger::error("Initializing of data writer failed: module {} exports an invalid writer lifecycle V1 table",
				module.c_str());
			DLLHelper::free_library(libWriter);
			restore();
			return false;
		}
	}
	else
	{
		WTSLogger::warn("Data writer module {} exports no lifecycle V1; it will be retained (not unloaded) on release",
			module.c_str());
	}

	IDataWriter* writer = NULL;
	try
	{
		writer = pFuncCreateWriter();
	}
	catch (...)
	{
		WTSLogger::error("Initializing of data writer failed: createWriter threw an exception in {}", module.c_str());
		DLLHelper::free_library(libWriter);
		restore();
		return false;
	}
	if (writer == NULL)
	{
		WTSLogger::error("Initializing of data writer failed: createWriter returned null in {}", module.c_str());
		DLLHelper::free_library(libWriter);
		restore();
		return false;
	}

	{
		StdUniqueLock lock(host->_mtx);
		host->_writer = writer;
		host->_remover = pFuncDeleteWriter;
		host->_module = libWriter;
		host->_lifecycle = lifecycle;
		host->_bd_mgr = bdMgr;
		host->_state_mon = stMonitor;
		host->_release_timeout_ms = params->getUInt32("release_timeout_ms");
		host->_stop_requested = false;
		host->_outbound += 1;	// the init call itself is a writer frame
	}

	bool initialized = false;
	t_host_frames.push_back(HostFrame{ host.get(), HFK_Outbound });
	try
	{
		initialized = writer->init(params, host.get());
	}
	catch (...)
	{
		WTSLogger::error("Initializing of data writer failed: writer initialization threw an exception in {}", module.c_str());
	}
	popFrame(host.get(), HFK_Outbound);

	bool ready = false;
	{
		StdUniqueLock lock(host->_mtx);
		host->_outbound -= 1;
		if (initialized && host->_state == WLS_Initializing)
		{
			host->_state = WLS_Ready;
			ready = true;
		}
		else if (host->_state == WLS_Initializing)
		{
			host->_state = WLS_ReleasePending;	// failed init is released like any writer
		}
	}
	host->_cond.notify_all();

	if (!ready)
	{
		WTSLogger::error("Initializing of data writer failed: writer initialization rejected {}", module.c_str());
		host->finalize();
		return false;
	}

	WTSLogger::info("Data storage module {} loaded{}", module, lifecycle != NULL ? " (lifecycle V1)" : " (legacy lifecycle)");
	return true;
}

/**
 * @brief 添加外部历史数据转储器
 * @details 注册一个外部的历史数据转储器，用于将数据转储到外部存储介质
 * @param id 转储器标识符
 * @param dumper 历史数据转储器指针
 */
void DataManager::add_ext_dumper(const char* id, IHisDataDumper* dumper)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return;

	call.writer()->add_ext_dumper(id, dumper);
}

/**
 * @brief 释放数据管理器资源
 * @details 见头文件中的写入器生命周期说明（卡9）
 */
void DataManager::release()
{
	std::shared_ptr<WriterHost> host = _host;
	host->requestRelease(false);
}

/**
 * @brief 写入Tick数据
 * @details 将当前的Tick数据写入到存储中，并根据处理标志进行相应处理
 * @param curTick 当前的Tick数据指针
 * @param procFlag 处理标志，用于控制处理行为
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeTick(WTSTickData* curTick, uint32_t procFlag)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return false;

	return call.writer()->writeTick(curTick, procFlag);
}

/**
 * @brief 写入委托队列数据
 * @details 将当前的委托队列数据写入到存储中
 * @param curOrdQue 当前的委托队列数据指针
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeOrderQueue(WTSOrdQueData* curOrdQue)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return false;

	return call.writer()->writeOrderQueue(curOrdQue);
}

/**
 * @brief 写入委托明细数据
 * @details 将当前的委托明细数据写入到存储中
 * @param curOrdDtl 当前的委托明细数据指针
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeOrderDetail(WTSOrdDtlData* curOrdDtl)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return false;

	return call.writer()->writeOrderDetail(curOrdDtl);
}

/**
 * @brief 写入成交数据
 * @details 将当前的成交数据写入到存储中
 * @param curTrans 当前的成交数据指针
 * @return bool 写入是否成功，true表示成功，false表示失败或数据写入器不可用
 */
bool DataManager::writeTransaction(WTSTransData* curTrans)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return false;

	return call.writer()->writeTransaction(curTrans);
}

/**
 * @brief 获取当前的Tick数据
 * @details 根据合约代码和交易所获取当前的Tick数据
 * @param code 合约代码
 * @param exchg 交易所代码，默认为空字符串
 * @return WTSTickData* 当前的Tick数据指针，如果没有或数据写入器不可用则返回NULL
 */
WTSTickData* DataManager::getCurTick(const char* code, const char* exchg/* = ""*/)
{
	WriterHost::Outbound call(_host);
	if (!call)
		return NULL;

	return call.writer()->getCurTick(code, exchg);
}

/**
 * @brief 转换历史数据
 * @details 根据会话标识符转换对应的历史数据
 * @param sid 会话标识符
 */
void DataManager::transHisData(const char* sid)
{
	WriterHost::Outbound call(_host);
	if (call)
		call.writer()->transHisData(sid);
}

//////////////////////////////////////////////////////////////////////////
#pragma region "IDataWriterSink"
/**
 * @brief 获取基础数据管理器
 * @details 实现IDataWriterSink接口的方法，返回基础数据管理器实例
 * @return IBaseDataMgr* 基础数据管理器指针
 */
IBaseDataMgr* DataManager::getBDMgr()
{
	return _host->getBDMgr();
}

/**
 * @brief 检查会话是否可以接收数据
 * @details 实现IDataWriterSink接口的方法，检查指定标识符的会话是否可以接收数据
 * @param sid 会话标识符
 * @return bool 是否可以接收数据，true表示可以，false表示不可以
 */
bool DataManager::canSessionReceive(const char* sid)
{
	return _host->canSessionReceive(sid);
}

/**
 * @brief 广播Tick数据
 * @details 实现IDataWriterSink接口的方法，将Tick数据广播给所有注册的广播器
 * @param curTick 当前的Tick数据指针
 */
void DataManager::broadcastTick(WTSTickData* curTick)
{
	_host->broadcastTick(curTick);
}

/**
 * @brief 广播委托明细数据
 * @details 实现IDataWriterSink接口的方法，将委托明细数据广播给所有注册的广播器
 * @param curOrdDtl 当前的委托明细数据指针
 */
void DataManager::broadcastOrdDtl(WTSOrdDtlData* curOrdDtl)
{
	_host->broadcastOrdDtl(curOrdDtl);
}

/**
 * @brief 广播委托队列数据
 * @details 实现IDataWriterSink接口的方法，将委托队列数据广播给所有注册的广播器
 * @param curOrdQue 当前的委托队列数据指针
 */
void DataManager::broadcastOrdQue(WTSOrdQueData* curOrdQue)
{
	_host->broadcastOrdQue(curOrdQue);
}

/**
 * @brief 广播成交数据
 * @details 实现IDataWriterSink接口的方法，将成交数据广播给所有注册的广播器
 * @param curTrans 当前的成交数据指针
 */
void DataManager::broadcastTrans(WTSTransData* curTrans)
{
	_host->broadcastTrans(curTrans);
}

/**
 * @brief 获取会话关联的合约集合
 * @details 实现IDataWriterSink接口的方法，获取指定会话关联的所有合约代码
 * @param sid 会话标识符
 * @return CodeSet* 合约代码集合指针
 */
CodeSet* DataManager::getSessionComms(const char* sid)
{
	return _host->getSessionComms(sid);
}

/**
 * @brief 获取交易日期
 * @details 实现IDataWriterSink接口的方法，获取指定品种的当前交易日期
 * @param pid 品种标识符
 * @return uint32_t 交易日期，格式为YYYYMMDD
 */
uint32_t DataManager::getTradingDate(const char* pid)
{
	return _host->getTradingDate(pid);
}

/**
 * @brief 处理解析模块的日志
 * @details 实现IDataWriterSink接口的方法，将解析模块产生的日志输出到日志系统
 * @param ll 日志级别
 * @param message 日志内容
 */
void DataManager::outputLog(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}

#pragma endregion "IDataWriterSink"
