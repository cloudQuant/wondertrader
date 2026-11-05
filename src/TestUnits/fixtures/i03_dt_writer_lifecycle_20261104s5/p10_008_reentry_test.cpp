// BUG-P10-008 (card 9d=A): order-queue / order-detail / transaction broadcasts are
// delivered per block in storage order, outside the block mutex. A synchronous
// same-block re-entry from inside the callback appends, returns ("queued") and its
// broadcast is delivered after the current callback returns.
//
// The writer is the real WtDataWriter loaded from $S5_MODULE_DIR/libWtDataStorage.dylib
// through createWriter (no DataManager). Each case must finish well inside the
// per-process hard limit of the runner; the old implementation spins forever on the
// non-recursive block mutex (red control = runner timeout).
#include "s5_common.h"

#include <condition_variable>
#include <dlfcn.h>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>

namespace {

using namespace s5;

class EmptyBaseData final : public IBaseDataMgr
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

class RecordingSink final : public IDataWriterSink
{
public:
	IBaseDataMgr* getBDMgr() override { return &_base; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override {}
	void broadcastOrdQue(WTSOrdQueData* data) override { onEvent(1, data); }
	void broadcastOrdDtl(WTSOrdDtlData* data) override { onEvent(2, data); }
	void broadcastTrans(WTSTransData* data) override { onEvent(3, data); }
	CodeSet* getSessionComms(const char*) override { return &_codes; }
	uint32_t getTradingDate(const char*) override { return 20260105; }
	void outputLog(WTSLogLevel ll, const char* message) override
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (ll >= LL_ERROR)
			_errors.emplace_back(message);
	}

	void onEvent(int stream, WTSObject* data)
	{
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_events.push_back(action_time_of(data, stream));
		}
		if (_hook)
			_hook(stream, data);
	}

	std::vector<uint32_t> events()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _events;
	}

	std::function<void(int, WTSObject*)> _hook;
	std::vector<std::string> _errors;

private:
	EmptyBaseData _base;
	CodeSet _codes;
	std::mutex _mtx;
	std::vector<uint32_t> _events;
};

class P10Reentry : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const std::string module = module_dir() + "libWtDataStorage.dylib";
		_module = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
		ASSERT_NE(_module, nullptr) << dlerror();
		_create = reinterpret_cast<FuncCreateWriter>(dlsym(_module, "createWriter"));
		_delete = reinterpret_cast<FuncDeleteWriter>(dlsym(_module, "deleteWriter"));
		ASSERT_NE(_create, nullptr);
		ASSERT_NE(_delete, nullptr);
		_dir = unique_dir("p10_008");
		_writer = _create();
		ASSERT_NE(_writer, nullptr);
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", (_dir.string() + "/").c_str());
		cfg->append("cache", "cache.dmb");
		cfg->append("async", false);
		cfg->append("groupsize", 1000u);
		ASSERT_TRUE(_writer->init(cfg, &_sink));
		cfg->release();
	}

	void TearDown() override
	{
		if (_writer != nullptr)
		{
			_writer->release();
			_delete(_writer);
		}
		// Keep the module mapped: RecordingSink objects may still be referenced by statics.
	}

	bool write(int stream, uint32_t seq)
	{
		switch (stream)
		{
		case 1: { WTSOrdQueData* d = _market.queue(seq); bool ok = _writer->writeOrderQueue(d); d->release(); return ok; }
		case 2: { WTSOrdDtlData* d = _market.detail(seq); bool ok = _writer->writeOrderDetail(d); d->release(); return ok; }
		case 3: { WTSTransData* d = _market.trans(seq); bool ok = _writer->writeTransaction(d); d->release(); return ok; }
		default: return false;
		}
	}

	fs::path blockFile(int stream) const
	{
		const char* sub = stream == 1 ? "queue" : (stream == 2 ? "orders" : "trans");
		return _dir / "rt" / sub / "SHFE" / "rb2210.dmb";
	}

	void sameBlockReentry(int stream)
	{
		std::atomic<int> nested{ 0 };
		std::atomic<bool> nestedReturned{ false };
		_sink._hook = [&](int s, WTSObject*) {
			if (s == stream && nested.fetch_add(1) == 0)
			{
				// Synchronous same-contract, same-stream re-entry from inside the callback.
				EXPECT_TRUE(write(stream, 2));
				nestedReturned = true;
				// Card 9d: the nested record is queued, not broadcast inside this callback.
				EXPECT_EQ(_sink.events().size(), 1u);
			}
		};
		EXPECT_TRUE(write(stream, 1));
		EXPECT_TRUE(nestedReturned.load());
		EXPECT_EQ(_sink.events(), (std::vector<uint32_t>{ 93000001u, 93000002u }));
		EXPECT_EQ(rt_block_size(blockFile(stream)), 2u);
	}

	Market _market;
	RecordingSink _sink;
	fs::path _dir;
	void* _module = nullptr;
	FuncCreateWriter _create = nullptr;
	FuncDeleteWriter _delete = nullptr;
	IDataWriter* _writer = nullptr;
};

