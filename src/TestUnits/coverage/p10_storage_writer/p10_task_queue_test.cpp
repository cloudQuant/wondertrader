// p10 / WtDataStorage: deterministic task/proc queue wakeup and shutdown tests.
// Workers are started directly where needed so the test can observe the condition
// wait state before publishing the task that must wake it.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "p10_writer_prefix.h"

using namespace wtp;

namespace {

// Explicit test seam: operational tests can exercise public methods without running the
// filesystem-heavy production init path. Production state transitions stay in init/release.
class WriterUnderTest : public WtDataWriter
{
public:
	void activateForTest(IDataWriterSink* sink, bool asyncMode = false)
	{
		StdUniqueLock lock(_accept_mtx);
		_sink = sink;
		_async_proc = asyncMode;
		_lifecycle_state = LifecycleState::Ready;
		_accepting = true;
	}
};

class QueueToken : public WTSObject
{
public:
	explicit QueueToken(std::atomic<uint32_t>& destroyed) : _destroyed(destroyed) {}
	~QueueToken() override { ++_destroyed; }

private:
	std::atomic<uint32_t>& _destroyed;
};

static bool wait_for_task_waiter(WtDataWriter& writer)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline)
	{
		{
			StdUniqueLock lock(writer._task_mtx);
			if (writer._task_waiters != 0)
				return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static bool wait_for_proc_waiter(WtDataWriter& writer)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline)
	{
		{
			StdUniqueLock lock(writer._proc_mtx);
			if (writer._proc_waiters != 0)
				return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static bool wait_for_task_stopping(WtDataWriter& writer)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline)
	{
		{
			StdUniqueLock lock(writer._task_mtx);
			if (writer._task_stopping)
				return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static bool wait_for_admission_closed(WtDataWriter& writer)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline)
	{
		{
			StdUniqueLock lock(writer._accept_mtx);
			if (!writer._accepting)
				return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static bool wait_for_active_operations(WtDataWriter& writer, uint32_t expected)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline)
	{
		{
			StdUniqueLock lock(writer._accept_mtx);
			if (writer._active_operations == expected)
				return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static bool wait_for_closing(WtDataWriter& writer)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline)
	{
		{
			StdUniqueLock lock(writer._accept_mtx);
			if (writer._lifecycle_state == WtDataWriter::LifecycleState::Closing)
				return true;
		}
		std::this_thread::yield();
	}
	return false;
}

static std::string make_init_test_root()
{
	static std::atomic<uint64_t> sequence{0};
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path root = std::filesystem::temp_directory_path() /
		("wtdatawriter_p10_" + std::to_string(stamp) + "_" + std::to_string(sequence++));
	std::filesystem::create_directories(root);
	return root.string() + std::filesystem::path::preferred_separator;
}

class InitTestRoot
{
public:
	InitTestRoot() : _path(make_init_test_root()) {}
	~InitTestRoot()
	{
		std::error_code ec;
		std::filesystem::remove_all(_path, ec);
	}
	const std::string& path() const { return _path; }

private:
	std::string _path;
};

struct InitParamsReleaser
{
	void operator()(WTSVariant* params) const
	{
		if (params != nullptr)
			params->release();
	}
};

using InitParams = std::unique_ptr<WTSVariant, InitParamsReleaser>;

static InitParams make_init_params(const std::string& root, bool asyncMode = true)
{
	WTSVariant* params = WTSVariant::createObject();
	params->append("savelog", false);
	params->append("path", root.c_str());
	params->append("cache", "cache.dmb");
	params->append("async", asyncMode);
	params->append("groupsize", (uint32_t)100);
	params->append("skip_notrade_tick", false);
	params->append("skip_notrade_bar", false);
	params->append("disablehis", false);
	params->append("disabletick", false);
	params->append("disablemin1", false);
	params->append("disablemin5", false);
	params->append("disableday", false);
	params->append("disabletrans", false);
	params->append("disableordque", false);
	params->append("disableorddtl", false);
	params->append("minbar_price_mode", (uint32_t)0);
	return InitParams(params);
}

struct TestMarketData
{
	WTSSessionInfo* session = nullptr;
	WTSCommodityInfo* commodity = nullptr;
	WTSContractInfo* contract = nullptr;

	TestMarketData()
	{
		session = WTSSessionInfo::create("P10_TASK", "p10 task session", 0);
		session->addTradingSection(0, 2359);
		commodity = WTSCommodityInfo::create("rb", "p10 rb", "SHFE", "P10_TASK", "CHN");
		commodity->setSessionInfo(session);
		commodity->setCategory(ContractCategory::CC_Future);
		contract = WTSContractInfo::create("rb2210", "p10 rb2210", "SHFE", "rb");
		contract->setCommInfo(commodity);
	}

	~TestMarketData()
	{
		if (contract != nullptr) contract->release();
		if (commodity != nullptr) commodity->release();
		if (session != nullptr) session->release();
	}

	WTSTickData* make_tick()
	{
		WTSTickStruct raw{};
		std::snprintf(raw.code, sizeof(raw.code), "%s", "rb2210");
		std::snprintf(raw.exchg, sizeof(raw.exchg), "%s", "SHFE");
		raw.trading_date = 20250102;
		raw.action_date = 20250102;
		raw.action_time = 93000000;
		raw.price = 100.0;
		WTSTickData* tick = WTSTickData::create(raw);
		tick->setContractInfo(contract);
		return tick;
	}
};

class WriterSinkBase : public IDataWriterSink
{
public:
	IBaseDataMgr* getBDMgr() override { return nullptr; }
	bool canSessionReceive(const char*) override { return false; }
	void broadcastTick(WTSTickData*) override {}
	void broadcastOrdQue(WTSOrdQueData*) override {}
	void broadcastOrdDtl(WTSOrdDtlData*) override {}
	void broadcastTrans(WTSTransData*) override {}
	CodeSet* getSessionComms(const char*) override { return &_comms; }
	uint32_t getTradingDate(const char*) override { return 20250102; }
	void outputLog(WTSLogLevel, const char*) override {}

private:
	CodeSet _comms;
};

class InitBaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return nullptr; }
	WTSArray* getContracts(const char*, uint32_t) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool) override { return 0; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
	uint32_t getContractSize(const char*, uint32_t) override { return 0; }
};

class CountingInitSink : public WriterSinkBase
{
public:
	explicit CountingInitSink(IBaseDataMgr* manager) : _manager(manager) {}
	IBaseDataMgr* getBDMgr() override
	{
		++_calls;
		return _manager;
	}
	uint32_t calls() const { return _calls.load(); }

private:
	IBaseDataMgr* _manager;
	std::atomic<uint32_t> _calls{0};
};

class BlockingInitSink : public WriterSinkBase
{
public:
	explicit BlockingInitSink(IBaseDataMgr* manager) : _manager(manager) {}

	IBaseDataMgr* getBDMgr() override
	{
		++_calls;
		std::unique_lock<std::mutex> lock(_mtx);
		_entered = true;
		_cond.notify_all();
		_cond.wait(lock, [this]() { return _unblocked; });
		return _manager;
	}

	bool wait_until_entered()
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cond.wait_for(lock, std::chrono::seconds(2), [this]() { return _entered; });
	}
	void unblock()
	{
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_unblocked = true;
		}
		_cond.notify_all();
	}
	uint32_t calls() const { return _calls.load(); }

private:
	IBaseDataMgr* _manager;
	std::mutex _mtx;
	std::condition_variable _cond;
	bool _entered = false;
	bool _unblocked = false;
	std::atomic<uint32_t> _calls{0};
};

