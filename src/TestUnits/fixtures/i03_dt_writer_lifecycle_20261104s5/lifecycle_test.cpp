// DataManager / writer lifecycle (iteration 03, card 9: 9a=A+C, 9b=A, 9c=A, 9d=A).
// Items: BUG-I02-NEW-038, BUG-R18-002, BUG-R23-001.
//
// Modules are loaded from $S5_MODULE_DIR:
//   libs5_fake_legacy.dylib    no lifecycle V1 (cannot prove drain)
//   libs5_fake_v1.dylib        lifecycle V1, synchronous
//   libs5_fake_v1_async.dylib  lifecycle V1 with a writer-owned callback thread
//   libWtDataStorage.dylib     the real writer built from the tree under test
// Fake writers append events to $S5_FAKE_TRACE (set per test).
#include "s5_common.h"

#include "WtDtCore/DataManager.h"
#include "WtDtCore/IDataCaster.h"
#include "WtDtCore/WtHelper.h"
#include "WTSTools/WTSBaseDataMgr.h"

#include <dlfcn.h>
#include <functional>
#include <future>
#include <mutex>

#if defined(S5_BASELINE_HOST)
// Red-control build against the pre-fix DataManager, which has no lifecycle observer:
// state/quarantine checks are skipped; event order, crashes and timeouts still decide.
#define S5_EXPECT_STATE(dm, st) do {} while (0)
#define S5_QUARANTINED() size_t(0)
#else
#define S5_EXPECT_STATE(dm, st) EXPECT_EQ((dm).writerLifecycleState(), DataManager::st)
#define S5_QUARANTINED() DataManager::quarantinedWriterCount()
#endif

namespace {

using namespace s5;

class Caster final : public IDataCaster
{
public:
	void broadcast(WTSTickData* tick) override
	{
		_ticks.fetch_add(1);
		if (_onTick)
			_onTick(tick);
	}
	void broadcast(WTSOrdQueData* queue) override
	{
		_queues.fetch_add(1);
		if (_onQueue)
			_onQueue(queue);
	}

	std::atomic<uint32_t> _ticks{ 0 };
	std::atomic<uint32_t> _queues{ 0 };
	std::function<void(WTSTickData*)> _onTick;
	std::function<void(WTSOrdQueData*)> _onQueue;
};

class Lifecycle : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = unique_dir("s5_lifecycle");
		_trace = _dir / "events.log";
		setenv("S5_FAKE_TRACE", _trace.string().c_str(), 1);
		WtHelper::set_module_dir(module_dir().c_str());
	}

	void TearDown() override { unsetenv("S5_FAKE_TRACE"); }

	WTSVariant* config(const char* module, bool async = false, uint32_t timeoutMs = 0)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module);
		cfg->append("path", (_dir.string() + "/store/").c_str());
		cfg->append("cache", "cache.dmb");
		cfg->append("async", async);
		cfg->append("groupsize", 1000u);
		if (timeoutMs != 0)
			cfg->append("release_timeout_ms", timeoutMs);
		return cfg;
	}

	bool init(DataManager& dm, const char* module, bool async = false, uint32_t timeoutMs = 0)
	{
		WTSVariant* cfg = config(module, async, timeoutMs);
		const bool ok = dm.init(cfg, &_bd, nullptr);
		cfg->release();
		return ok;
	}

	std::vector<std::string> events() const { return read_lines(_trace); }

	bool moduleStillLoaded(const char* module) const
	{
		const std::string path = module_dir() + "lib" + module + ".dylib";
		void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
		if (handle == nullptr)
			return false;
		dlclose(handle);	// drop the reference RTLD_NOLOAD added
		return true;
	}

	fs::path _dir;
	fs::path _trace;
	WTSBaseDataMgr _bd;
	Market _market;
};

