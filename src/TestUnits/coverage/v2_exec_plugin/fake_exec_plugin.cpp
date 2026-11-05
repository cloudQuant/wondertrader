#include "Includes/IExecPluginV2.h"
#include "Includes/ExecuteDefs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#define COV_EXPORT __declspec(dllexport)
#else
#define COV_EXPORT __attribute__((visibility("default")))
#endif

#ifndef COV_EXEC_PLUGIN_MODE
#define COV_EXEC_PLUGIN_MODE 0
#endif

struct WtExecPluginApiV2Abi20Prefix
{
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t reserved[2];
	WtExecPluginV2CreateFactoryFn create_factory;
	WtExecPluginV2DestroyFactoryFn destroy_factory;
	WtExecPluginV2GetFactoryNameFn get_factory_name;
	WtExecPluginV2CreateUnitFn create_unit;
	WtExecPluginV2DestroyUnitFn destroy_unit;
	WtExecPluginV2InitializeUnitFn initialize_unit;
};

struct WtExecPluginApiV2Abi21Prefix
{
	uint32_t struct_size;
	uint32_t abi_version;
	uint32_t reserved[2];
	WtExecPluginV2CreateFactoryFn create_factory;
	WtExecPluginV2DestroyFactoryFn destroy_factory;
	WtExecPluginV2GetFactoryNameFn get_factory_name;
	WtExecPluginV2CreateUnitFn create_unit;
	WtExecPluginV2DestroyUnitFn destroy_unit;
	WtExecPluginV2InitializeUnitFn initialize_unit;
	WtExecPluginV2Capabilities capabilities;
	WtExecPluginV2RequestQuiesceFn request_quiesce;
	WtExecPluginV2CanWaitQuiesceFn can_wait_quiesce;
	WtExecPluginV2WaitQuiesceFn wait_quiesce;
};