class BlockingSink : public WriterSinkBase
{
public:
	explicit BlockingSink(WtDataWriter& writer, bool releaseOnReturn = false)
		: _writer(writer), _release_on_return(releaseOnReturn) {}
	~BlockingSink()
	{
		unblock();
		_writer.release();
	}

	bool canSessionReceive(const char*) override
	{
		std::unique_lock<std::mutex> lock(_mtx);
		++_calls;
		_entered = true;
		_cond.notify_all();
		_cond.wait(lock, [this]() { return _unblocked; });
		lock.unlock();
		if (_release_on_return)
			_writer.release();
		return false;
	}

	bool wait_until_entered()
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cond.wait_for(lock, std::chrono::seconds(2), [this]() { return _entered; });
	}

	void unblock()
	{
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_unblocked = true;
		}
		_cond.notify_all();
	}

	uint32_t call_count()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _calls;
	}

private:
	WtDataWriter& _writer;
	bool _release_on_return;
	std::mutex _mtx;
	std::condition_variable _cond;
	bool _entered = false;
	bool _unblocked = false;
	uint32_t _calls = 0;
};

class BlockingSessionSink : public WriterSinkBase
{
public:
	explicit BlockingSessionSink(WtDataWriter& writer) : _writer(writer) {}
	~BlockingSessionSink()
	{
		unblock();
		_writer.release();
	}

