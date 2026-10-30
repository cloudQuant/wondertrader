#include <boost/noncopyable.hpp>
#include <gtest/gtest.h>

#include "WtUftCore/ITrdNotifySink.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/WtHelper.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <filesystem>
#include <map>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH
#error "CoverageTests_uftcore must wire the UFT lifecycle fake plugin"
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace wtp;

namespace
{
class UftLifecycleTestApi : public ITraderApi
{
public:
	bool init(WTSVariant*) override { ++_initCount; return _initResult; }
	void registerSpi(ITraderSpi* spi) override { _spi = spi; if (_registerHook) _registerHook(); }
	void release() override { ++_releaseCount; if (_releaseHook) _releaseHook(); }
	void connect() override { ++_connectCount; if (_connectHook) _connectHook(); }
	int queryPositions() override { ++_positionsQueries; if (_queryPositionsHook) _queryPositionsHook(); return 0; }
	int queryOrders() override { ++_ordersQueries; return 0; }
	int queryTrades() override { ++_tradesQueries; return 0; }
	int queryAccount() override { ++_accountQueries; return 0; }

	ITraderSpi* _spi{nullptr};
	std::function<void()> _registerHook;
	std::function<void()> _connectHook;
	std::function<void()> _queryPositionsHook;
	std::function<void()> _releaseHook;
	bool _initResult{true};
	std::atomic<uint32_t> _initCount{0};
	std::atomic<uint32_t> _positionsQueries{0};
	std::atomic<uint32_t> _ordersQueries{0};
	std::atomic<uint32_t> _tradesQueries{0};
	std::atomic<uint32_t> _accountQueries{0};
	std::atomic<uint32_t> _releaseCount{0};
	std::atomic<uint32_t> _connectCount{0};
};

class UftLifecycleTestSink : public ITrdNotifySink
{
public:
	void on_trade(uint32_t, const char*, bool, uint32_t, double, double) override {}
	void on_order(uint32_t, const char*, bool, uint32_t, double, double, double, bool) override {}
	void on_channel_ready(uint32_t) override { ++_ready; }
	void on_channel_lost() override { ++_lost; }

	std::atomic<uint32_t> _ready{0};
	std::atomic<uint32_t> _lost{0};
};

class UftBlockingSink : public UftLifecycleTestSink
{
public:
	void on_channel_lost() override
	{
		++_lost;
		std::unique_lock<std::mutex> lock(_mutex);
		_entered = true;
		_cv.notify_all();
		_cv.wait(lock, [this]() { return _released; });
	}

	void waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_cv.wait(lock, [this]() { return _entered; });
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_released = true;
		_cv.notify_all();
	}

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	bool _entered{false};
	bool _released{false};
};

class UftBlockingBaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char*, const char*, uint32_t = 0) override
	{
		std::function<void()> hook;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_lookup_entered = true;
			hook = _contract_hook;
			_cv.notify_all();
		}
		if (hook)
			hook();
		return nullptr;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

	void setContractHook(std::function<void()> hook)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_contract_hook = std::move(hook);
	}

	bool waitUntilLookupEntered(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _cv.wait_for(lock, timeout, [this]() { return _lookup_entered; });
	}

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	bool _lookup_entered{false};
	std::function<void()> _contract_hook;
};

class UftOrderedStatusSink : public UftLifecycleTestSink
{
public:
	void on_channel_ready(uint32_t) override
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_events.push_back("ready-enter");
		_entered = true;
		_cv.notify_all();
		_cv.wait(lock, [this]() { return _released; });
		_events.push_back("ready-exit");
		++_ready;
	}

	void on_channel_lost() override
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_events.push_back("lost");
		++_lost;
		_cv.notify_all();
	}

	void waitUntilReadyEntered()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_cv.wait(lock, [this]() { return _entered; });
	}

	void releaseReady()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_released = true;
		_cv.notify_all();
	}

	std::vector<std::string> events()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _events;
	}

private:
	std::mutex _mutex;
	std::condition_variable _cv;
	std::vector<std::string> _events;
	bool _entered{false};
	bool _released{false};
};

class UftReleaseFromSink : public UftLifecycleTestSink
{
public:
	UftReleaseFromSink(TraderAdapter* adapter, UftLifecycleTestApi* api)
		: _adapter(adapter), _api(api)
	{}
	void on_channel_lost() override
	{
		++_lost;
		_adapter->release();
		_releaseCountDuringCallback = _api->_releaseCount.load();
	}
	std::atomic<uint32_t> _releaseCountDuringCallback{UINT32_MAX};

private:
	TraderAdapter* _adapter;
	UftLifecycleTestApi* _api;
};

std::string lifecycleBinDir()
{
	std::string executablePath;
#if defined(_WIN32)
	char buffer[MAX_PATH] = {};
	DWORD size = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
	if (size == 0 || size >= MAX_PATH)
		return std::string();
	executablePath.assign(buffer, size);
#else
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&lifecycleBinDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	executablePath = info.dli_fname;
#endif
	std::filesystem::path path(executablePath);
	path = path.parent_path().parent_path();
	std::string result = path.string();
	if (!result.empty() && result.back() != '/')
		result.push_back('/');
	return result;
}

const char* lifecycleTraderLibraryName()
{
#if defined(_WIN32)
	return "TraderMocker.dll";
#elif defined(__APPLE__)
	return "libTraderMocker.dylib";
#else
	return "libTraderMocker.so";
#endif
}

