#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>

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

using namespace wtp;

namespace
{

TraderAdapter::OwnerRoutingCapabilities completeCapabilities()
{
	TraderAdapter::OwnerRoutingCapabilities capabilities;
	capabilities.durable_journal = true;
	capabilities.local_order_id_floor = true;
	capabilities.provider_user_tag_round_trip = true;
	capabilities.stable_trade_id_across_push_and_query = true;
	return capabilities;
}

class RecordingSink : public ITrdNotifySink
{
public:
	void on_trade(uint32_t, const char*, bool, double, double) override
	{
		++trade_calls;
		if (trade_hook)
			trade_hook();
	}
	void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
	void on_channel_ready() override {}
	void on_channel_lost() override {}

	std::atomic<uint32_t> trade_calls{ 0 };
	std::function<void()> trade_hook;
};

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

bool routeTrade(TraderAdapter& adapter, const std::string& owner_id)
{
	return adapter.forEachSinkForOwner(owner_id, [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	});
}

} // namespace

TEST(DiffOwnerSinkRegistry, DisabledByDefaultAndRoutesOnlyMatchingOwner)
{
	TraderAdapter adapter;
	RecordingSink ownerA;
	RecordingSink ownerB;
	RecordingSink ordinary;
	adapter.addSink(&ordinary);

	TraderAdapter::OwnerSinkRegistrationToken tokenA;
	TraderAdapter::OwnerSinkRegistrationToken tokenB;
	EXPECT_FALSE(adapter.registerOwnerSink("diff-A", &ownerA, tokenA));
	EXPECT_FALSE(tokenA.valid());
	EXPECT_FALSE(adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	}));
	EXPECT_EQ(0u, ownerA.trade_calls.load());
	EXPECT_EQ(0u, ownerB.trade_calls.load());
	EXPECT_EQ(0u, ordinary.trade_calls.load());

	TraderAdapter::OwnerRoutingCapabilities incomplete;
	EXPECT_FALSE(adapter.configureOwnerRoutingCapabilities(incomplete));
	EXPECT_FALSE(adapter.registerOwnerSink("diff-A", &ownerA, tokenA));
	ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(completeCapabilities()));
	EXPECT_FALSE(adapter.registerOwnerSink("diff-A", &ownerA, tokenA));
	EXPECT_FALSE(tokenA.valid());
	adapter.addSink(&ownerA);
	adapter.addSink(&ownerB);
	ASSERT_TRUE(adapter.registerOwnerSink("diff-A", &ownerA, tokenA));
	ASSERT_TRUE(adapter.registerOwnerSink("diff-B", &ownerB, tokenB));

	TraderAdapter::OwnerSinkRegistrationToken duplicateToken;
	EXPECT_FALSE(adapter.registerOwnerSink("diff-A", &ownerB, duplicateToken));
	EXPECT_FALSE(duplicateToken.valid());

	EXPECT_TRUE(adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	}));
	EXPECT_EQ(1u, ownerA.trade_calls.load());
	EXPECT_EQ(0u, ownerB.trade_calls.load());
	EXPECT_EQ(0u, ordinary.trade_calls.load());

	EXPECT_TRUE(adapter.forEachSinkForOwner("diff-B", [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	}));
	EXPECT_EQ(1u, ownerA.trade_calls.load());
	EXPECT_EQ(1u, ownerB.trade_calls.load());
	EXPECT_EQ(0u, ordinary.trade_calls.load());

	EXPECT_FALSE(adapter.forEachSinkForOwner("unknown-owner", [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	}));
	EXPECT_EQ(1u, ownerA.trade_calls.load());
	EXPECT_EQ(1u, ownerB.trade_calls.load());
	EXPECT_EQ(0u, ordinary.trade_calls.load());

	// The legacy broadcaster still reaches every registered sink.
	adapter.forEachSink([](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	});
	EXPECT_EQ(2u, ownerA.trade_calls.load());
	EXPECT_EQ(2u, ownerB.trade_calls.load());
	EXPECT_EQ(1u, ordinary.trade_calls.load());

	ASSERT_TRUE(adapter.unregisterOwnerSink(tokenA));
	EXPECT_FALSE(adapter.unregisterOwnerSink(tokenA));
	EXPECT_FALSE(adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	}));
	// The owner sink remains classified until removeSink and cannot fall back to
	// ordinary callbacks after its token is retired.
	EXPECT_EQ(2u, ownerA.trade_calls.load());
	EXPECT_EQ(2u, ownerB.trade_calls.load());
	EXPECT_EQ(1u, ordinary.trade_calls.load());
	EXPECT_TRUE(adapter.removeSink(&ownerA));
	EXPECT_TRUE(adapter.removeSink(&ownerB));
	EXPECT_TRUE(adapter.removeSink(&ordinary));
}