TEST_F(P10Reentry, OrderQueueSameBlockReentryIsQueuedAndOrdered) { sameBlockReentry(1); }
TEST_F(P10Reentry, OrderDetailSameBlockReentryIsQueuedAndOrdered) { sameBlockReentry(2); }
TEST_F(P10Reentry, TransactionSameBlockReentryIsQueuedAndOrdered) { sameBlockReentry(3); }

// While a broadcast for the block is running, another thread can still append to the
// same block (the block mutex is not held by the callback); its event is delivered by
// the running drainer after the current callback, in storage order.
TEST_F(P10Reentry, CallbackDoesNotHoldBlockMutexAndPreservesOrderAcrossThreads)
{
	for (int stream = 1; stream <= 3; stream++)
	{
		std::atomic<int> calls{ 0 };
		std::atomic<bool> otherReturned{ false };
		const uint32_t base = (uint32_t)stream * 100;
		_sink._hook = [&](int s, WTSObject*) {
			if (s != stream || calls.fetch_add(1) != 0)
				return;
			auto other = std::async(std::launch::async, [&]() { return write(stream, base + 2); });
			// Old code: the other thread spins on the block mutex held across this callback.
			const bool finished = other.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
			EXPECT_TRUE(finished) << "stream " << stream << ": concurrent append blocked by callback";
			if (finished)
			{
				EXPECT_TRUE(other.get());
				otherReturned = true;
			}
			else
			{
				other.wait();
			}
		};
		const size_t before = _sink.events().size();
		EXPECT_TRUE(write(stream, base + 1));
		EXPECT_TRUE(otherReturned.load());
		std::vector<uint32_t> all = _sink.events();
		ASSERT_EQ(all.size(), before + 2) << "stream " << stream;
		EXPECT_EQ(all[before], 93000000u + base + 1);
		EXPECT_EQ(all[before + 1], 93000000u + base + 2);
		EXPECT_EQ(rt_block_size(blockFile(stream)), 2u);
	}
}

// A throwing receiver is contained per event: the write still reports the persisted
// record, the failure is logged, and later events of the block are delivered.
TEST_F(P10Reentry, ThrowingBroadcastIsContainedAndLaterEventsDelivered)
{
	for (int stream = 1; stream <= 3; stream++)
	{
		std::atomic<int> calls{ 0 };
		_sink._hook = [&](int s, WTSObject*) {
			if (s == stream && calls.fetch_add(1) == 0)
				throw std::runtime_error("s5 receiver failure");
		};
		bool first = false;
		EXPECT_NO_THROW(first = write(stream, 11));
		EXPECT_TRUE(first);
		EXPECT_TRUE(write(stream, 12));
		EXPECT_EQ(calls.load(), 2) << "stream " << stream;
		EXPECT_EQ(rt_block_size(blockFile(stream)), 2u);
	}
	EXPECT_GE(_sink._errors.size(), 3u);
}

