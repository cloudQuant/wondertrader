/**
 * Regression coverage for TraderAdapter sink registration and channel-state
 * delivery. The test uses the adapter's callback entry points directly; it
 * does not load a trader plugin or connect to an external trading system.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/core/noncopyable.hpp>
#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/DLLHelper.hpp"
#include "Share/BoostFile.hpp"
#include "Share/StdUtils.hpp"
#include "Share/SpinMutex.hpp"

#define private public
#include "WtCore/TraderAdapter.h"
#undef private

#include "WtCore/ITrdNotifySink.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSCollection.hpp"

using namespace wtp;

namespace {

class Gate
{
public:
	void arriveAndWait()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_entered = true;
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _released; });
	}

	bool waitUntilEntered(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _entered; });
	}

	void release()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_released = true;
		_condition.notify_all();
	}

private:
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered = false;
	bool _released = false;
};

class RecordingSink : public ITrdNotifySink
{
public:
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
	void on_channel_ready() override
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_events.push_back("ready");
		if (_ready_gate)
			_ready_gate->arriveAndWait();
	}
	void on_channel_lost() override
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_events.push_back("lost");
	}
	void on_account(const char*, double, double, double, double, double, double, double, double, double, double) override
	{
		++account_calls;
		if (_account_gate)
			_account_gate->arriveAndWait();
	}

	void setReadyGate(Gate* gate) { _ready_gate = gate; }
	void setAccountGate(Gate* gate) { _account_gate = gate; }
	std::vector<std::string> events() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _events;
	}

	std::atomic<uint32_t> account_calls{ 0 };

private:
	mutable std::mutex _mutex;
	std::vector<std::string> _events;
	Gate* _ready_gate = NULL;
	Gate* _account_gate = NULL;
};

void setAdapterState(TraderAdapter& adapter, TraderAdapter::AdapterState state)
{
	std::lock_guard<std::mutex> lock(adapter._sink_mutex);
	adapter._state = state;
}

WTSArray* makeOneAccountArray()
{
	WTSArray* accounts = WTSArray::create();
	WTSAccountInfo* account = WTSAccountInfo::create();
	account->setCurrency("CNY");
	account->setBalance(100.0);
	account->setPreBalance(90.0);
	account->setAvailable(80.0);
	accounts->append(account);
	account->release();
	return accounts;
}

} // namespace

TEST(N01TraderAdapterSubscribe, ready_replay_is_idempotent_and_close_clears_ready)
{
	TraderAdapter adapter;
	RecordingSink sink;
	setAdapterState(adapter, TraderAdapter::AS_ALLREADY);

	adapter.addSink(&sink);
	adapter.addSink(&sink);
	EXPECT_EQ((std::vector<std::string>{ "ready" }), sink.events());

	adapter.handleEvent(WTE_Close, 0);
	EXPECT_FALSE(adapter.isReady());
	EXPECT_EQ((std::vector<std::string>{ "ready", "lost" }), sink.events());

	RecordingSink late;
	adapter.addSink(&late);
	EXPECT_TRUE(late.events().empty());
}

TEST(N01TraderAdapterSubscribe, subscribe_ready_and_close_race_keeps_fifo_order)
{
	TraderAdapter adapter;
	RecordingSink first;
	RecordingSink late;
	Gate readyGate;
	first.setReadyGate(&readyGate);
	setAdapterState(adapter, TraderAdapter::AS_TRADES_QRYED);
	adapter.addSink(&first);

	std::thread readyThread([&adapter]() { adapter.onRspAccount(NULL); });
	if (!readyGate.waitUntilEntered(std::chrono::seconds(2)))
	{
		readyGate.release();
		readyThread.join();
		FAIL() << "READY callback did not enter its gate";
		return;
	}

	// This subscriber linearizes after READY and before CLOSE; its initial READY
	// and the subsequent LOST must pass through the same FIFO drain.
	adapter.addSink(&late);
	adapter.handleEvent(WTE_Close, 0);
	EXPECT_FALSE(adapter.isReady());
	readyGate.release();
	readyThread.join();

	EXPECT_EQ((std::vector<std::string>{ "ready", "lost" }), first.events());
	EXPECT_EQ((std::vector<std::string>{ "ready", "lost" }), late.events());
}

TEST(N01TraderAdapterSubscribe, callback_can_reenter_subscribe_and_close)
{
	TraderAdapter adapter;
	RecordingSink late;
	class ReentrantSink : public RecordingSink
	{
	public:
		TraderAdapter* adapter = NULL;
		RecordingSink* late = NULL;
		void on_channel_ready() override
		{
			RecordingSink::on_channel_ready();
			adapter->addSink(late);
			adapter->handleEvent(WTE_Close, 0);
		}
	} first;
	first.adapter = &adapter;
	first.late = &late;
	setAdapterState(adapter, TraderAdapter::AS_ALLREADY);

	adapter.addSink(&first);
	EXPECT_FALSE(adapter.isReady());
	EXPECT_EQ((std::vector<std::string>{ "ready", "lost" }), first.events());
	EXPECT_EQ((std::vector<std::string>{ "ready", "lost" }), late.events());
}

TEST(N01TraderAdapterSubscribe, callback_can_unregister_without_waiting_on_itself)
{
	TraderAdapter adapter;
	class SelfRemovingSink : public RecordingSink
	{
	public:
		TraderAdapter* adapter = NULL;
		bool removed = true;
		void on_channel_ready() override
		{
			removed = adapter->removeSink(this);
			RecordingSink::on_channel_ready();
		}
	} sink;
	sink.adapter = &adapter;
	setAdapterState(adapter, TraderAdapter::AS_ALLREADY);

	adapter.addSink(&sink);
	EXPECT_FALSE(sink.removed);
	EXPECT_EQ((std::vector<std::string>{ "ready" }), sink.events());

	// The callback has returned, so re-registration is safe and replays READY.
	adapter.addSink(&sink);
	EXPECT_EQ((std::vector<std::string>{ "ready", "ready" }), sink.events());
}

TEST(N01TraderAdapterSubscribe, reentrant_remove_of_idle_sink_allows_ready_resubscribe)
{
	TraderAdapter adapter;
	RecordingSink target;
	class RemoveAndResubscribeSink : public RecordingSink
	{
	public:
		TraderAdapter* adapter = NULL;
		ITrdNotifySink* target = NULL;
		bool removed = false;
		void on_channel_ready() override
		{
			RecordingSink::on_channel_ready();
			removed = adapter->removeSink(target);
			adapter->addSink(target);
		}
	} caller;
	caller.adapter = &adapter;
	caller.target = &target;

	// Register target while not ready, then subscribe caller after READY. Its
	// initial callback deterministically removes the not-yet-called target.
	adapter.addSink(&target);
	setAdapterState(adapter, TraderAdapter::AS_ALLREADY);
	adapter.addSink(&caller);

	EXPECT_TRUE(caller.removed);
	EXPECT_EQ((std::vector<std::string>{ "ready" }), caller.events());
	EXPECT_EQ((std::vector<std::string>{ "ready" }), target.events());
}

TEST(N01TraderAdapterSubscribe, remove_sink_waits_for_in_flight_broadcast)
{
	TraderAdapter adapter;
	RecordingSink sink;
	Gate accountGate;
	sink.setAccountGate(&accountGate);
	adapter.addSink(&sink);
	WTSArray* accounts = makeOneAccountArray();

	std::thread broadcast([&adapter, accounts]() { adapter.onRspAccount(accounts); });
	if (!accountGate.waitUntilEntered(std::chrono::seconds(2)))
	{
		accountGate.release();
		broadcast.join();
		accounts->release();
		FAIL() << "account callback did not enter its gate";
		return;
	}

	std::promise<bool> removePromise;
	std::future<bool> removeResult = removePromise.get_future();
	std::thread remover([&adapter, &sink, &removePromise]() {
		removePromise.set_value(adapter.removeSink(&sink));
	});
	EXPECT_EQ(std::future_status::timeout, removeResult.wait_for(std::chrono::milliseconds(50)));

	accountGate.release();
	broadcast.join();
	EXPECT_TRUE(removeResult.get());
	remover.join();
	accounts->release();
}

TEST(N01TraderAdapterSubscribe, concurrent_account_broadcasts_use_safe_sink_snapshots)
{
	TraderAdapter adapter;
	RecordingSink sink;
	adapter.addSink(&sink);
	WTSArray* accounts = makeOneAccountArray();

	const size_t threadCount = 8;
	std::vector<std::thread> broadcasters;
	for (size_t i = 0; i < threadCount; ++i)
		broadcasters.emplace_back([&adapter, accounts]() { adapter.onRspAccount(accounts); });
	for (std::thread& broadcaster : broadcasters)
		broadcaster.join();

	EXPECT_EQ(static_cast<uint32_t>(threadCount), sink.account_calls.load());
	accounts->release();
}

TEST(N01TraderAdapterSubscribe, request_stop_is_idempotent_and_waits_for_broadcast)
{
	TraderAdapter adapter;
	class SelfRemovingSink : public RecordingSink
	{
	public:
		TraderAdapter* adapter = NULL;
		bool removed = true;
		void on_account(const char* currency, double pre, double balance, double dynamic,
			double available, double close, double floating, double margin, double fee,
			double deposit, double withdraw) override
		{
			removed = adapter->removeSink(this);
			RecordingSink::on_account(currency, pre, balance, dynamic, available, close,
				floating, margin, fee, deposit, withdraw);
		}
	} sink;
	sink.adapter = &adapter;
	RecordingSink active;
	RecordingSink late;
	Gate accountGate;
	sink.setAccountGate(&accountGate);
	adapter.addSink(&sink);
	adapter.addSink(&active);
	WTSArray* accounts = makeOneAccountArray();

	std::thread broadcast([&adapter, accounts]() { adapter.onRspAccount(accounts); });
	if (!accountGate.waitUntilEntered(std::chrono::seconds(2)))
	{
		accountGate.release();
		broadcast.join();
		accounts->release();
		FAIL() << "account callback did not enter its gate";
		return;
	}
	EXPECT_FALSE(sink.removed);

	adapter.requestStopSinkNotifications();
	adapter.requestStopSinkNotifications();
	EXPECT_FALSE(adapter.isReady());
	adapter.addSink(&late);
	EXPECT_TRUE(late.events().empty());

	// API release is a separate operation and does not drain an active sink callback.
	adapter.release();
	std::promise<bool> waitPromise;
	std::future<bool> waitResult = waitPromise.get_future();
	std::thread waiter([&adapter, &waitPromise]() {
		waitPromise.set_value(adapter.waitSinkNotificationsStopped());
	});
	EXPECT_EQ(std::future_status::timeout, waitResult.wait_for(std::chrono::milliseconds(50)));
	{
		std::lock_guard<std::mutex> lock(adapter._sink_mutex);
		EXPECT_EQ(static_cast<size_t>(1), adapter._sink_registrations.size());
		EXPECT_EQ(static_cast<size_t>(1), adapter._retired_sink_registrations.size());
	}

	accountGate.release();
	broadcast.join();
	EXPECT_TRUE(waitResult.get());
	waiter.join();
	EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
	EXPECT_TRUE(adapter._sinks.empty());
	EXPECT_TRUE(adapter._sink_registrations.empty());
	EXPECT_TRUE(adapter._retired_sink_registrations.empty());
	accounts->release();
}

TEST(N01TraderAdapterSubscribe, request_stop_waits_for_channel_drain_and_drops_queued_events)
{
	TraderAdapter adapter;
	RecordingSink sink;
	Gate readyGate;
	sink.setReadyGate(&readyGate);
	setAdapterState(adapter, TraderAdapter::AS_TRADES_QRYED);
	adapter.addSink(&sink);

	std::thread readyThread([&adapter]() { adapter.onRspAccount(NULL); });
	if (!readyGate.waitUntilEntered(std::chrono::seconds(2)))
	{
		readyGate.release();
		readyThread.join();
		FAIL() << "READY callback did not enter its gate";
		return;
	}

	adapter.handleEvent(WTE_Close, 0); // LOST is queued behind the blocked READY.
	adapter.requestStopSinkNotifications();
	std::promise<bool> waitPromise;
	std::future<bool> waitResult = waitPromise.get_future();
	std::thread waiter([&adapter, &waitPromise]() {
		waitPromise.set_value(adapter.waitSinkNotificationsStopped());
	});
	EXPECT_EQ(std::future_status::timeout, waitResult.wait_for(std::chrono::milliseconds(50)));

	readyGate.release();
	readyThread.join();
	EXPECT_TRUE(waitResult.get());
	waiter.join();
	EXPECT_EQ((std::vector<std::string>{ "ready" }), sink.events());
	EXPECT_FALSE(adapter.isReady());
}

TEST(N01TraderAdapterSubscribe, wait_from_own_callback_requests_stop_and_returns_false)
{
	TraderAdapter adapter;
	class WaitFromCallbackSink : public RecordingSink
	{
	public:
		TraderAdapter* adapter = NULL;
		bool wait_result = true;
		void on_account(const char* currency, double pre, double balance, double dynamic,
			double available, double close, double floating, double margin, double fee,
			double deposit, double withdraw) override
		{
			adapter->requestStopSinkNotifications();
			wait_result = adapter->waitSinkNotificationsStopped();
			RecordingSink::on_account(currency, pre, balance, dynamic, available, close,
				floating, margin, fee, deposit, withdraw);
		}
	} sink;
	sink.adapter = &adapter;
	adapter.addSink(&sink);
	WTSArray* accounts = makeOneAccountArray();

	adapter.onRspAccount(accounts);
	EXPECT_FALSE(sink.wait_result);
	EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
	EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
	accounts->release();
}

TEST(N01TraderAdapterSubscribe, manager_requests_all_before_waiting_for_all_adapters)
{
	TraderAdapterMgr manager;
	TraderAdapterPtr first(new TraderAdapter());
	TraderAdapterPtr second(new TraderAdapter());
	ASSERT_TRUE(manager.addAdapter("first", first));
	ASSERT_TRUE(manager.addAdapter("second", second));

	RecordingSink firstSink;
	RecordingSink secondSink;
	Gate firstGate;
	Gate secondGate;
	firstSink.setAccountGate(&firstGate);
	secondSink.setAccountGate(&secondGate);
	first->addSink(&firstSink);
	second->addSink(&secondSink);
	WTSArray* accounts = makeOneAccountArray();

	std::thread firstBroadcast([&first, accounts]() { first->onRspAccount(accounts); });
	std::thread secondBroadcast([&second, accounts]() { second->onRspAccount(accounts); });
	const bool firstEntered = firstGate.waitUntilEntered(std::chrono::seconds(2));
	const bool secondEntered = secondGate.waitUntilEntered(std::chrono::seconds(2));
	if (!firstEntered || !secondEntered)
	{
		firstGate.release();
		secondGate.release();
		firstBroadcast.join();
		secondBroadcast.join();
		accounts->release();
		FAIL() << "both adapter callbacks must enter their gates";
		return;
	}

	manager.requestStopSinkNotifications();
	{
		std::lock_guard<std::mutex> firstLock(first->_sink_mutex);
		std::lock_guard<std::mutex> secondLock(second->_sink_mutex);
		EXPECT_TRUE(first->_sink_notifications_stopping);
		EXPECT_TRUE(second->_sink_notifications_stopping);
	}

	std::promise<bool> waitPromise;
	std::future<bool> waitResult = waitPromise.get_future();
	std::thread waiter([&manager, &waitPromise]() {
		waitPromise.set_value(manager.waitSinkNotificationsStopped());
	});
	EXPECT_EQ(std::future_status::timeout, waitResult.wait_for(std::chrono::milliseconds(50)));

	firstGate.release();
	firstBroadcast.join();
	EXPECT_EQ(std::future_status::timeout, waitResult.wait_for(std::chrono::milliseconds(50)));

	secondGate.release();
	secondBroadcast.join();
	EXPECT_TRUE(waitResult.get());
	waiter.join();
	EXPECT_EQ(static_cast<size_t>(2), manager.getAdapters().size());

	manager.release();
	EXPECT_TRUE(manager.getAdapters().empty());
	accounts->release();
}

TEST(N01TraderAdapterSubscribe, manager_callback_wait_failure_preserves_all_adapters)
{
	TraderAdapterMgr manager;
	TraderAdapterPtr first(new TraderAdapter());
	TraderAdapterPtr second(new TraderAdapter());
	ASSERT_TRUE(manager.addAdapter("first", first));
	ASSERT_TRUE(manager.addAdapter("second", second));

	class ManagerWaitSink : public RecordingSink
	{
	public:
		TraderAdapterMgr* manager = NULL;
		bool wait_result = true;
		size_t size_after_wait = 0;
		size_t size_after_release = 0;
		void on_account(const char* currency, double pre, double balance, double dynamic,
			double available, double close, double floating, double margin, double fee,
			double deposit, double withdraw) override
		{
			manager->requestStopSinkNotifications();
			wait_result = manager->waitSinkNotificationsStopped();
			size_after_wait = manager->getAdapters().size();
			manager->release();
			size_after_release = manager->getAdapters().size();
			RecordingSink::on_account(currency, pre, balance, dynamic, available, close,
				floating, margin, fee, deposit, withdraw);
		}
	} sink;
	sink.manager = &manager;
	first->addSink(&sink);
	WTSArray* accounts = makeOneAccountArray();

	first->onRspAccount(accounts);
	EXPECT_FALSE(sink.wait_result);
	EXPECT_EQ(static_cast<size_t>(2), sink.size_after_wait);
	EXPECT_EQ(static_cast<size_t>(2), sink.size_after_release);
	EXPECT_TRUE(manager.waitSinkNotificationsStopped());
	EXPECT_EQ(static_cast<size_t>(2), manager.getAdapters().size());

	manager.release();
	EXPECT_TRUE(manager.getAdapters().empty());
	accounts->release();
}

TEST(N01TraderAdapterSubscribe, manager_callback_query_is_thread_local_nonblocking_and_read_only)
{
	TraderAdapterMgr manager;
	TraderAdapterPtr adapter(new TraderAdapter());
	ASSERT_TRUE(manager.addAdapter("query", adapter));

	class QuerySink : public RecordingSink
	{
	public:
		TraderAdapterMgr* manager = NULL;
		bool callback_query = false;
		void on_account(const char* currency, double pre, double balance, double dynamic,
			double available, double close, double floating, double margin, double fee,
			double deposit, double withdraw) override
		{
			callback_query = manager->isInSinkCallback();
			RecordingSink::on_account(currency, pre, balance, dynamic, available, close,
				floating, margin, fee, deposit, withdraw);
		}
	} sink;
	Gate accountGate;
	sink.manager = &manager;
	sink.setAccountGate(&accountGate);
	adapter->addSink(&sink);
	WTSArray* accounts = makeOneAccountArray();

	std::thread broadcast([&adapter, accounts]() { adapter->onRspAccount(accounts); });
	if (!accountGate.waitUntilEntered(std::chrono::seconds(2)))
	{
		accountGate.release();
		broadcast.join();
		accounts->release();
		FAIL() << "account callback did not enter its gate";
		return;
	}
	EXPECT_TRUE(sink.callback_query);

	std::promise<bool> queryPromise;
	std::future<bool> queryResult = queryPromise.get_future();
	std::thread query([&manager, &queryPromise]() {
		queryPromise.set_value(manager.isInSinkCallback());
	});
	const std::future_status queryStatus = queryResult.wait_for(std::chrono::milliseconds(50));
	accountGate.release();
	broadcast.join();
	EXPECT_EQ(std::future_status::ready, queryStatus);
	EXPECT_FALSE(queryResult.get());
	query.join();
	EXPECT_FALSE(manager.isInSinkCallback());
	EXPECT_EQ(static_cast<size_t>(1), manager.getAdapters().size());
	{
		std::lock_guard<std::mutex> lock(adapter->_sink_mutex);
		EXPECT_FALSE(adapter->_sink_notifications_stopping);
		EXPECT_EQ(static_cast<size_t>(1), adapter->_sink_registrations.size());
	}

	manager.release();
	EXPECT_TRUE(manager.getAdapters().empty());
	accounts->release();
}