	CodeSet* getSessionComms(const char*) override
	{
		std::unique_lock<std::mutex> lock(_mtx);
		++_calls;
		_entered = true;
		_cond.notify_all();
		_cond.wait(lock, [this]() { return _unblocked; });
		lock.unlock();
		_writer.release(); // Reentrant operation release must defer to the outer lease.
		return nullptr;
	}

	bool wait_until_entered()
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cond.wait_for(lock, std::chrono::seconds(2), [this]() { return _entered; });
	}

	void unblock()
	{
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_unblocked = true;
		}
		_cond.notify_all();
	}

	uint32_t call_count()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _calls;
	}

private:
	WtDataWriter& _writer;
	std::mutex _mtx;
	std::condition_variable _cond;
	bool _entered = false;
	bool _unblocked = false;
	uint32_t _calls = 0;
};

class ReentrantSink : public WriterSinkBase
{
public:
	ReentrantSink(WtDataWriter& writer, WTSTickData* nested)
		: _writer(writer), _nested(nested) {}
	~ReentrantSink() { _writer.release(); }

	bool canSessionReceive(const char*) override
	{
		uint32_t call = 0;
		{
			std::lock_guard<std::mutex> lock(_mtx);
			call = ++_calls;
		}
		_cond.notify_all();

		if (call == 1)
		{
			_nested_accepted.store(_writer.writeTick(_nested, 0));
			_writer.release(); // Must request stop without joining this worker to itself.
		}
		return false;
	}

	bool wait_for_two_calls()
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cond.wait_for(lock, std::chrono::seconds(2), [this]() { return _calls >= 2; });
	}

	bool nested_accepted() const { return _nested_accepted.load(); }

private:
	WtDataWriter& _writer;
	WTSTickData* _nested;
	std::mutex _mtx;
	std::condition_variable _cond;
	uint32_t _calls = 0;
	std::atomic<bool> _nested_accepted{false};
};

TEST(P10TaskQueueTest, PublicOperationsAreClosedBeforeInitialization)
{
	WriterUnderTest writer;
	EXPECT_FALSE(writer.writeTick(nullptr, 0));
	EXPECT_FALSE(writer.writeOrderQueue(nullptr));
	EXPECT_FALSE(writer.writeOrderDetail(nullptr));
	EXPECT_FALSE(writer.writeTransaction(nullptr));
	EXPECT_FALSE(writer.isSessionProceeded("P10_TASK"));
	EXPECT_EQ(writer.getCurTick(nullptr), nullptr);
	writer.transHisData("P10_TASK"); // Must return before dereferencing an uninitialized sink.
	writer.release();
}

TEST(P10TaskQueueTest, RealInitializationRejectsConcurrentAndRepeatedInitialization)
{
	InitBaseDataMgr manager;
	BlockingInitSink firstSink(&manager);
	CountingInitSink rejectedSink(&manager);
	InitTestRoot firstRoot;
	InitTestRoot rejectedRoot;
	InitParams firstParams = make_init_params(firstRoot.path());
	InitParams rejectedParams = make_init_params(rejectedRoot.path());
	WriterUnderTest writer;
	std::atomic<bool> firstInitResult{false};
	std::thread initializer([&]() {
		firstInitResult.store(writer.init(firstParams.get(), &firstSink));
	});
	if (!firstSink.wait_until_entered())
	{
		firstSink.unblock();
		initializer.join();
		writer.release();
		FAIL() << "real initialization did not enter the blocking sink";
		return;
	}

	EXPECT_FALSE(writer.init(rejectedParams.get(), &rejectedSink));
	EXPECT_EQ(firstSink.calls(), 1u);
	EXPECT_EQ(rejectedSink.calls(), 0u);
	firstSink.unblock();
	initializer.join();
	EXPECT_TRUE(firstInitResult.load());
	EXPECT_NE(writer._proc_chk, nullptr);
	if (writer._proc_chk != nullptr)
		EXPECT_TRUE(writer._proc_chk->joinable());

	EXPECT_FALSE(writer.init(rejectedParams.get(), &rejectedSink));
	EXPECT_EQ(rejectedSink.calls(), 0u);
	writer.release();
	EXPECT_NE(writer._proc_chk, nullptr);
	if (writer._proc_chk != nullptr)
		EXPECT_FALSE(writer._proc_chk->joinable());
}