// ---------------------------------------------------------------------------
// I03-NEW-048 (card 9d): a dumper called by the close worker writes the same contract
// synchronously. The old close job held the tick block lease across the dumper call,
// so the nested write spun on the same non-recursive mutex forever (red = runner
// timeout). Now the write is accepted, deferred, and replayed after the contract's
// close job released its leases.
class MarketBaseData final : public IBaseDataMgr
{
public:
	explicit MarketBaseData(const Market& market) : _market(market) {}
	WTSCommodityInfo* getCommodity(const char*) override { return _market.commodity; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _market.commodity; }
	WTSContractInfo* getContract(const char* code, const char*, uint32_t) override
	{
		return (code != nullptr && std::strcmp(code, "rb2210") == 0) ? _market.contract : nullptr;
	}
	WTSArray* getContracts(const char*, uint32_t) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return _market.session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _market.session; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool) override { return 20260105; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
	uint32_t getContractSize(const char*, uint32_t) override { return 0; }
private:
	const Market& _market;
};

class CloseSink final : public IDataWriterSink
{
public:
	explicit CloseSink(IBaseDataMgr* base) : _base(base) { _codes.insert("SHFE.rb"); }
	IBaseDataMgr* getBDMgr() override { return _base; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override
	{
		std::lock_guard<std::mutex> lock(_mtx);
		++_ticks;
		_cond.notify_all();
	}
	void broadcastOrdQue(WTSOrdQueData*) override {}
	void broadcastOrdDtl(WTSOrdDtlData*) override {}
	void broadcastTrans(WTSTransData*) override {}
	CodeSet* getSessionComms(const char*) override { return &_codes; }
	uint32_t getTradingDate(const char*) override { return 20260105; }
	void outputLog(WTSLogLevel ll, const char* message) override
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (message != nullptr)
			_logs.emplace_back(message);
		if (ll >= LL_ERROR && message != nullptr)
			_errors.emplace_back(message);
		_cond.notify_all();
	}
	bool waitTicks(uint32_t n, int seconds)
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cond.wait_for(lock, std::chrono::seconds(seconds), [&] { return _ticks >= n; });
	}
	bool waitLog(const std::string& needle, int seconds)
	{
		std::unique_lock<std::mutex> lock(_mtx);
		return _cond.wait_for(lock, std::chrono::seconds(seconds), [&] {
			for (const auto& line : _logs)
				if (line.find(needle) != std::string::npos)
					return true;
			return false;
		});
	}
	uint32_t ticks() { std::lock_guard<std::mutex> lock(_mtx); return _ticks; }
	std::vector<std::string> _errors;
private:
	IBaseDataMgr* _base;
	CodeSet _codes;
	std::mutex _mtx;
	std::condition_variable _cond;
	uint32_t _ticks = 0;
	std::vector<std::string> _logs;
};

class ReentrantDumper final : public IHisDataDumper
{
public:
	ReentrantDumper(const Market& market) : _market(market) {}
	bool dumpHisBars(const char*, const char*, WTSBarStruct*, uint32_t) override { return true; }
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t count) override
	{
		_tickCount += count;
		if (_writer != nullptr && !_reentered.exchange(true))
		{
			// Same contract, synchronous writer, on the close worker thread.
			WTSTickData* tick = _market.tick(7);
			_nestedResult = _writer->writeTick(tick, 0) ? 1 : 0;
			tick->release();
		}
		return true;
	}
	IDataWriter* _writer = nullptr;
	std::atomic<uint32_t> _tickCount{ 0 };
	std::atomic<bool> _reentered{ false };
	std::atomic<int> _nestedResult{ -1 };
private:
	const Market& _market;
};