class LifecycleTempRoot
{
public:
	LifecycleTempRoot()
		: _path(std::filesystem::temp_directory_path() /
			("uft_adapter_lifecycle_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
	{
		std::filesystem::create_directories(_path / "traders");
	}
	~LifecycleTempRoot()
	{
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
	}
	const std::filesystem::path& path() const { return _path; }

private:
	std::filesystem::path _path;
};

class LifecycleInstDirGuard
{
public:
	explicit LifecycleInstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~LifecycleInstDirGuard() { WtHelper::setInstDir(_old.c_str()); }

private:
	std::string _old;
};

class LifecycleCwdGuard
{
public:
	LifecycleCwdGuard() : _old(std::filesystem::current_path()) {}
	~LifecycleCwdGuard()
	{
		std::error_code ec;
		std::filesystem::current_path(_old, ec);
	}

private:
	std::filesystem::path _old;
};

bool stageLifecycleTrader(const LifecycleTempRoot& root)
{
	const std::string binDir = lifecycleBinDir();
	if (binDir.empty())
		return false;
	const std::filesystem::path source = std::filesystem::path(binDir) / lifecycleTraderLibraryName();
	std::error_code ec;
	if (!std::filesystem::exists(source, ec) || ec)
		return false;
	std::filesystem::create_symlink(source, root.path() / "traders" / lifecycleTraderLibraryName(), ec);
	return !ec;
}

const char* lifecycleFakeLibraryName()
{
#if defined(_WIN32)
	return "UftLifecycleFakeTrader.dll";
#elif defined(__APPLE__)
	return "libUftLifecycleFakeTrader.dylib";
#else
	return "libUftLifecycleFakeTrader.so";
#endif
}

bool stageLifecycleFakeTrader(const LifecycleTempRoot& root)
{
	const std::filesystem::path source(WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH);
	std::error_code ec;
	if (!std::filesystem::exists(source, ec) || ec)
		return false;
	std::filesystem::copy_file(source, root.path() / "traders" / lifecycleFakeLibraryName(), ec);
	return !ec;
}

class LifecycleEnvGuard
{
public:
	void set(const char* name, const std::string& value)
	{
		if (_old.find(name) == _old.end())
		{
			const char* current = std::getenv(name);
			_old[name] = current == nullptr ? std::string() : std::string(current);
			_had[name] = current != nullptr;
		}
#if defined(_WIN32)
		_putenv_s(name, value.c_str());
#else
		setenv(name, value.c_str(), 1);
#endif
	}

	~LifecycleEnvGuard()
	{
		for (const auto& item : _old)
		{
#if defined(_WIN32)
			_putenv_s(item.first.c_str(), _had[item.first] ? item.second.c_str() : "");
#else
			if (_had[item.first])
				setenv(item.first.c_str(), item.second.c_str(), 1);
			else
				unsetenv(item.first.c_str());
#endif
		}
	}

private:
	std::map<std::string, std::string> _old;
	std::map<std::string, bool> _had;
};

void resetLifecycleLog(const std::filesystem::path& path)
{
	std::ofstream out(path, std::ios::trunc);
}

std::vector<std::string> readLifecycleLog(const std::filesystem::path& path)
{
	std::vector<std::string> events;
	std::ifstream in(path);
	std::string event;
	while (std::getline(in, event))
		events.push_back(event);
	return events;
}

WTSVariant* makeLifecycleFakeConfig(const char* mode, const std::filesystem::path& logPath)
{
	WTSVariant* params = WTSVariant::createObject();
	params->append("module", "UftLifecycleFakeTrader");
	params->append("lifecycle_mode", mode);
	params->append("lifecycle_log_path", logPath.string().c_str());
	return params;
}

WTSVariant* makeLifecycleTraderConfig(const char* module)
{
	WTSVariant* params = WTSVariant::createObject();
	params->append("module", module);
	params->append("mockerid", (uint32_t)72861);
	params->append("span", (uint32_t)0);
	return params;
}

void makeAdapterReady(TraderAdapter& adapter, UftLifecycleTestApi& api)
{
	ASSERT_TRUE(adapter.initExt("lifecycle", &api, nullptr, nullptr));
	ASSERT_TRUE(adapter.run());
	ASSERT_NE(nullptr, api._spi);
	api._spi->onLoginResult(true, "ok", 20261029);
	WTSArray* emptyResponse = WTSArray::create();
	ASSERT_NE(nullptr, emptyResponse);
	api._spi->onRspPosition(emptyResponse);
	api._spi->onRspOrders(emptyResponse);
	api._spi->onRspTrades(emptyResponse);
	api._spi->onRspAccount(emptyResponse);
	emptyResponse->release();
	ASSERT_EQ(TraderAdapter::AS_ALLREADY, adapter.state());
}

void advanceAdapterToTradesQuery(TraderAdapter& adapter, UftLifecycleTestApi& api)
{
	ASSERT_TRUE(adapter.initExt("status-order", &api, nullptr, nullptr));
	ASSERT_TRUE(adapter.run());
	ASSERT_NE(nullptr, api._spi);
	api._spi->onLoginResult(true, "ok", 20261029);
	WTSArray* emptyResponse = WTSArray::create();
	ASSERT_NE(nullptr, emptyResponse);
	api._spi->onRspPosition(emptyResponse);
	api._spi->onRspOrders(emptyResponse);
	api._spi->onRspTrades(emptyResponse);
	emptyResponse->release();
	ASSERT_EQ(TraderAdapter::AS_TRADES_QRYED, adapter.state());
}
}

TEST(UftTraderAdapterLifecycle, LateSinkReceivesCurrentReadyState)
{
	UftLifecycleTestApi api;
	TraderAdapter adapter;
	UftLifecycleTestSink first;
	UftLifecycleTestSink late;
	adapter.addSink(&first);
	makeAdapterReady(adapter, api);
	ASSERT_EQ(1u, first._ready.load());

	adapter.addSink(&late);
	EXPECT_EQ(1u, late._ready.load());

	adapter.requestStopSinkNotifications();
	EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
}

TEST(UftTraderAdapterLifecycle, ReadyAndLostAreDeliveredInLinearizedOrder)
{
	UftLifecycleTestApi api;
	TraderAdapter adapter;
	UftOrderedStatusSink sink;
	adapter.addSink(&sink);
	advanceAdapterToTradesQuery(adapter, api);

	WTSArray* emptyAccountResponse = WTSArray::create();
	ASSERT_NE(nullptr, emptyAccountResponse);
	std::thread ready([&]() { api._spi->onRspAccount(emptyAccountResponse); });
	sink.waitUntilReadyEntered();
	std::thread lost([&]() { adapter.handleEvent(WTE_Close, 1); });
	lost.join();
	EXPECT_EQ(0u, sink._lost.load());

	sink.releaseReady();
	ready.join();
	emptyAccountResponse->release();
	EXPECT_EQ((std::vector<std::string>{"ready-enter", "ready-exit", "lost"}), sink.events());
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());