TEST(P10TaskQueueTest, RealInitializationAndReleaseRaceJoinsCreatedChecker)
{
	InitBaseDataMgr manager;
	BlockingInitSink sink(&manager);
	InitTestRoot root;
	InitParams params = make_init_params(root.path());
	WriterUnderTest writer;
	std::atomic<bool> initResult{true};
	std::atomic<bool> releaseReturned{false};
	std::thread initializer([&]() {
		initResult.store(writer.init(params.get(), &sink));
	});
	if (!sink.wait_until_entered())
	{
		sink.unblock();
		initializer.join();
		writer.release();
		FAIL() << "real initialization did not enter the blocking sink";
		return;
	}

	std::thread releaser([&]() {
		writer.release();
		releaseReturned.store(true);
	});
	if (!wait_for_closing(writer))
	{
		sink.unblock();
		initializer.join();
		releaser.join();
		FAIL() << "release did not close admission during real initialization";
		return;
	}

	EXPECT_TRUE(wait_for_active_operations(writer, 1));
	EXPECT_FALSE(releaseReturned.load());
	EXPECT_FALSE(writer.init(params.get(), &sink));
	EXPECT_EQ(sink.calls(), 1u);
	sink.unblock();
	initializer.join();
	releaser.join();
	EXPECT_FALSE(initResult.load());
	EXPECT_TRUE(releaseReturned.load());
	EXPECT_NE(writer._proc_chk, nullptr);
	if (writer._proc_chk != nullptr)
		EXPECT_FALSE(writer._proc_chk->joinable());
	EXPECT_TRUE(writer._released);
	writer.release(); // Repeated release after the real init/release race is safe.
}

TEST(P10TaskQueueTest, FailedRealInitializationCanBeReleasedRepeatedly)
{
	CountingInitSink sink(nullptr);
	InitTestRoot root;
	InitParams params = make_init_params(root.path());
	WriterUnderTest writer;

	EXPECT_FALSE(writer.init(params.get(), &sink));
	EXPECT_EQ(sink.calls(), 1u);
	EXPECT_EQ(writer._lifecycle_state, WtDataWriter::LifecycleState::Failed);
	EXPECT_FALSE(writer.writeTick(nullptr, 0));
	EXPECT_EQ(writer._proc_chk, nullptr);
	writer.release();
	writer.release();
	EXPECT_TRUE(writer._released);
	EXPECT_FALSE(writer.init(params.get(), &sink));
	EXPECT_EQ(sink.calls(), 1u);
}