class CloseDumpReentry : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const std::string module = module_dir() + "libWtDataStorage.dylib";
		_module = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
		ASSERT_NE(_module, nullptr) << dlerror();
		_create = reinterpret_cast<FuncCreateWriter>(dlsym(_module, "createWriter"));
		_delete = reinterpret_cast<FuncDeleteWriter>(dlsym(_module, "deleteWriter"));
		ASSERT_NE(_create, nullptr);
		ASSERT_NE(_delete, nullptr);
		_dir = unique_dir("close_dump");
	}

	IDataWriter* makeWriter(IDataWriterSink* sink, bool withGroupSize)
	{
		IDataWriter* writer = _create();
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", (_dir.string() + "/").c_str());
		cfg->append("cache", "cache.dmb");
		cfg->append("async", false);
		if (withGroupSize)
			cfg->append("groupsize", 1000u);
		EXPECT_TRUE(writer->init(cfg, sink));
		cfg->release();
		return writer;
	}

	Market _market;
	fs::path _dir;
	void* _module = nullptr;
	FuncCreateWriter _create = nullptr;
	FuncDeleteWriter _delete = nullptr;
};

TEST_F(CloseDumpReentry, DumperSameContractWriteIsDeferredUntilCloseJobReleasesLeases)
{
	MarketBaseData base(_market);
	CloseSink sink(&base);
	IDataWriter* writer = makeWriter(&sink, true);
	ReentrantDumper dumper(_market);
	dumper._writer = writer;

	WTSTickData* tick = _market.tick(1);
	ASSERT_TRUE(writer->writeTick(tick, 0));
	tick->release();
	ASSERT_EQ(sink.ticks(), 1u);
	ASSERT_EQ(rt_block_size(_dir / "rt" / "ticks" / "SHFE" / "rb2210.dmb"), 1u);

	writer->add_ext_dumper("s5", &dumper);
	writer->transHisData("S5DAY");

	// Close job finishes (old code: the close worker spins forever inside the dumper).
	EXPECT_TRUE(sink.waitLog("ClosingTask of rb2210[SHFE] done", 10)) << "close job did not finish";
	// The nested write was accepted and replayed after the job: second broadcast arrives.
	EXPECT_TRUE(sink.waitTicks(2, 10));
	EXPECT_EQ(dumper._tickCount.load(), 1u);
	EXPECT_EQ(dumper._nestedResult.load(), 1);
	EXPECT_TRUE(sink.waitLog("ClosingTask mark of Trading session [S5DAY]", 10));

	writer->release();
	// The dumped record went to the history file; the replayed record is the only live one.
	EXPECT_TRUE(fs::exists(_dir / "his" / "ticks" / "SHFE" / "20260105" / "rb2210.dsb"));
	EXPECT_EQ(rt_block_size(_dir / "rt" / "ticks" / "SHFE" / "rb2210.dmb"), 1u);
	_delete(writer);
}

// I03-NEW-050: a writer configured without "groupsize" must not divide by zero when
// counting received records (run under UBSan for the red control: integer division by
// zero is not trapped on arm64, so the normal build cannot tell the difference).
TEST_F(CloseDumpReentry, MissingGroupSizeDoesNotDivideByZero)
{
	MarketBaseData base(_market);
	CloseSink sink(&base);
	IDataWriter* writer = makeWriter(&sink, false);
	for (uint32_t i = 1; i <= 3; i++)
	{
		WTSTickData* tick = _market.tick(i);
		EXPECT_TRUE(writer->writeTick(tick, 0));
		tick->release();
		WTSOrdQueData* queue = _market.queue(i);
		EXPECT_TRUE(writer->writeOrderQueue(queue));
		queue->release();
		WTSOrdDtlData* detail = _market.detail(i);
		EXPECT_TRUE(writer->writeOrderDetail(detail));
		detail->release();
		WTSTransData* trans = _market.trans(i);
		EXPECT_TRUE(writer->writeTransaction(trans));
		trans->release();
	}
	EXPECT_EQ(sink.ticks(), 3u);
	writer->release();
	_delete(writer);
}

} // namespace