	adapter.requestStopSinkNotifications();
	EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
}

TEST(UftTraderAdapterLifecycle, StopRequestBlocksNewCallbacksAndWaitDrainsInFlight)
{
	TraderAdapter adapter;
	UftBlockingSink blocking;
	UftLifecycleTestSink late;
	adapter.addSink(&blocking);

	std::thread callback([&]() { adapter.handleEvent(WTE_Close, 1); });
	blocking.waitUntilEntered();
	adapter.requestStopSinkNotifications();
	adapter.addSink(&late);
	adapter.handleEvent(WTE_Close, 2);
	EXPECT_EQ(0u, late._lost.load());

	auto waiter = std::async(std::launch::async, [&]() { return adapter.waitSinkNotificationsStopped(); });
	EXPECT_EQ(std::future_status::timeout, waiter.wait_for(std::chrono::milliseconds(30)));
	blocking.release();
	callback.join();
	EXPECT_TRUE(waiter.get());
	EXPECT_EQ(1u, blocking._lost.load());
}

TEST(UftTraderAdapterLifecycle, DirectReleaseFromSinkCallbackDefersApiCleanup)
{
	UftLifecycleTestApi api;
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("sink-release-callback", &api, nullptr, nullptr));
	UftReleaseFromSink sink(&adapter, &api);
	adapter.addSink(&sink);

	adapter.handleEvent(WTE_Close, 1);
	EXPECT_EQ(0u, sink._releaseCountDuringCallback.load());
	EXPECT_EQ(0u, api._releaseCount.load());

	adapter.release();
	EXPECT_EQ(1u, api._releaseCount.load());
}

TEST(UftTraderAdapterLifecycle, DirectReleaseFromApiCallbackDefersApiCleanup)
{
	UftLifecycleTestApi api;
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("api-release-callback", &api, nullptr, nullptr));
	ASSERT_TRUE(adapter.run());
	ASSERT_NE(nullptr, api._spi);
	std::atomic<uint32_t> releaseCountDuringCallback{UINT32_MAX};
	api._queryPositionsHook = [&]() {
		adapter.release();
		releaseCountDuringCallback = api._releaseCount.load();
	};

	api._spi->onLoginResult(true, "ok", 20261029);
	EXPECT_EQ(0u, releaseCountDuringCallback.load());
	EXPECT_EQ(0u, api._releaseCount.load());

	adapter.release();
	EXPECT_EQ(1u, api._releaseCount.load());
}

TEST(UftTraderAdapterLifecycle, ManagerRequestsAllAdaptersAndDefersReleaseFromCallback)
{
	TraderAdapterMgr manager;
	TraderAdapterPtr first = std::make_shared<TraderAdapter>();
	TraderAdapterPtr second = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(manager.addAdapter("first", first));
	ASSERT_TRUE(manager.addAdapter("second", second));

	class ReentrantSink : public UftLifecycleTestSink
	{
	public:
		explicit ReentrantSink(TraderAdapterMgr* manager) : _manager(manager) {}
		void on_channel_lost() override
		{
			++_lost;
			_manager->requestStopSinkNotifications();
			_reentrantWaitResult = _manager->waitSinkNotificationsStopped();
			_manager->release();
			_reentrantReleaseRetainedAdapters = _manager->getAdapters().size() == 2;
		}
		TraderAdapterMgr* _manager;
		bool _reentrantWaitResult{true};
		bool _reentrantReleaseRetainedAdapters{false};
	} sink(&manager);
	first->addSink(&sink);

	first->handleEvent(WTE_Close, 1);
	EXPECT_FALSE(sink._reentrantWaitResult);
	EXPECT_TRUE(sink._reentrantReleaseRetainedAdapters);
	EXPECT_EQ(2u, manager.getAdapters().size());
	EXPECT_TRUE(manager.waitSinkNotificationsStopped());

	// The manager barrier covers every adapter before API release or map removal.
	UftLifecycleTestSink afterStop;
	second->addSink(&afterStop);
	second->handleEvent(WTE_Close, 2);
	EXPECT_EQ(0u, afterStop._lost.load());

	manager.release();
	EXPECT_TRUE(manager.getAdapters().empty());
}