// NEW-038: release requested inside a synchronous writer callback must not destroy the
// writer while writeTick is still on the stack. Legacy writer -> retained (9a).
TEST_F(Lifecycle, LegacyWriterReleaseInsideCallbackNeverDestroysActiveFrame)
{
	DataManager dm;
	Caster caster;
	dm.add_caster(&caster);
	ASSERT_TRUE(init(dm, "s5_fake_legacy"));
	caster._onTick = [&](WTSTickData*) { dm.release(); };

	EXPECT_TRUE(dm.writeTick(nullptr, 0));

	const auto ev = events();
	EXPECT_GE(index_of(ev, "write-exit"), 0);
	EXPECT_EQ(index_of(ev, "write-exit-dead"), -1);
	EXPECT_EQ(index_of(ev, "delete"), -1) << "legacy writer must not be deleted";
	EXPECT_EQ(index_of(ev, "destroy"), -1);
	// Delivered at the safe point after writeTick returned: legacy release, then quarantine.
	EXPECT_GT(index_of(ev, "release"), index_of(ev, "write-exit"));
	S5_EXPECT_STATE(dm, WLS_Quarantined);
	EXPECT_TRUE(moduleStillLoaded("s5_fake_legacy"));
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
}

// NEW-038 with a V1 writer: stop is requested in the callback, drain/release/delete
// happen only after the active writeTick frame returned.
TEST_F(Lifecycle, V1WriterReleaseInsideCallbackIsDeliveredAtSafePoint)
{
	DataManager dm;
	Caster caster;
	dm.add_caster(&caster);
	ASSERT_TRUE(init(dm, "s5_fake_v1"));
	caster._onTick = [&](WTSTickData*) {
		dm.release();
		S5_EXPECT_STATE(dm, WLS_ReleasePending);
	};

	EXPECT_TRUE(dm.writeTick(nullptr, 0));

	const auto ev = events();
	const long exitAt = index_of(ev, "write-exit");
	ASSERT_GE(exitAt, 0);
	EXPECT_EQ(index_of(ev, "write-exit-dead"), -1);
	EXPECT_LT(index_of(ev, "request-stop"), exitAt);
	EXPECT_GT(index_of(ev, "drained"), exitAt);
	EXPECT_GT(index_of(ev, "release"), index_of(ev, "drained"));
	EXPECT_GT(index_of(ev, "delete"), index_of(ev, "release"));
	EXPECT_GT(index_of(ev, "destroy"), index_of(ev, "delete"));
	EXPECT_EQ(count_of(ev, "destroy"), 1u);
	S5_EXPECT_STATE(dm, WLS_Released);
}