TEST(P10TaskQueueTest, EnqueueAfterWorkerIsWaitingWakesItAndReleasesOwnership)
{
	WriterUnderTest writer;
	writer.activateForTest(nullptr, true);
	{
		StdUniqueLock lock(writer._task_mtx);
		ASSERT_TRUE(writer.ensureTaskThreadLocked());
	}
	ASSERT_TRUE(wait_for_task_waiter(writer));

	std::atomic<uint32_t> destroyed{0};
	QueueToken* token = new QueueToken(destroyed);
	EXPECT_TRUE(writer.pushTask(WtDataWriter::TaskInfo(token, 99)));
	token->release();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (destroyed.load() == 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	EXPECT_EQ(destroyed.load(), 1u);
	writer.release();
}

TEST(P10TaskQueueTest, ProcQueueUsesPredicateWaitAndDoesNotLoseNotification)
{
	WriterUnderTest writer;
	std::string task;
	bool received = false;
	std::thread consumer([&]() { received = writer.takeProcTask(task); });

	if (!wait_for_proc_waiter(writer))
	{
		{
			StdUniqueLock lock(writer._proc_mtx);
			writer._proc_stopping = true;
		}
		writer._proc_cond.notify_all();
		consumer.join();
		FAIL() << "proc worker did not reach its condition wait";
		return;
	}

	{
		StdUniqueLock lock(writer._proc_mtx);
		writer._proc_que.emplace("wake-marker");
		writer._proc_cond.notify_one();
	}
	consumer.join();
	EXPECT_TRUE(received);
	EXPECT_EQ(task, "wake-marker");

	// Stop with work already queued: the consumer must take it before exiting.
	{
		StdUniqueLock lock(writer._proc_mtx);
		writer._proc_stopping = true;
		writer._proc_que.emplace("drain-marker");
	}
	EXPECT_TRUE(writer.takeProcTask(task));
	EXPECT_EQ(task, "drain-marker");
	EXPECT_FALSE(writer.takeProcTask(task));
	writer.release();
}

TEST(P10TaskQueueTest, ReleaseStopsIdleWorkersIdempotentlyAndRejectsProducers)
{
	WriterUnderTest writer;
	writer.activateForTest(nullptr, true);
	{
		StdUniqueLock lock(writer._task_mtx);
		ASSERT_TRUE(writer.ensureTaskThreadLocked());
	}
	{
		StdUniqueLock lock(writer._proc_mtx);
		ASSERT_TRUE(writer.ensureProcThreadLocked());
	}
	ASSERT_TRUE(wait_for_task_waiter(writer));
	ASSERT_TRUE(wait_for_proc_waiter(writer));

	writer.release();
	EXPECT_FALSE(writer._task_thrd->joinable());
	EXPECT_FALSE(writer._proc_thrd->joinable());
	writer.release();

	std::atomic<uint32_t> destroyed{0};
	QueueToken* rejected = new QueueToken(destroyed);
	EXPECT_FALSE(writer.pushTask(WtDataWriter::TaskInfo(rejected, 99)));
	rejected->release();
	EXPECT_EQ(destroyed.load(), 1u);

	TestMarketData market;
	WTSTickData* rejectedTick = market.make_tick();
	EXPECT_FALSE(writer.writeTick(rejectedTick, 0));
	rejectedTick->release();
}

TEST(P10TaskQueueTest, ReleaseDrainsAcceptedTasksAndRejectsConcurrentProducer)
{
	TestMarketData market;
	WriterUnderTest writer;
	BlockingSink sink(writer);
	writer.activateForTest(&sink, true);

	WTSTickData* tick = market.make_tick();
	if (!writer.writeTick(tick, 0))
	{
		tick->release();
		ADD_FAILURE() << "initial async write was rejected";
		return;
	}
	if (!sink.wait_until_entered())
	{
		sink.unblock();
		writer.release();
		tick->release();
		FAIL() << "tick callback did not start";
		return;
	}

	std::atomic<uint32_t> drained{0};
	QueueToken* accepted = new QueueToken(drained);
	EXPECT_TRUE(writer.pushTask(WtDataWriter::TaskInfo(accepted, 99)));
	accepted->release();

	std::thread releaser([&]() { writer.release(); });
	if (!wait_for_task_stopping(writer))
	{
		sink.unblock();
		releaser.join();
		tick->release();
		FAIL() << "release did not close task admission";
		return;
	}

	std::atomic<uint32_t> rejectedDestroyed{0};
	QueueToken* rejected = new QueueToken(rejectedDestroyed);
	EXPECT_FALSE(writer.pushTask(WtDataWriter::TaskInfo(rejected, 99)));
	rejected->release();
	sink.unblock();
	releaser.join();
	tick->release();

	EXPECT_EQ(drained.load(), 1u);
	EXPECT_EQ(rejectedDestroyed.load(), 1u);
}

TEST(P10TaskQueueTest, ReleaseWaitsForSynchronousWriteLeaseAndRejectsLaterCalls)
{
	TestMarketData market;
	WriterUnderTest writer;
	BlockingSink sink(writer, true);
	writer.activateForTest(&sink);

	WTSTickData* tick = market.make_tick();
	std::atomic<bool> writeReturned{false};
	std::thread producer([&]() {
		writer.writeTick(tick, 0);
		writeReturned.store(true);
	});
	if (!sink.wait_until_entered())
	{
		sink.unblock();
		producer.join();
		tick->release();
		FAIL() << "synchronous write did not enter sink callback";
		return;
	}

	std::atomic<bool> releaseReturned{false};
	std::thread releaser([&]() {
		writer.release();
		releaseReturned.store(true);
	});
	if (!wait_for_admission_closed(writer))
	{
		sink.unblock();
		producer.join();
		releaser.join();
		tick->release();
		FAIL() << "release did not close public operation admission";
		return;
	}

	WTSTickData* rejected = market.make_tick();
	EXPECT_FALSE(writer.writeTick(rejected, 0));
	rejected->release();
	EXPECT_EQ(sink.call_count(), 1u);
	EXPECT_FALSE(writeReturned.load());
	EXPECT_FALSE(releaseReturned.load());

	sink.unblock(); // Callback re-enters release; the operation lease must finish first.
	producer.join();
	releaser.join();
	tick->release();
	EXPECT_TRUE(writeReturned.load());
	EXPECT_TRUE(releaseReturned.load());
}

TEST(P10TaskQueueTest, PreCloseWriteLeaseMayEnqueueAfterReleaseClosesAdmission)
{
	TestMarketData market;
	WriterUnderTest writer;
	BlockingSink sink(writer);
	writer.activateForTest(&sink, true);

	WTSTickData* tick = market.make_tick();
	std::atomic<bool> writeAccepted{false};
	StdUniqueLock queueLock(writer._task_mtx);
	std::thread producer([&]() { writeAccepted.store(writer.writeTick(tick, 0)); });
	if (!wait_for_active_operations(writer, 1))
	{
		sink.unblock();
		queueLock.unlock();
		producer.join();
		writer.release();
		tick->release();
		FAIL() << "producer did not obtain its operation lease before queueing";
		return;
	}

	std::atomic<bool> releaseReturned{false};
	std::thread releaser([&]() {
		writer.release();
		releaseReturned.store(true);
	});
	if (!wait_for_admission_closed(writer))
	{
		sink.unblock();
		queueLock.unlock();
		producer.join();
		releaser.join();
		tick->release();
		FAIL() << "release did not close admission before the producer enqueued";
		return;
	}

	EXPECT_FALSE(writer._task_stopping); // The outstanding lease keeps the queue open.
	queueLock.unlock();
	if (!sink.wait_until_entered())
	{
		sink.unblock();
		producer.join();
		releaser.join();
		tick->release();
		FAIL() << "pre-close write was not drained after queue admission closed";
		return;
	}
	EXPECT_TRUE(writeAccepted.load());
	EXPECT_FALSE(releaseReturned.load());
	sink.unblock();
	producer.join();
	releaser.join();
	tick->release();
	EXPECT_TRUE(releaseReturned.load());
}

TEST(P10TaskQueueTest, TransHisDataLeaseStartsBeforeSinkAccessAndDefersReentrantRelease)
{
	WriterUnderTest writer;
	BlockingSessionSink sink(writer);
	writer.activateForTest(&sink);

	std::thread producer([&]() { writer.transHisData("P10_TASK"); });
	if (!sink.wait_until_entered())
	{
		sink.unblock();
		producer.join();
		FAIL() << "transHisData did not enter sink callback";
		return;
	}

	std::atomic<bool> releaseReturned{false};
	std::thread releaser([&]() {
		writer.release();
		releaseReturned.store(true);
	});
	if (!wait_for_admission_closed(writer))
	{
		sink.unblock();
		producer.join();
		releaser.join();
		FAIL() << "release did not close admission during transHisData";
		return;
	}

	writer.transHisData("P10_TASK");
	EXPECT_EQ(sink.call_count(), 1u);
	EXPECT_FALSE(releaseReturned.load());

	sink.unblock();	// The sink re-enters release while the transHisData lease remains active.
	producer.join();
	releaser.join();
	EXPECT_TRUE(releaseReturned.load());
}

TEST(P10TaskQueueTest, CallbackCanReenterWriterAndRequestShutdownWithoutSelfJoin)
{
	TestMarketData market;
	WriterUnderTest writer;
	WTSTickData* first = market.make_tick();
	WTSTickData* nested = market.make_tick();
	ReentrantSink sink(writer, nested);
	writer.activateForTest(&sink, true);

	EXPECT_TRUE(writer.writeTick(first, 0));
	first->release();
	if (!sink.wait_for_two_calls())
	{
		writer.release();
		nested->release();
		FAIL() << "reentrant task was not drained after self-release";
		return;
	}

	EXPECT_TRUE(sink.nested_accepted());
	writer.release(); // External join completes deferred cleanup.
	nested->release();
}

} // namespace