TEST(UftTraderAdapterLifecycle, ManagerWaitsForEveryAdapterInFlightGate)
{
	TraderAdapterMgr manager;
	TraderAdapterPtr first = std::make_shared<TraderAdapter>();
	TraderAdapterPtr second = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(manager.addAdapter("first-gate", first));
	ASSERT_TRUE(manager.addAdapter("second-gate", second));
	UftBlockingSink firstGate;
	UftBlockingSink secondGate;
	first->addSink(&firstGate);
	second->addSink(&secondGate);

	std::thread firstCallback([&]() { first->handleEvent(WTE_Close, 1); });
	firstGate.waitUntilEntered();
	std::thread secondCallback([&]() { second->handleEvent(WTE_Close, 2); });
	secondGate.waitUntilEntered();
	manager.requestStopSinkNotifications();
	auto waiter = std::async(std::launch::async, [&]() { return manager.waitSinkNotificationsStopped(); });
	EXPECT_EQ(std::future_status::timeout, waiter.wait_for(std::chrono::milliseconds(30)));

	firstGate.release();
	firstCallback.join();
	EXPECT_EQ(std::future_status::timeout, waiter.wait_for(std::chrono::milliseconds(30)));
	secondGate.release();
	secondCallback.join();
	EXPECT_TRUE(waiter.get());
	manager.release();
	EXPECT_TRUE(manager.getAdapters().empty());
}

TEST(UftTraderAdapterLifecycle, ManagerReleaseReentryDefersAndConcurrentCallerWaits)
{
	UftLifecycleTestApi api;
	TraderAdapterMgr manager;
	TraderAdapterPtr adapter = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(manager.addAdapter("manager-release-gate", adapter));
	ASSERT_TRUE(adapter->initExt("manager-release-gate", &api, nullptr, nullptr));
	std::mutex mutex;
	std::condition_variable cv;
	bool releaseEntered = false;
	bool finishRelease = false;
	api._releaseHook = [&]() {
		// Same-thread manager release from the provider callback must defer to the
		// outer teardown instead of waiting on itself.
		manager.release();
		std::unique_lock<std::mutex> lock(mutex);
		releaseEntered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return finishRelease; });
	};

	std::thread first([&]() { manager.release(); });
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&]() { return releaseEntered; });
	}
	auto second = std::async(std::launch::async, [&]() { manager.release(); });
	EXPECT_EQ(std::future_status::timeout, second.wait_for(std::chrono::milliseconds(30)));
	EXPECT_EQ(1u, api._releaseCount.load());
	{
		std::lock_guard<std::mutex> lock(mutex);
		finishRelease = true;
		cv.notify_all();
	}
	first.join();
	EXPECT_EQ(std::future_status::ready, second.wait_for(std::chrono::seconds(1)));
	second.get();
	EXPECT_EQ(1u, api._releaseCount.load());
	EXPECT_TRUE(manager.getAdapters().empty());
}

