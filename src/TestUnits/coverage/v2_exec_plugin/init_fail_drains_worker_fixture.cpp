#include "Includes/IExecPluginV2.h"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>


namespace
{
std::atomic<uint32_t> g_active_initializations{0};

struct InitializationActivity
{
	InitializationActivity() { g_active_initializations.fetch_add(1, std::memory_order_acq_rel); }
	~InitializationActivity() { g_active_initializations.fetch_sub(1, std::memory_order_acq_rel); }
};

void writeError(char* error, uint32_t capacity, const char* message)
{
	if (error == nullptr || capacity == 0)
		return;
	const size_t length = std::strlen(message);
	const size_t copy = length < capacity - 1 ? length : capacity - 1;
	std::memcpy(error, message, copy);
	error[copy] = '\0';
}

struct Factory
{};

struct Unit
{
	std::mutex mutex;
	std::condition_variable changed;
	bool worker_entered = false;
	bool stop_worker = false;
	bool worker_created = false;
	std::atomic<bool> worker_exited{false};
	std::thread worker;
};

bool stopAndJoinWorkerOrTerminate(Unit* unit) noexcept
{
	bool joinFailed = false;
	try
	{
		{
			std::lock_guard<std::mutex> lock(unit->mutex);
			unit->stop_worker = true;
		}
		unit->changed.notify_all();
		if (unit->worker.joinable())
		{
			try
			{
				unit->worker.join();
			}
			catch (...)
			{
				joinFailed = true;
				if (unit->worker.joinable())
					unit->worker.join();
			}
		}
	}
	catch (...)
	{
		std::terminate();
	}
	if (!unit->worker_exited.load(std::memory_order_acquire))
		std::terminate();
	return joinFailed;
}

void record(const char* event)
{
	const char* path = std::getenv("WT_COV_EXEC_V2_DRAIN_LOG");
	if (path == nullptr || path[0] == '\0')
		return;
	FILE* file = std::fopen(path, "ab");
	if (file == nullptr)
		return;
	std::fprintf(file, "%s\n", event);
	std::fclose(file);
}

void* WT_EXEC_PLUGIN_V2_CALL createFactory()
{
	record("create_factory");
	try
	{
		return new Factory();
	}
	catch (...)
	{
		record("create_factory_failed");
		return nullptr;
	}
}

void WT_EXEC_PLUGIN_V2_CALL destroyFactory(void* factory)
{
	record("destroy_factory");
	delete static_cast<Factory*>(factory);
}

const char* WT_EXEC_PLUGIN_V2_CALL getFactoryName(void* factory)
{
	return factory == nullptr ? nullptr : "InitFailDrainsWorker";
}

void* WT_EXEC_PLUGIN_V2_CALL createUnit(
	void* factory, WtExecPluginV2UnitKind kind, const char* name)
{
	if (factory == nullptr || kind != WT_EXEC_PLUGIN_V2_UNIT_PLAIN || name == nullptr)
		return nullptr;
	record("create_unit");
	try
	{
		return new Unit();
	}
	catch (...)
	{
		record("create_unit_failed");
		return nullptr;
	}
}

void WT_EXEC_PLUGIN_V2_CALL destroyUnit(void*, void* opaque_unit)
{
	Unit* unit = static_cast<Unit*>(opaque_unit);
	if (unit == nullptr)
		return;
	if (unit->worker.joinable())
	{
		record("destroy_while_worker_joinable");
		stopAndJoinWorkerOrTerminate(unit);
	}
	if (unit->worker_created && !unit->worker_exited.load(std::memory_order_acquire))
		std::terminate();
	if (unit->worker_exited.load(std::memory_order_acquire))
		record("destroy_after_worker_exit");
	else
		record("destroy_before_worker_exit");
	record("destroy_unit");
	delete unit;
}

WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL initializeUnit(
	void*, void* opaque_unit, void*, const char*, void*, char* error, uint32_t capacity)
{
	Unit* unit = static_cast<Unit*>(opaque_unit);
	if (unit == nullptr)
		return WT_EXEC_PLUGIN_V2_INVALID_ARGUMENT;
	record("initialize_begin");
	InitializationActivity activity;
	try
	{
		unit->worker = std::thread([unit]() {
			try
			{
				{
					std::lock_guard<std::mutex> lock(unit->mutex);
					unit->worker_entered = true;
				}
				unit->changed.notify_all();
				record("worker_started");
				std::unique_lock<std::mutex> lock(unit->mutex);
				unit->changed.wait(lock, [unit]() { return unit->stop_worker; });
				lock.unlock();
			}
			catch (...)
			{
				record("worker_callback_failed");
			}
			unit->worker_exited.store(true, std::memory_order_release);
			unit->changed.notify_all();
			record("worker_exited");
		});
		unit->worker_created = true;
	}
	catch (...)
	{
		record("worker_create_failed");
		writeError(error, capacity, "controlled worker creation failed");
		return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	}

	try
	{
		const char* gatePath = std::getenv("WT_COV_EXEC_V2_DRAIN_INIT_GATE");
		if (gatePath != nullptr && gatePath[0] != '\0')
		{
			{
				std::unique_lock<std::mutex> lock(unit->mutex);
				unit->changed.wait(lock, [unit]() {
					return unit->worker_entered ||
						unit->worker_exited.load(std::memory_order_acquire);
				});
			}
			if (!unit->worker_entered)
			{
				stopAndJoinWorkerOrTerminate(unit);
				record("worker_joined");
				writeError(error, capacity, "controlled worker failed before initialization");
				return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
			}
			record("initialize_waiting");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			bool opened = false;
			while (std::chrono::steady_clock::now() < deadline)
			{
				FILE* gate = std::fopen(gatePath, "rb");
				if (gate != nullptr)
				{
					std::fclose(gate);
					opened = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			if (opened)
			{
				record("initialize_succeeded");
				return WT_EXEC_PLUGIN_V2_OK;
			}
		}

		{
			std::unique_lock<std::mutex> lock(unit->mutex);
			unit->changed.wait(lock, [unit]() {
				return unit->worker_entered ||
					unit->worker_exited.load(std::memory_order_acquire);
			});
		}
		const bool joinFailed = stopAndJoinWorkerOrTerminate(unit);
		record("worker_joined");
		if (!unit->worker_entered)
		{
			writeError(error, capacity, "controlled worker failed before initialization");
			return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
		}
		if (joinFailed)
		{
			record("initialize_worker_join_failed");
			writeError(error, capacity, "controlled worker join required a retry");
			return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
		}

		const char* message = "controlled initialization failure after worker drain";
		writeError(error, capacity, message);
		record("initialize_failed");
		return WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED;
	}
	catch (...)
	{
		record("initialize_callback_failed");
		stopAndJoinWorkerOrTerminate(unit);
		writeError(error, capacity, "controlled initialization callback failure after worker drain");
		return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	}
}

WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL requestQuiesce(void*, void* opaque_unit)
{
	record("request_quiesce");
	Unit* unit = static_cast<Unit*>(opaque_unit);
	if (unit == nullptr)
		return WT_EXEC_PLUGIN_V2_INVALID_ARGUMENT;
	try
	{
		{
			std::lock_guard<std::mutex> lock(unit->mutex);
			unit->stop_worker = true;
		}
		unit->changed.notify_all();
	}
	catch (...)
	{
		record("request_quiesce_failed");
		return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	}
	return WT_EXEC_PLUGIN_V2_OK;
}

int32_t WT_EXEC_PLUGIN_V2_CALL canWaitQuiesce(void*, void*)
{
	record("can_wait_quiesce");
	return 1;
}

int32_t WT_EXEC_PLUGIN_V2_CALL waitQuiesce(void*, void* opaque_unit)
{
	record("wait_quiesce");
	Unit* unit = static_cast<Unit*>(opaque_unit);
	if (unit == nullptr)
		return 0;
	if (unit->worker.joinable())
	{
		try
		{
			unit->worker.join();
		}
		catch (...)
		{
			record("wait_quiesce_join_failed");
			return 0;
		}
	}
	if (!unit->worker_exited.load(std::memory_order_acquire))
		return 0;
	record("worker_joined");
	return 1;
}

int32_t WT_EXEC_PLUGIN_V2_CALL canWaitFactoryQuiesce(void*)
{
	record("can_wait_factory_quiesce");
	return g_active_initializations.load(std::memory_order_acquire) != 0 ? 0 : 1;
}

const WtExecPluginApiV2 API = {
	WT_EXEC_PLUGIN_V2_FACTORY_PREFLIGHT_SIZE,
	WT_EXEC_PLUGIN_V2_ABI_VERSION,
	{0, 0},
	createFactory,
	destroyFactory,
	getFactoryName,
	createUnit,
	destroyUnit,
	initializeUnit,
	WT_EXEC_PLUGIN_V2_CAP_QUIESCE | WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT,
	requestQuiesce,
	canWaitQuiesce,
	waitQuiesce,
	canWaitFactoryQuiesce
};
}

WT_EXEC_PLUGIN_V2_EXTERN_C WT_EXEC_PLUGIN_V2_EXPORT
const WtExecPluginApiV2* WT_EXEC_PLUGIN_V2_CALL
getExecPluginApiV2(uint32_t requested_major)
{
	return requested_major == WT_EXEC_PLUGIN_V2_ABI_MAJOR ? &API : nullptr;
}