// R18-002/NEW-038: release requested from a writer-owned thread's callback is queued
// (no self-join); the owner's later release drains the thread, then deletes.
TEST_F(Lifecycle, V1AsyncReleaseFromWriterThreadIsDeferredThenDrained)
{
	DataManager dm;
	Caster caster;
	std::promise<void> requested;
	std::atomic<bool> armed{ false };
	std::atomic<bool> once{ false };
	dm.add_caster(&caster);
	// The fake's worker starts calling the sink inside init; arm the release only after
	// init returned (a release queued during init makes init fail, which is covered by
	// the host contract but is not what this case measures).
	caster._onTick = [&](WTSTickData*) {
		if (armed.load() && !once.exchange(true))
		{
			dm.release();
			requested.set_value();
		}
	};
	ASSERT_TRUE(init(dm, "s5_fake_v1_async"));
	armed = true;
	ASSERT_EQ(requested.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
	S5_EXPECT_STATE(dm, WLS_ReleasePending);
	EXPECT_EQ(index_of(events(), "delete"), -1);

	dm.release();	// owner, safe context
	const auto ev = events();
	S5_EXPECT_STATE(dm, WLS_Released);
	EXPECT_GE(index_of(ev, "worker-exit"), 0);
	EXPECT_GT(index_of(ev, "drained"), index_of(ev, "worker-exit"));
	EXPECT_GT(index_of(ev, "delete"), index_of(ev, "drained"));
	EXPECT_EQ(count_of(ev, "destroy"), 1u);
}

// 9a: a legacy writer released from a safe context is retained with its module.
TEST_F(Lifecycle, LegacyWriterExternalReleaseIsQuarantinedNotUnloaded)
{
	const size_t before = S5_QUARANTINED();
	{
		DataManager dm;
		ASSERT_TRUE(init(dm, "s5_fake_legacy"));
		dm.release();
		S5_EXPECT_STATE(dm, WLS_Quarantined);
		dm.release();	// idempotent
	}
	const auto ev = events();
	EXPECT_EQ(count_of(ev, "release"), 1u);
	EXPECT_EQ(index_of(ev, "delete"), -1);
	EXPECT_EQ(index_of(ev, "destroy"), -1);
	EXPECT_TRUE(moduleStillLoaded("s5_fake_legacy"));
	#if !defined(S5_BASELINE_HOST)
	EXPECT_EQ(S5_QUARANTINED(), before + 1);
#else
	(void)before;
#endif
}

// 9c: a second init is rejected and the original writer keeps working.
TEST_F(Lifecycle, RepeatedInitIsRejectedAndOriginalKept)
{
	DataManager dm;
	ASSERT_TRUE(init(dm, "s5_fake_v1"));
	EXPECT_FALSE(init(dm, "s5_fake_v1"));
	EXPECT_TRUE(dm.writeTick(nullptr, 0));
	dm.release();
	const auto ev = events();
	EXPECT_EQ(count_of(ev, "factory"), 1u);
	EXPECT_EQ(count_of(ev, "destroy"), 1u);
	S5_EXPECT_STATE(dm, WLS_Released);
}

// NEW-038 with the real WtDataWriter (sync): DataManager::release inside the tick
// broadcast. Old host deleted the writer while procTick was active (std::terminate on
// the joinable checker thread); now the release is delivered after writeTick returns.
TEST_F(Lifecycle, RealSyncWriterReleaseInsideTickBroadcastIsDeferred)
{
	DataManager dm;
	Caster caster;
	dm.add_caster(&caster);
	ASSERT_TRUE(init(dm, "WtDataStorage"));
	caster._onTick = [&](WTSTickData*) { dm.release(); };

	WTSTickData* tick = _market.tick(1);
	EXPECT_TRUE(dm.writeTick(tick, 0));
	tick->release();
	EXPECT_EQ(caster._ticks.load(), 1u);
	S5_EXPECT_STATE(dm, WLS_Released);
	tick = _market.tick(2);
	EXPECT_FALSE(dm.writeTick(tick, 0));
	tick->release();
}

// NEW-038/R18-002 with the real async writer: release from the task worker callback must
// not delete the writer on its own worker thread (old: terminate on self-owned thread).
TEST_F(Lifecycle, RealAsyncWriterReleaseOnWorkerThreadDoesNotSelfJoin)
{
	DataManager dm;
	Caster caster;
	std::promise<void> requested;
	std::atomic<bool> once{ false };
	dm.add_caster(&caster);
	ASSERT_TRUE(init(dm, "WtDataStorage", true));
	caster._onTick = [&](WTSTickData*) {
		if (!once.exchange(true))
		{
			dm.release();
			requested.set_value();
		}
	};

	WTSTickData* tick = _market.tick(1);
	EXPECT_TRUE(dm.writeTick(tick, 0));
	tick->release();
	ASSERT_EQ(requested.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
	dm.release();
	S5_EXPECT_STATE(dm, WLS_Released);
}

// R23-001: a sink callback that blocks must not make release unbounded. With
// release_timeout_ms the writer is quarantined (not destroyed) and the blocked call
// later returns safely.
TEST_F(Lifecycle, RealWriterReleaseIsBoundedWhenCallbackBlocks)
{
	DataManager dm;
	Caster caster;
	std::promise<void> entered;
	std::promise<void> gate;
	std::shared_future<void> gateFuture = gate.get_future().share();
	dm.add_caster(&caster);
	ASSERT_TRUE(init(dm, "WtDataStorage", false, 300));
	caster._onQueue = [&](WTSOrdQueData*) {
		entered.set_value();
		gateFuture.wait();
	};

	WTSOrdQueData* queue = _market.queue(1);
	auto producer = std::async(std::launch::async, [&]() { return dm.writeOrderQueue(queue); });
	ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

	const auto started = std::chrono::steady_clock::now();
	auto releasing = std::async(std::launch::async, [&]() { dm.release(); });
	const bool bounded = releasing.wait_for(std::chrono::seconds(4)) == std::future_status::ready;
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	EXPECT_TRUE(bounded) << "release did not return while a callback was blocked";
	gate.set_value();
	EXPECT_TRUE(producer.get());
	releasing.wait();
	queue->release();
	if (bounded)
	{
		EXPECT_LT(elapsed, 4000);
		S5_EXPECT_STATE(dm, WLS_Quarantined);
	}
}

// R23-001: concurrent producers during release. Every write that returned true is
// processed and broadcast before release returns; writes after release are rejected.
TEST_F(Lifecycle, RealAsyncWriterConcurrentProducersDuringRelease)
{
	DataManager dm;
	Caster caster;
	dm.add_caster(&caster);
	ASSERT_TRUE(init(dm, "WtDataStorage", true));

	std::atomic<bool> stop{ false };
	std::atomic<uint32_t> accepted{ 0 };
	std::vector<std::thread> producers;
	for (int t = 0; t < 4; t++)
	{
		producers.emplace_back([&, t]() {
			// Bounded volume: at most 500 writes per producer, paced so that release
			// overlaps active producers without building an unbounded async backlog.
			uint32_t seq = (uint32_t)t * 100000;
			for (int i = 0; i < 500 && !stop.load(); i++)
			{
				WTSTickData* tick = _market.tick(seq++);
				if (dm.writeTick(tick, 0))
					accepted.fetch_add(1);
				tick->release();
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		});
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	dm.release();
	const uint32_t broadcastAtRelease = caster._ticks.load();
	stop = true;
	for (auto& producer : producers)
		producer.join();

	S5_EXPECT_STATE(dm, WLS_Released);
	EXPECT_GT(accepted.load(), 0u);
	EXPECT_EQ(broadcastAtRelease, accepted.load()) << "accepted writes were lost or delivered after release";
	EXPECT_EQ(caster._ticks.load(), accepted.load());
	WTSTickData* late = _market.tick(999999);
	EXPECT_FALSE(dm.writeTick(late, 0));
	late->release();
}

// Destruction of the owner from a safe context completes the lifecycle (V1).
TEST_F(Lifecycle, DestructorReleasesV1WriterOnce)
{
	{
		DataManager dm;
		ASSERT_TRUE(init(dm, "s5_fake_v1"));
	}
	const auto ev = events();
	EXPECT_EQ(count_of(ev, "release"), 1u);
	EXPECT_EQ(count_of(ev, "delete"), 1u);
	EXPECT_EQ(count_of(ev, "destroy"), 1u);
}

// Coordinator follow-up (2026-11-04): the AD writer now exports lifecycle V1, so the
// host drains, releases, deletes and unloads it instead of quarantining it. Red control:
// the same test against the pre-change AD module (legacy) ends quarantined.
TEST_F(Lifecycle, RealAdWriterReleaseIsDrainedNotQuarantined)
{
	const size_t before = S5_QUARANTINED();
	{
		DataManager dm;
		WTSVariant* cfg = config("WtDataStorageAD", true);
		cfg->append("disabletick", true);
		cfg->append("disablemin1", true);
		cfg->append("disablemin5", true);
		cfg->append("disableday", true);
		ASSERT_TRUE(dm.init(cfg, &_bd, nullptr));
		cfg->release();
		dm.release();
		S5_EXPECT_STATE(dm, WLS_Released);
	}
#if !defined(S5_BASELINE_HOST)
	EXPECT_EQ(S5_QUARANTINED(), before) << "AD writer was quarantined instead of drained";
#else
	(void)before;
#endif
}

} // namespace