TEST(UftTraderAdapterLifecycle, SpiCallbackReleaseReentryDefersAndDrainsBeforeManagerReturns)
{
	EXPECT_EXIT({
		UftLifecycleTestApi api;
		UftBlockingBaseDataMgr baseData;
		TraderAdapterMgr manager;
		TraderAdapterPtr adapter = std::make_shared<TraderAdapter>();
		if (!manager.addAdapter("spi-release-gate", adapter) ||
			!adapter->initExt("spi-release-gate", &api, &baseData, nullptr) || !adapter->run())
			std::_Exit(2);

		std::mutex gateMutex;
		std::condition_variable gateCv;
		bool apiReleaseEntered = false;
		bool callbackAtGate = false;
		bool apiReleaseReturned = false;
		bool allowCallbackExit = false;
		std::atomic<bool> reentrantReleaseReturned{false};
		std::atomic<bool> callbackExited{false};
		api._releaseHook = [&]() {
			std::unique_lock<std::mutex> lock(gateMutex);
			apiReleaseEntered = true;
			gateCv.notify_all();
			if (!gateCv.wait_for(lock, std::chrono::seconds(2), [&]() { return callbackAtGate; }))
				std::_Exit(3);
			apiReleaseReturned = true;
			gateCv.notify_all();
		};
		baseData.setContractHook([&]() {
			{
				std::unique_lock<std::mutex> lock(gateMutex);
				if (!gateCv.wait_for(lock, std::chrono::seconds(2), [&]() { return apiReleaseEntered; }))
					std::_Exit(4);
			}
			adapter->release();
			manager.release();
			reentrantReleaseReturned = true;
			std::unique_lock<std::mutex> lock(gateMutex);
			callbackAtGate = true;
			gateCv.notify_all();
			if (!gateCv.wait_for(lock, std::chrono::seconds(2), [&]() { return apiReleaseReturned; }))
				std::_Exit(5);
			if (!gateCv.wait_for(lock, std::chrono::seconds(2), [&]() { return allowCallbackExit; }))
				std::_Exit(6);
		});

		WTSArray* positions = WTSArray::create();
		WTSPositionItem* position = WTSPositionItem::create("rb2610", "CNY", "SHFE");
		positions->append(position);
		position->release();
		ITraderSpi* spi = api._spi;
		std::thread callback([&]() {
			spi->onRspPosition(positions);
			callbackExited = true;
		});
		if (!baseData.waitUntilLookupEntered(std::chrono::seconds(2)))
			std::_Exit(7);

		auto teardown = std::async(std::launch::async, [&]() { manager.release(); });
		{
			std::unique_lock<std::mutex> lock(gateMutex);
			if (!gateCv.wait_for(lock, std::chrono::seconds(2), [&]() { return apiReleaseReturned; }))
				std::_Exit(8);
		}
		if (teardown.wait_for(std::chrono::milliseconds(50)) != std::future_status::timeout)
			std::_Exit(9); // Release must drain the still-running SPI callback.
		{
			std::lock_guard<std::mutex> lock(gateMutex);
			allowCallbackExit = true;
			gateCv.notify_all();
		}
		if (teardown.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
			std::_Exit(10);
		teardown.get();
		callback.join();
		if (!reentrantReleaseReturned.load() || !callbackExited.load() || !manager.getAdapters().empty())
			std::_Exit(11);
		positions->release();
		std::_Exit(0);
	}, ::testing::ExitedWithCode(0), "");
}

TEST(UftTraderAdapterLifecycle, ManagerRetainsAdapterWhenProviderReleaseThrows)
{
	EXPECT_EXIT({
		UftLifecycleTestApi api;
		TraderAdapterMgr manager;
		TraderAdapterPtr adapter = std::make_shared<TraderAdapter>();
		if (!manager.addAdapter("manager-release-failure", adapter) ||
			!adapter->initExt("manager-release-failure", &api, nullptr, nullptr))
			std::_Exit(2);
		api._releaseHook = []() { throw 1; };
		manager.release();
		if (manager.getAdapters().size() != 1u || api._releaseCount.load() != 1u)
			std::_Exit(3);
		manager.release();
		if (manager.getAdapters().size() != 1u || api._releaseCount.load() != 1u)
			std::_Exit(4);
		std::_Exit(0); // Keep the intentionally undrainable adapter alive until process exit.
	}, ::testing::ExitedWithCode(0), "");
}

TEST(UftTraderAdapterLifecycle, StopWaitIncludesApiConnectStartup)
{
	UftLifecycleTestApi api;
	TraderAdapterMgr manager;
	TraderAdapterPtr adapter = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(manager.addAdapter("connect-gate", adapter));
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool releaseConnect = false;
	api._connectHook = [&]() {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return releaseConnect; });
	};
	ASSERT_TRUE(adapter->initExt("connect-gate", &api, nullptr, nullptr));
	bool runResult = false;
	std::thread runner([&]() { runResult = adapter->run(); });
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&]() { return entered; });
	}
	manager.requestStopSinkNotifications();
	auto releaser = std::async(std::launch::async, [&]() { manager.release(); });
	EXPECT_EQ(std::future_status::timeout, releaser.wait_for(std::chrono::milliseconds(30)));
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseConnect = true;
		cv.notify_all();
	}
	runner.join();
	EXPECT_TRUE(runResult);
	EXPECT_EQ(std::future_status::ready, releaser.wait_for(std::chrono::seconds(1)));
	releaser.get();
	EXPECT_EQ(1u, api._releaseCount.load());
	EXPECT_TRUE(manager.getAdapters().empty());
}

TEST(UftTraderAdapterLifecycle, StopDuringSpiRegistrationPreventsConnectAndKeepsNotLoginState)
{
	UftLifecycleTestApi api;
	TraderAdapterMgr manager;
	TraderAdapterPtr adapter = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(manager.addAdapter("register-gate", adapter));
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool releaseRegistration = false;
	api._registerHook = [&]() {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return releaseRegistration; });
	};
	ASSERT_TRUE(adapter->initExt("register-gate", &api, nullptr, nullptr));
	bool runResult = true;
	std::thread runner([&]() { runResult = adapter->run(); });
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&]() { return entered; });
	}
	EXPECT_EQ(TraderAdapter::AS_LOGINING, adapter->state());
	manager.requestStopSinkNotifications();
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter->state());
	auto releaser = std::async(std::launch::async, [&]() { manager.release(); });
	EXPECT_EQ(std::future_status::timeout, releaser.wait_for(std::chrono::milliseconds(30)));
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseRegistration = true;
		cv.notify_all();
	}
	runner.join();
	EXPECT_FALSE(runResult);
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter->state());
	EXPECT_EQ(0u, api._connectCount.load());
	releaser.get();
	EXPECT_EQ(1u, api._releaseCount.load());
}

TEST(UftTraderAdapterLifecycle, QueryCallLeaseBlocksReleaseAndRejectsReentrantWait)
{
	UftLifecycleTestApi api;
	TraderAdapterMgr manager;
	TraderAdapterPtr adapter = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(manager.addAdapter("query-gate", adapter));
	ASSERT_TRUE(adapter->initExt("query-gate", &api, nullptr, nullptr));
	ASSERT_TRUE(adapter->run());
	ASSERT_NE(nullptr, api._spi);

	std::mutex mutex;
	std::condition_variable cv;
	bool queryEntered = false;
	bool releaseQuery = false;
	bool reentrantWaitResult = true;
	api._queryPositionsHook = [&]() {
		adapter->requestStopSinkNotifications();
		reentrantWaitResult = adapter->waitSinkNotificationsStopped();
		std::unique_lock<std::mutex> lock(mutex);
		queryEntered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return releaseQuery; });
	};

	ITraderSpi* spi = api._spi;
	std::thread loginReply([&]() { spi->onLoginResult(true, "ok", 20261029); });
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&]() { return queryEntered; });
	}
	EXPECT_FALSE(reentrantWaitResult);

	auto releaser = std::async(std::launch::async, [&]() { manager.release(); });
	EXPECT_EQ(std::future_status::timeout, releaser.wait_for(std::chrono::milliseconds(30)));
	EXPECT_EQ(0u, api._releaseCount.load());
	{
		std::lock_guard<std::mutex> lock(mutex);
		releaseQuery = true;
		cv.notify_all();
	}
	loginReply.join();
	EXPECT_EQ(std::future_status::ready, releaser.wait_for(std::chrono::seconds(1)));
	releaser.get();
	EXPECT_EQ(1u, api._releaseCount.load());
	EXPECT_TRUE(manager.getAdapters().empty());
}