TEST(DiffOwnerSinkRegistry, ConcurrentRemoveWaitsForOwnerCallback)
{
	TraderAdapter adapter;
	RecordingSink owner;
	RecordingSink ordinary;
	Gate gate;
	owner.trade_hook = [&gate]() { gate.arriveAndWait(); };
	adapter.addSink(&owner);
	adapter.addSink(&ordinary);
	ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(completeCapabilities()));
	TraderAdapter::OwnerSinkRegistrationToken token;
	ASSERT_TRUE(adapter.registerOwnerSink("diff-A", &owner, token));

	std::thread routing([&adapter]() { routeTrade(adapter, "diff-A"); });
	if (!gate.waitUntilEntered(std::chrono::seconds(2)))
	{
		gate.release();
		routing.join();
		FAIL() << "owner callback did not enter its gate";
		return;
	}

	std::promise<bool> removePromise;
	std::future<bool> removeResult = removePromise.get_future();
	std::thread remover([&adapter, &owner, &removePromise]() {
		removePromise.set_value(adapter.removeSink(&owner));
	});
	EXPECT_EQ(std::future_status::timeout, removeResult.wait_for(std::chrono::milliseconds(50)));

	gate.release();
	routing.join();
	EXPECT_TRUE(removeResult.get());
	remover.join();
	EXPECT_FALSE(adapter.unregisterOwnerSink(token));
	EXPECT_FALSE(adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink* sink) {
		sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
	}));
	EXPECT_EQ(1u, owner.trade_calls.load());
	EXPECT_EQ(0u, ordinary.trade_calls.load());
	EXPECT_TRUE(adapter.removeSink(&ordinary));
}

TEST(DiffOwnerSinkRegistry, ReentrantUnregisterAndRemoveDoNotHoldRoutingLocks)
{
	TraderAdapter adapter;
	RecordingSink owner;
	RecordingSink ordinary;
	adapter.addSink(&owner);
	adapter.addSink(&ordinary);
	ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(completeCapabilities()));
	TraderAdapter::OwnerSinkRegistrationToken token;
	ASSERT_TRUE(adapter.registerOwnerSink("diff-A", &owner, token));

	bool ownerUnregistered = true;
	bool sinkRemoved = true;
	owner.trade_hook = [&]() {
		ownerUnregistered = adapter.unregisterOwnerSink(token);
		sinkRemoved = adapter.removeSink(&owner);
	};
	const bool routed = routeTrade(adapter, "diff-A");

	EXPECT_TRUE(routed); // Invocation began before the reentrant unregister.
	EXPECT_FALSE(ownerUnregistered);
	EXPECT_FALSE(sinkRemoved);
	EXPECT_EQ(1u, owner.trade_calls.load());
	EXPECT_EQ(0u, ordinary.trade_calls.load());
	EXPECT_FALSE(adapter.unregisterOwnerSink(token));

	owner.trade_hook = std::function<void()>();
	adapter.addSink(&owner);
	TraderAdapter::OwnerSinkRegistrationToken replacementToken;
	ASSERT_TRUE(adapter.registerOwnerSink("diff-A", &owner, replacementToken));
	EXPECT_FALSE(adapter.unregisterOwnerSink(token));
	EXPECT_TRUE(adapter.unregisterOwnerSink(replacementToken));
	EXPECT_TRUE(adapter.removeSink(&owner));
	EXPECT_TRUE(adapter.removeSink(&ordinary));
}

#ifdef WT_OWNER_ROUTING_TEST_HOOKS
namespace {
std::atomic<Gate*> ownerLookupGate{nullptr};
}

// Called only by a specially compiled TraderAdapter TU at the race boundary.
void wtOwnerRoutingTestAfterLookup()
{
	Gate* gate = ownerLookupGate.load(std::memory_order_acquire);
	if (gate)
		gate->arriveAndWait();
}