namespace
{
using namespace wtp;

// Named lifecycle fixtures use concrete ExecuteUnit objects; all other fake
// plugin tests retain their original int-backed behavior.
std::mutex blockingUnitMutex;
std::set<void*> blockingUnits;
std::set<void*> contextGateUnits;

void record(const char* event)
{
	const char* path = std::getenv("WT_COV_EXEC_V2_LOG");
	if (path == nullptr || path[0] == '\0')
		return;
	FILE* file = std::fopen(path, "ab");
	if (file == nullptr)
		return;
	std::fprintf(file, "%s\n", event);
	std::fclose(file);
}

#if COV_EXEC_PLUGIN_MODE == 27
struct NullFactoryModuleLifetime
{
	NullFactoryModuleLifetime() { record("module_loaded"); }
	~NullFactoryModuleLifetime() { record("module_unloaded"); }
};

NullFactoryModuleLifetime nullFactoryModuleLifetime;
#endif

class BlockingTickUnit final : public ExecuteUnit
{
public:
	const char* getName() override { return "BlockingUnit"; }
	const char* getFactName() override { return "FakeV2Factory"; }
	void set_position(const char*, double) override {}
	void on_tick(WTSTickData*) override
	{
		record("blocking_on_tick_enter");
		const char* gatePath = std::getenv("WT_COV_EXEC_V2_TICK_GATE");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		bool gateOpened = false;
		while (gatePath != nullptr && gatePath[0] != '\0' &&
			std::chrono::steady_clock::now() < deadline)
		{
			FILE* gate = std::fopen(gatePath, "rb");
			if (gate != nullptr)
			{
				std::fclose(gate);
				gateOpened = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		record(gateOpened ? "blocking_on_tick_released" : "blocking_on_tick_timeout");
	}
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, bool) override {}
	void on_entrust(uint32_t, const char*, bool, const char*) override {}
	void on_channel_ready() override {}
	void on_channel_lost() override {}
};

bool waitForGateFile(const std::string& path)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline)
	{
		FILE* gate = std::fopen(path.c_str(), "rb");
		if (gate != nullptr)
		{
			std::fclose(gate);
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

void tryAllTradeOperations(ExecuteContext* context)
{
	if (context == nullptr)
		return;
	context->buy("SHFE.rb.2510", 10.0, 1.0);
	context->sell("SHFE.rb.2510", 10.0, 1.0);
	context->cancel(uint32_t(7));
	context->cancel("SHFE.rb.2510", true, 1.0);
	context->registerTimer("SHFE.rb.2510", 1000);
}

class ContextGateUnit final : public ExecuteUnit
{
public:
	const char* getName() override { return "ContextGateUnit"; }
	const char* getFactName() override { return "FakeV2Factory"; }
	void set_position(const char*, double) override {}
	void on_tick(WTSTickData*) override {}
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, bool) override {}
	void on_entrust(uint32_t, const char*, bool, const char*) override {}
	void on_channel_ready() override {}
	void on_channel_lost() override {}

	bool initializeContext(ExecuteContext* context, const char* stdCode,
		char* error, uint32_t capacity)
	{
		init(context, stdCode, nullptr);
		if (stdCode != nullptr && std::strcmp(stdCode, "BLOCKED_SUCCESS") == 0)
		{
			if (_ctx != nullptr)
				_ctx->buy("SHFE.rb.2510", 10.0, 1.0);
			record("context_gate_init_waiting");
			const char* gatePath = std::getenv("WT_COV_EXEC_V2_INIT_GATE");
			if (gatePath == nullptr || !waitForGateFile(gatePath))
			{
				clearBorrowedContext();
				setError(error, capacity, "controlled initialization gate timed out");
				return false;
			}
			startPostInitOrderWorker();
			return true;
		}
		if (stdCode != nullptr && std::strcmp(stdCode, "BLOCKED_INIT") == 0)
		{
			tryAllTradeOperations(context);
			std::thread otherThread([context]() { tryAllTradeOperations(context); });
			otherThread.join();
			record("init_trade_attempts_finished");

			const char* gatePath = std::getenv("WT_COV_EXEC_V2_INIT_GATE");
			if (gatePath == nullptr || !waitForGateFile(gatePath))
			{
				clearBorrowedContext();
				setError(error, capacity, "controlled initialization gate timed out");
				return false;
			}
			clearBorrowedContext();
			setError(error, capacity, "controlled failure after blocked trade attempts");
			return false;
		}

		if (stdCode != nullptr && std::strcmp(stdCode, "AFTER_INIT_ORDER") == 0)
			startPostInitOrderWorker();
		return true;
	}

	void waitForQuiesce()
	{
		if (_post_init_worker.joinable())
			_post_init_worker.join();
		clearBorrowedContext();
	}

	void notifyWaitQuiesceEntry()
	{
		if (_ctx != nullptr)
			_ctx->writeLog("fake wait quiesce entry");
	}

	void notifyRequestQuiesceForReentryTest()
	{
		if (_ctx != nullptr)
			_ctx->writeLog("fake request quiesce reentry probe");
	}

private:
	void startPostInitOrderWorker()
	{
		const char* gatePath = std::getenv("WT_COV_EXEC_V2_AFTER_INIT_GATE");
		if (gatePath == nullptr || gatePath[0] == '\0')
			return;
		const std::string path(gatePath);
		_post_init_worker = std::thread([this, path]() {
			if (waitForGateFile(path) && _ctx != nullptr)
			{
				_ctx->registerTimer("SHFE.rb.2510", 1000);
				const OrderIDs firstBuy = _ctx->buy("SHFE.rb.2510", 10.0, 1.0);
				const OrderIDs sell = _ctx->sell("SHFE.rb.2510", 10.0, 1.0);
				const bool cancelById = _ctx->cancel(uint32_t(7));
				const OrderIDs cancelByCode = _ctx->cancel("SHFE.rb.2510", true, 1.0);
				const OrderIDs secondBuy = _ctx->buy("SHFE.rb.2510", 10.0, 1.0);
				_ctx->registerTimer("SHFE.rb.2510", 1000);
				const bool allTradeOperationsForwarded = firstBuy.size() == 1 &&
					sell.size() == 1 && cancelById && cancelByCode.size() == 1 &&
					secondBuy.size() == 1;
				record(allTradeOperationsForwarded
					? "post_init_trade_operations_forwarded"
					: "post_init_trade_operations_rejected");
				record("post_init_buy_finished");
			}
		});
	}

	static void setError(char* error, uint32_t capacity, const char* message)
	{
		if (error == nullptr || capacity == 0)
			return;
		const size_t length = std::strlen(message);
		const size_t copy = length < capacity - 1 ? length : capacity - 1;
		std::memcpy(error, message, copy);
		error[copy] = '\0';
	}
	std::thread _post_init_worker;
};

bool removeBlockingUnit(void* unit)
{
	std::lock_guard<std::mutex> lock(blockingUnitMutex);
	return blockingUnits.erase(unit) != 0;
}

bool removeContextGateUnit(void* unit)
{
	std::lock_guard<std::mutex> lock(blockingUnitMutex);
	return contextGateUnits.erase(unit) != 0;
}

ContextGateUnit* findContextGateUnit(void* unit)
{
	std::lock_guard<std::mutex> lock(blockingUnitMutex);
	return contextGateUnits.find(unit) == contextGateUnits.end()
		? nullptr : static_cast<ContextGateUnit*>(unit);
}

void* WT_EXEC_PLUGIN_V2_CALL createFactory()
{
	record("create_factory");
	if (COV_EXEC_PLUGIN_MODE == 27)
		return nullptr;
	if (COV_EXEC_PLUGIN_MODE == 19)
	{
		const char* gatePath = std::getenv("WT_COV_EXEC_V2_LOAD_GATE");
		if (gatePath != nullptr && gatePath[0] != '\0')
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			bool gateOpened = false;
			while (std::chrono::steady_clock::now() < deadline)
			{
				FILE* gate = std::fopen(gatePath, "rb");
				if (gate != nullptr)
				{
					std::fclose(gate);
					gateOpened = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (!gateOpened)
				return nullptr;
		}
		const char* throwAfterGate = std::getenv("WT_COV_EXEC_V2_LOAD_THROW");
		if (throwAfterGate != nullptr && std::strcmp(throwAfterGate, "1") == 0)
			throw std::runtime_error("controlled factory loader exception");
	}
	return new int(1);
}

void WT_EXEC_PLUGIN_V2_CALL destroyFactory(void* factory)
{
	record("destroy_factory");
	delete static_cast<int*>(factory);
}

const char* WT_EXEC_PLUGIN_V2_CALL getFactoryName(void* factory)
{
	return factory == nullptr ? nullptr : "FakeV2Factory";
}

void* WT_EXEC_PLUGIN_V2_CALL createUnit(void* factory, WtExecPluginV2UnitKind kind, const char* name)
{
	if (factory == nullptr || name == nullptr || kind != WT_EXEC_PLUGIN_V2_UNIT_PLAIN)
		return nullptr;
	record("create_unit");
	if (COV_EXEC_PLUGIN_MODE == 19)
	{
		const char* throwCreate = std::getenv("WT_COV_EXEC_V2_CREATE_THROW");
		if (throwCreate != nullptr && std::strcmp(throwCreate, "1") == 0)
			throw std::runtime_error("controlled create-unit exception");
	}
	if (std::strcmp(name, "BlockingUnit") == 0)
	{
		BlockingTickUnit* unit = new BlockingTickUnit();
		std::lock_guard<std::mutex> lock(blockingUnitMutex);
		blockingUnits.insert(unit);
		return unit;
	}
	if (std::strcmp(name, "ContextGateUnit") == 0)
	{
		ContextGateUnit* unit = new ContextGateUnit();
		std::lock_guard<std::mutex> lock(blockingUnitMutex);
		contextGateUnits.insert(unit);
		return unit;
	}
	return new int(2);
}

void WT_EXEC_PLUGIN_V2_CALL destroyUnit(void*, void* unit)
{
	record("destroy_unit");
	const char* throwDestroy = std::getenv("WT_COV_EXEC_V2_DESTROY_THROW");
	const bool throwAfterDestroy = throwDestroy != nullptr && std::strcmp(throwDestroy, "1") == 0;
	if (removeContextGateUnit(unit))
	{
		delete static_cast<ContextGateUnit*>(unit);
		if (throwAfterDestroy)
			throw std::runtime_error("controlled destroy-unit exception after deletion");
		return;
	}
	if (removeBlockingUnit(unit))
	{
		delete static_cast<ExecuteUnit*>(unit);
		if (throwAfterDestroy)
			throw std::runtime_error("controlled destroy-unit exception after deletion");
		return;
	}
	delete static_cast<int*>(unit);
	if (throwAfterDestroy)
		throw std::runtime_error("controlled destroy-unit exception after deletion");
}

WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL initializeUnit(
	void*, void* unit, void* executeContext, const char* stdCode, void*, char* error, uint32_t capacity)
{
	record("initialize_unit");
	if (stdCode != nullptr && std::strcmp(stdCode, "NULL_CONTEXT_PROBE") == 0)
	{
		if (executeContext == nullptr)
		{
			record("null_context_preserved");
			return WT_EXEC_PLUGIN_V2_OK;
		}
		record("null_context_unexpectedly_wrapped");
		return WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED;
	}
	if (ContextGateUnit* contextUnit = findContextGateUnit(unit))
	{
		return contextUnit->initializeContext(static_cast<ExecuteContext*>(executeContext),
			stdCode, error, capacity)
			? WT_EXEC_PLUGIN_V2_OK : WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED;
	}
	if (COV_EXEC_PLUGIN_MODE == 19)
	{
		const char* throwInit = std::getenv("WT_COV_EXEC_V2_INIT_THROW");
		if (throwInit != nullptr && std::strcmp(throwInit, "1") == 0)
			throw std::runtime_error("controlled initialize-unit exception");
		const char* gatePath = std::getenv("WT_COV_EXEC_V2_INIT_GATE");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		bool gateOpened = false;
		while (gatePath != nullptr && gatePath[0] != '\0' &&
			std::chrono::steady_clock::now() < deadline)
		{
			FILE* gate = std::fopen(gatePath, "rb");
			if (gate != nullptr)
			{
				std::fclose(gate);
				gateOpened = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (!gateOpened)
		{
			const char* failure = "controlled initialization gate timed out";
			if (error != nullptr && capacity != 0)
			{
				const size_t length = std::strlen(failure);
				const size_t copy = length < capacity - 1 ? length : capacity - 1;
				std::memcpy(error, failure, copy);
				error[copy] = '\0';
			}
			return WT_EXEC_PLUGIN_V2_INITIALIZATION_FAILED;
		}
		const char* reenter = std::getenv("WT_COV_EXEC_V2_INIT_REENTER");
		if (reenter != nullptr && std::strcmp(reenter, "1") == 0 && executeContext != nullptr)
		{
			static_cast<ExecuteContext*>(executeContext)->writeLog("fake init reentry probe");
		}
	}
	const char* message = COV_EXEC_PLUGIN_MODE == 4 ? "controlled initialization failure" : "";
	if (capacity != 0 && error != nullptr)
	{
		const size_t length = std::strlen(message);
		const size_t copy = length < capacity - 1 ? length : capacity - 1;
		std::memcpy(error, message, copy);
		error[copy] = '\0';
	}
	return COV_EXEC_PLUGIN_MODE == 4
		? WT_EXEC_PLUGIN_V2_INVALID_CONFIGURATION
		: WT_EXEC_PLUGIN_V2_OK;
}

WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL requestQuiesce(void*, void* unit)
{
	record("request_quiesce");
	const char* reenterHost = std::getenv("WT_COV_EXEC_V2_REQUEST_REENTER_HOST");
	if (reenterHost != nullptr && std::strcmp(reenterHost, "1") == 0)
	{
		if (ContextGateUnit* contextUnit = findContextGateUnit(unit))
			contextUnit->notifyRequestQuiesceForReentryTest();
	}
	static std::atomic<uint32_t> calls(0);
	const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (COV_EXEC_PLUGIN_MODE == 14 && call == 1)
		return WT_EXEC_PLUGIN_V2_INTERNAL_ERROR;
	if (COV_EXEC_PLUGIN_MODE == 16 && call == 1)
		throw std::runtime_error("controlled request quiesce failure");
	return WT_EXEC_PLUGIN_V2_OK;
}

int32_t WT_EXEC_PLUGIN_V2_CALL canWaitQuiesce(void*, void*)
{
	record("can_wait_quiesce");
	static std::atomic<uint32_t> calls(0);
	const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (COV_EXEC_PLUGIN_MODE == 12 && call == 1)
		return 0;
	if (COV_EXEC_PLUGIN_MODE == 13 && call == 1)
		return 2;
	if (COV_EXEC_PLUGIN_MODE == 17 && call == 1)
		throw std::runtime_error("controlled can-wait quiesce failure");
	return 1;
}

int32_t WT_EXEC_PLUGIN_V2_CALL waitQuiesce(void*, void* unit)
{
	if (ContextGateUnit* contextUnit = findContextGateUnit(unit))
	{
		contextUnit->notifyWaitQuiesceEntry();
		record("wait_quiesce");
		contextUnit->waitForQuiesce();
	}
	else
		record("wait_quiesce");
	static std::atomic<uint32_t> calls(0);
	const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (COV_EXEC_PLUGIN_MODE == 15 && call == 1)
		return 0;
	if (COV_EXEC_PLUGIN_MODE == 18 && call == 1)
		throw std::runtime_error("controlled wait quiesce failure");
	return 1;
}

int32_t WT_EXEC_PLUGIN_V2_CALL canWaitFactoryQuiesce(void*)
{
	record("can_wait_factory_quiesce");
	static std::atomic<uint32_t> calls(0);
	const uint32_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (COV_EXEC_PLUGIN_MODE == 24 && call == 1)
		return 0;
	if (COV_EXEC_PLUGIN_MODE == 25 && call == 1)
		return 2;
	if (COV_EXEC_PLUGIN_MODE == 26 && call == 1)
		throw std::runtime_error("controlled factory preflight failure");
	return 1;
}

WtExecPluginApiV2 makeApi()
{
	WtExecPluginApiV2 api = {
		sizeof(WtExecPluginApiV2),
		WT_EXEC_PLUGIN_V2_ABI_VERSION,
		{ 0, 0 },
		createFactory,
		destroyFactory,
		getFactoryName,
		createUnit,
		destroyUnit,
		initializeUnit,
		WT_EXEC_PLUGIN_V2_CAP_QUIESCE |
			WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT,
		requestQuiesce,
		canWaitQuiesce,
		waitQuiesce,
		canWaitFactoryQuiesce
	};
	if (COV_EXEC_PLUGIN_MODE == 2)
		api.abi_version = (3u << 16);
	else if (COV_EXEC_PLUGIN_MODE == 3)
		api.struct_size = static_cast<uint32_t>(offsetof(WtExecPluginApiV2, initialize_unit));
	else if (COV_EXEC_PLUGIN_MODE == 5)
		api.destroy_unit = nullptr;
	else if (COV_EXEC_PLUGIN_MODE == 7)
		api.capabilities = 0;
	else if (COV_EXEC_PLUGIN_MODE == 8)
		api.wait_quiesce = nullptr;
	else if (COV_EXEC_PLUGIN_MODE == 9)
		api.can_wait_quiesce = nullptr;
	else if (COV_EXEC_PLUGIN_MODE == 10)
		api.struct_size = WT_EXEC_PLUGIN_V2_REQUIRED_SIZE;
	else if (COV_EXEC_PLUGIN_MODE == 11)
		api.request_quiesce = nullptr;
	else if (COV_EXEC_PLUGIN_MODE == 21)
		api.struct_size = WT_EXEC_PLUGIN_V2_QUIESCE_SIZE;
	else if (COV_EXEC_PLUGIN_MODE == 22)
		api.capabilities = WT_EXEC_PLUGIN_V2_CAP_QUIESCE;
	else if (COV_EXEC_PLUGIN_MODE == 23)
		api.can_wait_factory_quiesce = nullptr;
	else if (COV_EXEC_PLUGIN_MODE == 28)
		// Decision K41 positive control: a plugin declaring host-verified order identity.
		api.capabilities |= WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY;
	return api;
}

WtExecPluginApiV2Abi20Prefix makeAbi20Api()
{
	WtExecPluginApiV2Abi20Prefix api = {
		sizeof(WtExecPluginApiV2Abi20Prefix),
		(WT_EXEC_PLUGIN_V2_ABI_MAJOR << 16),
		{ 0, 0 },
		createFactory,
		destroyFactory,
		getFactoryName,
		createUnit,
		destroyUnit,
		initializeUnit
	};
	return api;
}

WtExecPluginApiV2Abi21Prefix makeAbi21Api()
{
	WtExecPluginApiV2Abi21Prefix api = {
		sizeof(WtExecPluginApiV2Abi21Prefix),
		(WT_EXEC_PLUGIN_V2_ABI_MAJOR << 16) | 1u,
		{ 0, 0 },
		createFactory,
		destroyFactory,
		getFactoryName,
		createUnit,
		destroyUnit,
		initializeUnit,
		WT_EXEC_PLUGIN_V2_CAP_QUIESCE,
		requestQuiesce,
		canWaitQuiesce,
		waitQuiesce
	};
	return api;
}
}

extern "C" COV_EXPORT const WtExecPluginApiV2* WT_EXEC_PLUGIN_V2_CALL
getExecPluginApiV2(uint32_t requested_major)
{
	if (requested_major != WT_EXEC_PLUGIN_V2_ABI_MAJOR)
		return nullptr;
	record("get_api");
	if (COV_EXEC_PLUGIN_MODE == 6)
	{
		static WtExecPluginApiV2Abi20Prefix abi20 = makeAbi20Api();
		return reinterpret_cast<const WtExecPluginApiV2*>(&abi20);
	}
	if (COV_EXEC_PLUGIN_MODE == 20)
	{
		static WtExecPluginApiV2Abi21Prefix abi21 = makeAbi21Api();
		return reinterpret_cast<const WtExecPluginApiV2*>(&abi21);
	}
	static WtExecPluginApiV2 api = makeApi();
	return &api;
}