TEST(UftTraderAdapterLifecycle, LoginReplyAfterCloseCannotAdvanceClosedSession)
{
	UftLifecycleTestApi api;
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("late-login", &api, nullptr, nullptr));
	ASSERT_TRUE(adapter.run());
	ASSERT_NE(nullptr, api._spi);
	adapter.handleEvent(WTE_Close, 1);
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());

	api._spi->onLoginResult(true, "late response", 20261029);
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
	EXPECT_EQ(0u, api._positionsQueries.load());

	adapter.requestStopSinkNotifications();
	EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
}

TEST(UftTraderAdapterLifecycle, FailedPluginInitReleasesConfigAndAllowsRetry)
{
	LifecycleTempRoot root;
	if (!stageLifecycleTrader(root))
		GTEST_SKIP() << "TraderMocker shared library is unavailable";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());

	WTSVariant* missingModule = makeLifecycleTraderConfig("UftLifecycleMissingModule");
	WTSVariant* validModule = makeLifecycleTraderConfig("TraderMocker");
	ASSERT_NE(nullptr, missingModule);
	ASSERT_NE(nullptr, validModule);
	TraderAdapter adapter;

	EXPECT_FALSE(adapter.init("lifecycle-retry", missingModule, nullptr, nullptr));
	EXPECT_EQ(1u, missingModule->retainCount());
	EXPECT_FALSE(adapter.run());

	EXPECT_TRUE(adapter.init("lifecycle-retry", validModule, nullptr, nullptr));
	EXPECT_EQ(2u, validModule->retainCount());
	adapter.release();
	EXPECT_EQ(1u, validModule->retainCount());
	adapter.release();
	EXPECT_EQ(1u, validModule->retainCount());

	missingModule->release();
	validModule->release();
}

TEST(UftTraderAdapterLifecycle, SuccessfulPluginReleaseIsIdempotentAndClosesAdapter)
{
	LifecycleTempRoot root;
	if (!stageLifecycleTrader(root))
		GTEST_SKIP() << "TraderMocker shared library is unavailable";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());

	WTSVariant* params = makeLifecycleTraderConfig("TraderMocker");
	ASSERT_NE(nullptr, params);
	{
		TraderAdapter adapter;
		ASSERT_TRUE(adapter.init("lifecycle-release", params, nullptr, nullptr));
		EXPECT_EQ(2u, params->retainCount());

		adapter.release();
		EXPECT_EQ(1u, params->retainCount());
		adapter.release();
		EXPECT_EQ(1u, params->retainCount());

		// A released adapter remains closed: init must fail before retaining cfg.
		EXPECT_FALSE(adapter.init("lifecycle-reinit", params, nullptr, nullptr));
		EXPECT_EQ(1u, params->retainCount());
	}
	EXPECT_EQ(1u, params->retainCount());
	params->release();
}

TEST(UftTraderAdapterLifecycle, DestructorReleasesPluginConfigWithoutExplicitRelease)
{
	LifecycleTempRoot root;
	if (!stageLifecycleTrader(root))
		GTEST_SKIP() << "TraderMocker shared library is unavailable";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());

	WTSVariant* params = makeLifecycleTraderConfig("TraderMocker");
	ASSERT_NE(nullptr, params);
	{
		TraderAdapter adapter;
		ASSERT_TRUE(adapter.init("lifecycle-destructor", params, nullptr, nullptr));
		EXPECT_EQ(2u, params->retainCount());
	}
	EXPECT_EQ(1u, params->retainCount());
	params->release();
}

TEST(UftTraderAdapterLifecycle, FailedExternalApiInitRollsBackAndAllowsRetry)
{
	UftLifecycleTestApi failedApi;
	failedApi._initResult = false;
	UftLifecycleTestApi workingApi;
	TraderAdapter adapter;

	EXPECT_FALSE(adapter.initExt("lifecycle-ext-fail", &failedApi, nullptr, nullptr));
	EXPECT_EQ(1u, failedApi._initCount.load());
	EXPECT_EQ(1u, failedApi._releaseCount.load());

	EXPECT_TRUE(adapter.initExt("lifecycle-ext-retry", &workingApi, nullptr, nullptr));
	EXPECT_EQ(1u, workingApi._initCount.load());
	adapter.release();
	EXPECT_EQ(1u, workingApi._releaseCount.load());
	adapter.release();
	EXPECT_EQ(1u, workingApi._releaseCount.load());
}