namespace {
enum class RouteInterruption { Unregister, Remove, Stop, Replace };

void verifyInterruptedOwnerRoute(RouteInterruption interruption)
{
	TraderAdapter adapter;
	RecordingSink oldOwner;
	RecordingSink replacement;
	adapter.addSink(&oldOwner);
	ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(completeCapabilities()));
	TraderAdapter::OwnerSinkRegistrationToken token;
	ASSERT_TRUE(adapter.registerOwnerSink("diff-A", &oldOwner, token));
	Gate gate;
	ownerLookupGate.store(&gate, std::memory_order_release);
	bool routed = true;
	std::thread route([&]() {
		routed = adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink* sink) {
			sink->on_trade(17, "EX.TEST", true, 1.0, 100.0);
		});
	});
	if (!gate.waitUntilEntered(std::chrono::seconds(2)))
	{
		gate.release();
		route.join();
		ownerLookupGate.store(nullptr, std::memory_order_release);
		FAIL() << "owner route did not reach the lookup barrier";
		return;
	}

	TraderAdapter::OwnerSinkRegistrationToken replacementToken;
	switch (interruption)
	{
	case RouteInterruption::Unregister:
		EXPECT_TRUE(adapter.unregisterOwnerSink(token));
		break;
	case RouteInterruption::Remove:
		EXPECT_TRUE(adapter.removeSink(&oldOwner));
		break;
	case RouteInterruption::Stop:
		adapter.requestStopSinkNotifications();
		break;
	case RouteInterruption::Replace:
		EXPECT_TRUE(adapter.unregisterOwnerSink(token));
		EXPECT_TRUE(adapter.removeSink(&oldOwner));
		adapter.addSink(&replacement);
		EXPECT_TRUE(adapter.registerOwnerSink("diff-A", &replacement, replacementToken));
		break;
	}
	gate.release();
	route.join();
	ownerLookupGate.store(nullptr, std::memory_order_release);
	EXPECT_FALSE(routed);
	EXPECT_EQ(0u, oldOwner.trade_calls.load());
	EXPECT_EQ(0u, replacement.trade_calls.load());
	if (interruption == RouteInterruption::Stop)
		EXPECT_TRUE(adapter.waitSinkNotificationsStopped());
	else if (interruption == RouteInterruption::Replace)
	{
		EXPECT_TRUE(adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink* sink) {
			sink->on_trade(18, "EX.TEST", true, 1.0, 100.0);
		}));
		EXPECT_EQ(1u, replacement.trade_calls.load());
		EXPECT_TRUE(adapter.unregisterOwnerSink(replacementToken));
		EXPECT_TRUE(adapter.removeSink(&replacement));
	}
	else if (interruption == RouteInterruption::Unregister)
		EXPECT_TRUE(adapter.removeSink(&oldOwner));
}
} // namespace

TEST(DiffOwnerSinkDelivery, UnregisterBeforeCallbackReportsNotDelivered)
{
	verifyInterruptedOwnerRoute(RouteInterruption::Unregister);
}
TEST(DiffOwnerSinkDelivery, RemoveBeforeCallbackReportsNotDelivered)
{
	verifyInterruptedOwnerRoute(RouteInterruption::Remove);
}
TEST(DiffOwnerSinkDelivery, StopBeforeCallbackReportsNotDelivered)
{
	verifyInterruptedOwnerRoute(RouteInterruption::Stop);
}
TEST(DiffOwnerSinkDelivery, ReplacementCannotReceiveOldReservation)
{
	verifyInterruptedOwnerRoute(RouteInterruption::Replace);
}
#endif

TEST(DiffOwnerSinkDelivery, CallbackExceptionPropagatesAndReleasesReservation)
{
	TraderAdapter adapter;
	RecordingSink owner;
	adapter.addSink(&owner);
	ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(completeCapabilities()));
	TraderAdapter::OwnerSinkRegistrationToken token;
	ASSERT_TRUE(adapter.registerOwnerSink("diff-A", &owner, token));
	EXPECT_THROW(adapter.forEachSinkForOwner("diff-A", [](ITrdNotifySink*) {
		throw std::runtime_error("intentional callback exception");
	}), std::runtime_error);
	EXPECT_TRUE(adapter.unregisterOwnerSink(token));
	EXPECT_TRUE(adapter.removeSink(&owner));
}