TEST(UftTraderAdapterLifecycle, ConcurrentReleaseWaitsForSingleApiRelease)
{
	UftLifecycleTestApi api;
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("lifecycle-concurrent-release", &api, nullptr, nullptr));

	std::mutex mutex;
	std::condition_variable cv;
	bool releaseEntered = false;
	bool finishRelease = false;
	api._releaseHook = [&]() {
		std::unique_lock<std::mutex> lock(mutex);
		releaseEntered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return finishRelease; });
	};

	std::thread first([&]() { adapter.release(); });
	{
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&]() { return releaseEntered; });
	}
	auto second = std::async(std::launch::async, [&]() { adapter.release(); });
	EXPECT_EQ(std::future_status::timeout, second.wait_for(std::chrono::milliseconds(30)));
	EXPECT_EQ(1u, api._releaseCount.load());
	{
		std::lock_guard<std::mutex> lock(mutex);
		finishRelease = true;
		cv.notify_all();
	}
	first.join();
	EXPECT_EQ(std::future_status::ready, second.wait_for(std::chrono::seconds(1)));
	second.get();
	EXPECT_EQ(1u, api._releaseCount.load());
}

TEST(UftTraderAdapterLifecycle, FakeFactoryThrowAndNullRollbackConfigAndAllowRetry)
{
	LifecycleTempRoot root;
	ASSERT_TRUE(stageLifecycleFakeTrader(root)) << "UFT lifecycle fake plugin output is missing";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());
	const std::filesystem::path logPath = root.path() / "lifecycle.log";
	LifecycleEnvGuard env;
	env.set("WT_UFT_LIFECYCLE_LOG", logPath.string());
	WTSVariant* params = makeLifecycleFakeConfig("success", logPath);
	ASSERT_NE(nullptr, params);
	TraderAdapter adapter;

	env.set("WT_UFT_LIFECYCLE_CREATE_MODE", "throw");
	EXPECT_FALSE(adapter.init("fake-create-throw", params, nullptr, nullptr));
	EXPECT_EQ(1u, params->retainCount());
	EXPECT_EQ((std::vector<std::string>{"factory.create", "factory.create.throw"}), readLifecycleLog(logPath));

	resetLifecycleLog(logPath);
	env.set("WT_UFT_LIFECYCLE_CREATE_MODE", "null");
	EXPECT_FALSE(adapter.init("fake-create-null", params, nullptr, nullptr));
	EXPECT_EQ(1u, params->retainCount());
	EXPECT_EQ((std::vector<std::string>{"factory.create", "factory.create.null"}), readLifecycleLog(logPath));

	resetLifecycleLog(logPath);
	env.set("WT_UFT_LIFECYCLE_CREATE_MODE", "");
	ASSERT_TRUE(adapter.init("fake-create-retry", params, nullptr, nullptr));
	EXPECT_EQ(2u, params->retainCount());
	adapter.release();
	EXPECT_EQ(1u, params->retainCount());
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleLog(logPath));
	EXPECT_FALSE(adapter.init("fake-create-after-release", params, nullptr, nullptr));
	EXPECT_EQ(1u, params->retainCount());
	params->release();
}

TEST(UftTraderAdapterLifecycle, FakePluginInitFalseAndThrowRollbackInSafeOrderAndAllowRetry)
{
	LifecycleTempRoot root;
	ASSERT_TRUE(stageLifecycleFakeTrader(root)) << "UFT lifecycle fake plugin output is missing";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());
	const std::filesystem::path logPath = root.path() / "lifecycle.log";
	LifecycleEnvGuard env;
	env.set("WT_UFT_LIFECYCLE_LOG", logPath.string());
	TraderAdapter adapter;

	WTSVariant* throwing = makeLifecycleFakeConfig("init_throw", logPath);
	ASSERT_NE(nullptr, throwing);
	EXPECT_FALSE(adapter.init("fake-init-throw", throwing, nullptr, nullptr));
	EXPECT_EQ(1u, throwing->retainCount());
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.init.throw",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleLog(logPath));

	WTSVariant* rejected = makeLifecycleFakeConfig("init_false", logPath);
	ASSERT_NE(nullptr, rejected);
	resetLifecycleLog(logPath);
	EXPECT_FALSE(adapter.init("fake-init-false", rejected, nullptr, nullptr));
	EXPECT_EQ(1u, rejected->retainCount());
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.init.false",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleLog(logPath));

	WTSVariant* working = makeLifecycleFakeConfig("success", logPath);
	ASSERT_NE(nullptr, working);
	resetLifecycleLog(logPath);
	EXPECT_TRUE(adapter.init("fake-init-retry", working, nullptr, nullptr));
	EXPECT_EQ(2u, working->retainCount());
	adapter.release();
	EXPECT_EQ(1u, working->retainCount());
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleLog(logPath));

	throwing->release();
	rejected->release();
	working->release();
}

TEST(UftTraderAdapterLifecycle, FailedInitCleanupFailuresRetainResourcesRejectRetryAndFailStop)
{
	LifecycleTempRoot root;
	ASSERT_TRUE(stageLifecycleFakeTrader(root)) << "UFT lifecycle fake plugin output is missing";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());
	const std::filesystem::path logPath = root.path() / "lifecycle.log";
	LifecycleEnvGuard env;
	env.set("WT_UFT_LIFECYCLE_LOG", logPath.string());

	auto verifyScenario = [&](const char* id, const char* initMode, const char* releaseThrows,
		const char* deleteThrows, const std::vector<std::string>& expectedEvents) {
		env.set("WT_UFT_LIFECYCLE_RELEASE_THROW", releaseThrows);
		env.set("WT_UFT_LIFECYCLE_DELETE_THROW", deleteThrows);
		env.set("WT_UFT_LIFECYCLE_CREATE_MODE", "");
		resetLifecycleLog(logPath);
		WTSVariant* params = makeLifecycleFakeConfig(initMode, logPath);
		ASSERT_NE(nullptr, params);
		EXPECT_EXIT({
			TraderAdapter* adapter = new TraderAdapter();
			if (adapter->init(id, params, nullptr, nullptr))
				std::_Exit(2);
			if (params->retainCount() != 2u)
				std::_Exit(3);
			if (adapter->init(id, params, nullptr, nullptr))
				std::_Exit(4);
			if (params->retainCount() != 2u)
				std::_Exit(5);
			std::_Exit(0); // The provider cleanup failed, so keep the adapter and its dependencies alive.
		}, ::testing::ExitedWithCode(0), "");
		EXPECT_EQ(expectedEvents, readLifecycleLog(logPath));

		WTSVariant* deathParams = makeLifecycleFakeConfig(initMode, logPath);
		ASSERT_NE(nullptr, deathParams);
		EXPECT_DEATH({
			TraderAdapter* adapter = new TraderAdapter();
			if (adapter->init(id, deathParams, nullptr, nullptr))
				std::abort();
			delete adapter;
		}, "");
		EXPECT_EQ(1u, deathParams->retainCount()); // Child process changes are isolated.
		params->release();
		deathParams->release();
	};

	for (const char* initMode : {"init_false", "init_throw"})
	{
		const std::string suffix = initMode;
		verifyScenario(("failed-init-release-throw-" + suffix).c_str(), initMode, "1", "", {
			"factory.create",
			"api.init refs=2",
			std::string("api.init.") + (suffix == "init_false" ? "false" : "throw"),
			"api.release refs=2",
			"api.release.throw"});
		verifyScenario(("failed-init-delete-throw-" + suffix).c_str(), initMode, "", "1", {
			"factory.create",
			"api.init refs=2",
			std::string("api.init.") + (suffix == "init_false" ? "false" : "throw"),
			"api.release refs=2",
			"factory.delete.begin",
			"factory.delete.throw"});
	}
}

TEST(UftTraderAdapterLifecycle, PluginCleanupThrowsAreCaughtAndDestructorFailsClosed)
{
	LifecycleTempRoot root;
	ASSERT_TRUE(stageLifecycleFakeTrader(root)) << "UFT lifecycle fake plugin output is missing";
	LifecycleInstDirGuard instDir(root.path().string() + "/");
	LifecycleCwdGuard cwd;
	std::filesystem::current_path(root.path());
	const std::filesystem::path logPath = root.path() / "lifecycle.log";
	LifecycleEnvGuard env;
	env.set("WT_UFT_LIFECYCLE_LOG", logPath.string());

	WTSVariant* apiReleaseThrows = makeLifecycleFakeConfig("success", logPath);
	ASSERT_NE(nullptr, apiReleaseThrows);
	env.set("WT_UFT_LIFECYCLE_RELEASE_THROW", "1");
	EXPECT_EXIT({
		TraderAdapter* adapter = new TraderAdapter();
		if (!adapter->init("fake-release-throw", apiReleaseThrows, nullptr, nullptr))
			std::_Exit(2);
		adapter->release(); // The exception is contained; cfg stays retained because the API may still use it.
		if (apiReleaseThrows->retainCount() != 2)
			std::_Exit(3);
		std::_Exit(0); // Deliberately isolate the pinned provider resources from the test process.
	}, ::testing::ExitedWithCode(0), "");
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.release refs=2",
		"api.release.throw"}), readLifecycleLog(logPath));
	EXPECT_DEATH({
		TraderAdapter* adapter = new TraderAdapter();
		if (!adapter->init("fake-release-throw-dtor", apiReleaseThrows, nullptr, nullptr))
			std::abort();
		adapter->release();
		delete adapter; // Without a provider stop fence, destroying its SPI owner is unsafe.
	}, "");
	EXPECT_EQ(1u, apiReleaseThrows->retainCount()); // Child process changes are isolated.
	apiReleaseThrows->release();

	resetLifecycleLog(logPath);
	WTSVariant* deleteThrows = makeLifecycleFakeConfig("success", logPath);
	ASSERT_NE(nullptr, deleteThrows);
	env.set("WT_UFT_LIFECYCLE_RELEASE_THROW", "");
	env.set("WT_UFT_LIFECYCLE_DELETE_THROW", "1");
	EXPECT_EXIT({
		TraderAdapter* adapter = new TraderAdapter();
		if (!adapter->init("fake-delete-throw", deleteThrows, nullptr, nullptr))
			std::_Exit(2);
		adapter->release(); // The deleter exception is contained and dependent resources stay pinned.
		if (deleteThrows->retainCount() != 2)
			std::_Exit(3);
		std::_Exit(0);
	}, ::testing::ExitedWithCode(0), "");
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.release refs=2",
		"factory.delete.begin",
		"factory.delete.throw"}), readLifecycleLog(logPath));
	EXPECT_DEATH({
		TraderAdapter* adapter = new TraderAdapter();
		if (!adapter->init("fake-delete-throw-dtor", deleteThrows, nullptr, nullptr))
			std::abort();
		adapter->release();
		delete adapter;
	}, "");
	EXPECT_EQ(1u, deleteThrows->retainCount());
	deleteThrows->release();
}
