// p10 / WtDataStorage：WtDataWriter 的实时数据文件块（.dmb / 分钟 .dmb）装配与写入
//
// 覆盖对象（全部是普通文件 + 文件映射，路径都在 temp 目录内；不触碰命名共享内存）：
//  - getTickBlock：新建块头、容量/日期/标志、日期切换清零、文件大小与 capacity 不符的自愈分支
//  - pipeToTicks：写入 RT tick 文件块（写回磁盘后逐字段读回比对）
//  - getKlineBlock：min1/min5 两个分支、按交易分钟数预分配容量、非法周期与 NULL 合约拒绝
//  - getTransBlock / getOrdDtlBlock / getOrdQueBlock：三个逐笔文件块的新建与拒绝分支
//  - releaseBlock / release：块指针回收
//
// 异步交错用例会启动真实 worker/checker 线程；临时文件由 TearDown 清理，不碰 dist/
#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <unistd.h>

#include "p10_writer_prefix.h"   // 第三方头需先于访问宏 include，见该文件注释
#include "p10_writer_test_hooks.h"

#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"

using namespace wtp;

namespace {

static const uint32_t kHftStep = 2500;      // WtDataWriter.cpp: HFT_SIZE_STEP
static const uint32_t kTickDate = 20240108;
static const uint32_t kNextDate = 20240219;

static std::string read_file_bytes(const std::string& path)
{
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (!ifs)
		return std::string();
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

static WTSTickStruct make_tick(uint32_t seq)
{
	WTSTickStruct ts;
	std::memset(&ts, 0, sizeof(ts));
	snprintf(ts.exchg, sizeof(ts.exchg), "%s", "SHFE");
	snprintf(ts.code, sizeof(ts.code), "%s", "rb2210");
	ts.price = 3600.0 + seq;
	ts.open = 3560.0 + seq;
	ts.high = 3620.0 + seq;
	ts.low = 3550.0 + seq;
	ts.settle_price = 3590.0 + seq;
	ts.total_volume = 10000 + seq;
	ts.volume = 20 + seq;
	ts.open_interest = 1234567 + seq;
	ts.pre_interest = 1230000 + seq;
	ts.trading_date = kTickDate;
	ts.action_date = kTickDate;
	ts.action_time = 93000000 + seq * 500;
	return ts;
}

class P10RtBlockTest : public ::testing::Test
{
protected:
	std::filesystem::path _dir;
	std::string _base;
	WTSSessionInfo* _session;
	WTSCommodityInfo* _comm;
	WTSContractInfo* _ct;
	std::vector<WtDataWriter*> _writers;

	void SetUp() override
	{
		static std::atomic<uint32_t> seq(0);
		_dir = std::filesystem::temp_directory_path()
			/ ("p10_rt_" + std::to_string(getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(ec) << ec.message();
		_base = _dir.string() + "/";

		_session = WTSSessionInfo::create("P10_RT_SESS", "p10 实时", 0);
		_session->addTradingSection(900, 1130);
		_session->addTradingSection(1330, 1500);
		_comm = WTSCommodityInfo::create("p10rb", "p10 螺纹钢", "SHFE", "P10_RT_SESS", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
		_ct = WTSContractInfo::create("rb2210", "p10 rb2210", "SHFE", "p10rb");
		_ct->setCommInfo(_comm);
	}

	WtDataWriter* new_writer()
	{
		WtDataWriter* w = new WtDataWriter();
		w->_base_dir = _base;
		w->_save_tick_log = false;
		_writers.push_back(w);
		return w;
	}

	void TearDown() override
	{
		for (size_t i = 0; i < _writers.size(); i++)
		{
			_writers[i]->release();
			delete _writers[i];
		}
		_writers.clear();

		if (_ct != NULL) { _ct->release(); _ct = NULL; }
		if (_comm != NULL) { _comm->release(); _comm = NULL; }
		if (_session != NULL) { _session->release(); _session = NULL; }

		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}
};

class P10AsyncWriterSink : public IDataWriterSink
{
public:
	void arm(WtDataWriter* writer, WTSContractInfo* contract)
	{
		_writer = writer;
		_contract = contract;
	}

	IBaseDataMgr* getBDMgr() override { return nullptr; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override
	{
		// Tick processing has already created tick/min1/min5 pairs when this callback
		// runs. Hold the min5 lease on the actual task worker until the test releases it.
		auto min5 = _writer->getKlineBlock(_contract, KP_Minute5, false);
		std::unique_lock<std::mutex> lock(_mutex);
		if (_expire_tick_min5_on_callback && min5)
		{
			// The callback owns this lease, so the expiry update is serialized with the
			// checker's close attempt while the worker is held at the callback gate.
			min5->_lasttime = 1;
			_tick_min5_marked_expired = min5->_block != nullptr && min5->_file != nullptr;
		}
		_events.emplace_back("tick");
		_tick_min5_lease_acquired = static_cast<bool>(min5);
		if (min5 && min5->_block != nullptr)
		{
			_tick_min5_size = min5->_block->_size;
			if (_tick_min5_size != 0)
			{
				_tick_min5_date = min5->_block->_bars[0].date;
				_tick_min5_close = min5->_block->_bars[0].close;
				_tick_min5_volume = min5->_block->_bars[0].vol;
			}
		}
		_tick_worker_id = std::this_thread::get_id();
		_tick_callback_entered = true;
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _allow_tick_return; });
	}
	void broadcastOrdQue(WTSOrdQueData*) override { recordEvent("queue"); }
	void broadcastOrdDtl(WTSOrdDtlData*) override { recordEvent("detail"); }
	void broadcastTrans(WTSTransData*) override { recordEvent("trans"); }
	CodeSet* getSessionComms(const char*) override { return &_comms; }
	uint32_t getTradingDate(const char*) override { return kTickDate; }
	void outputLog(WTSLogLevel, const char*) override {}

	bool waitForTickCallback(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, timeout, [this]() { return _tick_callback_entered; });
	}

	void allowTickCallbackReturn()
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_allow_tick_return = true;
		}
		_condition.notify_all();
	}

	void expireMin5OnTickCallback()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_expire_tick_min5_on_callback = true;
	}

	std::vector<std::string> events() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _events;
	}

	bool tickMin5LeaseAcquired() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_min5_lease_acquired;
	}

	bool tickMin5MarkedExpired() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_min5_marked_expired;
	}

	uint32_t tickMin5Size() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_min5_size;
	}

	uint32_t tickMin5Date() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_min5_date;
	}

	double tickMin5Close() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_min5_close;
	}

	double tickMin5Volume() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_min5_volume;
	}

	std::thread::id tickWorkerId() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _tick_worker_id;
	}

private:
	void recordEvent(const char* event)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_events.emplace_back(event);
	}

	WtDataWriter* _writer = nullptr;
	WTSContractInfo* _contract = nullptr;
	mutable std::mutex _mutex;
	std::condition_variable _condition;
	std::vector<std::string> _events;
	std::thread::id _tick_worker_id;
	bool _tick_callback_entered = false;
	bool _allow_tick_return = false;
	bool _expire_tick_min5_on_callback = false;
	bool _tick_min5_marked_expired = false;
	bool _tick_min5_lease_acquired = false;
	uint32_t _tick_min5_size = 0;
	uint32_t _tick_min5_date = 0;
	double _tick_min5_close = 0;
	double _tick_min5_volume = 0;
	CodeSet _comms;
};

template<typename Predicate>
static bool wait_for_condition(Predicate predicate, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::yield();
	}
	return predicate();
}

struct P10FirstInsertSync
{
	mutable std::mutex mutex;
	std::condition_variable condition;
	bool insertionPaused = false;
	bool allowInsertion = false;
	bool snapshotLockContended = false;
	bool snapshotOrderDetailsCopied = false;
	bool allowSnapshot = false;
	bool detailCallbackEntered = false;
	bool allowDetailCallback = false;
	long snapshotOwnerCount = 0;
	std::weak_ptr<void> snapshotPairLifetime;
	std::thread::id callbackThread;
	WTSOrdDtlStruct callbackDetail;

	void pauseAfterInsert() noexcept
	{
		try
		{
			std::unique_lock<std::mutex> lock(mutex);
			insertionPaused = true;
			condition.notify_all();
			condition.wait(lock, [this]() { return allowInsertion; });
		}
		catch (...)
		{
			allowAllGates();
		}
	}

	void notifySnapshotLockContention() noexcept
	{
		try
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				snapshotLockContended = true;
			}
			condition.notify_all();
		}
		catch (...)
		{
			allowAllGates();
		}
	}

	void pauseAfterSnapshotCopy(long owners, std::weak_ptr<void> lifetime) noexcept
	{
		try
		{
			std::unique_lock<std::mutex> lock(mutex);
			snapshotOwnerCount = owners;
			snapshotPairLifetime = std::move(lifetime);
			snapshotOrderDetailsCopied = true;
			condition.notify_all();
			condition.wait(lock, [this]() { return allowSnapshot; });
		}
		catch (...)
		{
			allowAllGates();
		}
	}

	void pauseInDetailCallback(const WTSOrdDtlStruct& detail, std::thread::id workerId) noexcept
	{
		try
		{
			std::unique_lock<std::mutex> lock(mutex);
			callbackDetail = detail;
			callbackThread = workerId;
			detailCallbackEntered = true;
			condition.notify_all();
			condition.wait(lock, [this]() { return allowDetailCallback; });
		}
		catch (...)
		{
			allowAllGates();
		}
	}

	bool waitForInsertion(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this]() { return insertionPaused; });
	}

	bool waitForSnapshotLockContention(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this]() { return snapshotLockContended; });
	}

	bool waitForSnapshotCopy(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this]() { return snapshotOrderDetailsCopied; });
	}

	bool waitForSnapshotAndDetailCallback(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [this]()
		{
			return snapshotOrderDetailsCopied && detailCallbackEntered;
		});
	}

	void allowInsertionGate()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			allowInsertion = true;
		}
		condition.notify_all();
	}

	void allowSnapshotGate()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			allowSnapshot = true;
		}
		condition.notify_all();
	}

	void allowDetailCallbackGate()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			allowDetailCallback = true;
		}
		condition.notify_all();
	}

	void allowAllGates()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			allowInsertion = true;
			allowSnapshot = true;
			allowDetailCallback = true;
		}
		condition.notify_all();
	}

	bool insertionGateClosed() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return !allowInsertion;
	}

	long copiedSnapshotOwnerCount() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return snapshotOwnerCount;
	}

	std::weak_ptr<void> copiedSnapshotLifetime() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return snapshotPairLifetime;
	}

	std::thread::id detailCallbackThread() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return callbackThread;
	}

	WTSOrdDtlStruct copiedCallbackDetail() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return callbackDetail;
	}
};

struct P10WriterGateReset
{
	P10FirstInsertSync& sync;
	P10AsyncWriterSink& sink;

	~P10WriterGateReset()
	{
		sync.allowAllGates();
		sink.allowTickCallbackReturn();
	}
};

class P10OrderDetailRaceSink : public IDataWriterSink
{
public:
	explicit P10OrderDetailRaceSink(P10FirstInsertSync& sync) : _sync(sync) {}
	IBaseDataMgr* getBDMgr() override { return nullptr; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override {}
	void broadcastOrdQue(WTSOrdQueData*) override {}
	void broadcastOrdDtl(WTSOrdDtlData* detail) override
	{
		_sync.pauseInDetailCallback(detail->getOrdDtlStruct(), std::this_thread::get_id());
	}
	void broadcastTrans(WTSTransData*) override {}
	CodeSet* getSessionComms(const char*) override { return &_comms; }
	uint32_t getTradingDate(const char*) override { return kTickDate; }
	void outputLog(WTSLogLevel, const char*) override {}

private:
	P10FirstInsertSync& _sync;
	CodeSet _comms;
};

static void p10_after_order_detail_insert(void* context) noexcept
{
	static_cast<P10FirstInsertSync*>(context)->pauseAfterInsert();
}

static void p10_snapshot_map_lock_contended(void* context) noexcept
{
	static_cast<P10FirstInsertSync*>(context)->notifySnapshotLockContention();
}

static void p10_after_order_details_copied(void* context, long ownerCount, std::weak_ptr<void> lifetime) noexcept
{
	static_cast<P10FirstInsertSync*>(context)->pauseAfterSnapshotCopy(ownerCount, std::move(lifetime));
}

TEST_F(P10RtBlockTest, TickBlockCreatesDmbWithV2Header)
{
	WtDataWriter* w = new_writer();
	auto pair = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(pair);
	ASSERT_TRUE(pair->_block != NULL);

	const std::string file = _base + "rt/ticks/SHFE/rb2210.dmb";
	ASSERT_TRUE(std::filesystem::exists(file));

	RTTickBlock* blk = pair->_block;
	EXPECT_EQ(std::memcmp(blk->_blk_flag, BLK_FLAG, sizeof(blk->_blk_flag)), 0);
	EXPECT_EQ(blk->_type, (uint16_t)BT_RT_Ticks);
	EXPECT_EQ(blk->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_EQ(blk->_capacity, kHftStep);
	EXPECT_EQ(blk->_size, 0u);
	EXPECT_EQ(blk->_date, kTickDate);

	// 第二次调用必须命中缓存表，返回同一个块指针（不重复建文件）
	pair.unlock();
	auto again = w->getTickBlock(_ct, kTickDate, true);
	EXPECT_EQ(again.get(), pair.get());
	again.unlock();

	const std::string content = read_file_bytes(file);
	EXPECT_EQ(content.size(), sizeof(RTTickBlock) + (size_t)kHftStep * sizeof(WTSTickStruct));
	BlockHeader hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._type, (uint16_t)BT_RT_Ticks);
	EXPECT_FALSE(hdr.is_compressed());
	EXPECT_FALSE(hdr.is_old_version());
}

TEST_F(P10RtBlockTest, PipeToTicksWritesBlockAndDiskRoundTrip)
{
	WtDataWriter* w = new_writer();
	auto pair = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(pair);

	const uint32_t nt = 5;
	WTSTickStruct sent[nt];
	pair.unlock();
	for (uint32_t i = 0; i < nt; i++)
	{
		sent[i] = make_tick(i);
		WTSTickData* tick = WTSTickData::create(sent[i]);
		ASSERT_TRUE(tick != NULL);
		w->pipeToTicks(_ct, tick);
	}

	auto written = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(written);
	RTTickBlock* blk = written->_block;
	ASSERT_TRUE(blk != NULL);
	EXPECT_EQ(blk->_size, nt);
	for (uint32_t i = 0; i < nt; i++)
	{
		EXPECT_DOUBLE_EQ(blk->_ticks[i].price, 3600.0 + i);
		EXPECT_EQ(blk->_ticks[i].trading_date, kTickDate);
	}

	// 落盘后按文件字节读回，逐字段比对
	written->_file->sync();
	const std::string content = read_file_bytes(_base + "rt/ticks/SHFE/rb2210.dmb");
	ASSERT_GT(content.size(), sizeof(RTTickBlock));
	RTTickBlock disk;
	std::memcpy(&disk, content.data(), sizeof(disk));
	EXPECT_EQ(disk._size, nt);
	EXPECT_EQ(disk._capacity, kHftStep);
	EXPECT_EQ(disk._date, kTickDate);

	const WTSTickStruct* onDisk = (const WTSTickStruct*)(content.data() + sizeof(RTTickBlock));
	for (uint32_t i = 0; i < nt; i++)
	{
		EXPECT_EQ(std::memcmp(&onDisk[i], &sent[i], sizeof(WTSTickStruct)), 0);
		EXPECT_STREQ(onDisk[i].code, "rb2210");
		EXPECT_DOUBLE_EQ(onDisk[i].settle_price, 3590.0 + i);
		EXPECT_EQ(onDisk[i].total_volume, 10000u + i);
		EXPECT_EQ(onDisk[i].action_time, 93000000u + i * 500);
	}
	written.unlock();
}

TEST_F(P10RtBlockTest, TickBlockDateSwitchClearsBlock)
{
	{
		WtDataWriter* w1 = new_writer();
		auto p1 = w1->getTickBlock(_ct, kTickDate, true);
		ASSERT_TRUE(p1);
		WTSTickStruct ts = make_tick(0);
		WTSTickData* tick = WTSTickData::create(ts);
		p1.unlock();
		w1->pipeToTicks(_ct, tick);
		auto p1_after = w1->getTickBlock(_ct, kTickDate, true);
		ASSERT_EQ(p1_after->_block->_size, 1u);
		p1_after->_file->sync();
		p1_after.unlock();
		w1->release();
		delete w1;
		for (size_t i = 0; i < _writers.size(); i++)
		{
			if (_writers[i] == w1)
			{
				_writers.erase(_writers.begin() + i);
				break;
			}
		}
	}

	// 新 writer（块表为空）重新映射同一个 .dmb：日期不同 -> 归零并改写日期
	WtDataWriter* w2 = new_writer();
	auto p2 = w2->getTickBlock(_ct, kNextDate, true);
	ASSERT_TRUE(p2);
	ASSERT_TRUE(p2->_block != NULL);
	EXPECT_EQ(p2->_block->_date, kNextDate);
	EXPECT_EQ(p2->_block->_size, 0u);
	EXPECT_EQ(p2->_block->_capacity, kHftStep);
	EXPECT_EQ(p2->_block->_type, (uint16_t)BT_RT_Ticks);

	p2->_file->sync();
	const std::string content = read_file_bytes(_base + "rt/ticks/SHFE/rb2210.dmb");
	WTSTickStruct first;
	std::memcpy(&first, content.data() + sizeof(RTTickBlock), sizeof(first));
	EXPECT_DOUBLE_EQ(first.price, 0.0);
	EXPECT_EQ(first.trading_date, 0u);
}

TEST_F(P10RtBlockTest, TickBlockCapacityRepairedFromRealFileSize)
{
	std::string file;
	{
		WtDataWriter* w1 = new_writer();
		auto p1 = w1->getTickBlock(_ct, kTickDate, true);
		ASSERT_TRUE(p1);
		p1->_file->sync();
		file = _base + "rt/ticks/SHFE/rb2210.dmb";
		p1.unlock();
		w1->release();
		delete w1;
		_writers.clear();
	}

	// 人为把文件截成「只够 10 条 tick」，模拟 capacity 改小但未扩容的历史文件
	const uint64_t small = (uint64_t)sizeof(RTTickBlock) + 10 * sizeof(WTSTickStruct);
	std::error_code ec;
	std::filesystem::resize_file(file, small, ec);
	ASSERT_FALSE(ec) << ec.message();
	ASSERT_EQ((uint64_t)std::filesystem::file_size(file), small);

	WtDataWriter* w2 = new_writer();
	auto p2 = w2->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(p2);
	ASSERT_TRUE(p2->_block != NULL);
	// 自愈分支：按真实文件大小重算 capacity，size 取 min(真实, 标记)
	EXPECT_EQ(p2->_block->_capacity, 10u);
	EXPECT_EQ(p2->_block->_size, 10u);
	EXPECT_EQ(p2->_block->_date, kTickDate);
}

TEST_F(P10RtBlockTest, KlineBlocksUseTradingMinutesAsCapacity)
{
	WtDataWriter* w = new_writer();
	const uint32_t totalMins = _ct->getCommInfo()->getSessionInfo()->getTradingMins();
	ASSERT_GT(totalMins, 0u);

	auto m1 = w->getKlineBlock(_ct, KP_Minute1, true);
	ASSERT_TRUE(m1);
	ASSERT_TRUE(m1->_block != NULL);
	RTKlineBlock* blk = m1->_block;
	EXPECT_EQ(std::memcmp(blk->_blk_flag, BLK_FLAG, sizeof(blk->_blk_flag)), 0);
	EXPECT_EQ(blk->_type, (uint16_t)BT_RT_Minute1);
	EXPECT_EQ(blk->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_EQ(blk->_capacity, totalMins);
	EXPECT_EQ(blk->_size, 0u);

	auto m5 = w->getKlineBlock(_ct, KP_Minute5, true);
	ASSERT_TRUE(m5);
	ASSERT_TRUE(m5->_block != NULL);
	EXPECT_EQ(m5->_block->_type, (uint16_t)BT_RT_Minute5);
	EXPECT_EQ(m5->_block->_capacity, totalMins / 5);

	// 同一合约同一周期命中缓存表
	m1.unlock();
	auto again = w->getKlineBlock(_ct, KP_Minute1, true);
	EXPECT_EQ(again.get(), m1.get());

	// 文件形态与容量一致
	const std::string f1 = _base + "rt/min1/SHFE/rb2210.dmb";
	const std::string f5 = _base + "rt/min5/SHFE/rb2210.dmb";
	ASSERT_TRUE(std::filesystem::exists(f1));
	ASSERT_TRUE(std::filesystem::exists(f5));
	EXPECT_EQ(std::filesystem::file_size(f1), (uint64_t)(sizeof(RTKlineBlock) + totalMins * sizeof(WTSBarStruct)));
	EXPECT_EQ(std::filesystem::file_size(f5), (uint64_t)(sizeof(RTKlineBlock) + (totalMins / 5) * sizeof(WTSBarStruct)));

	const std::string content = read_file_bytes(f1);
	WTSBarStruct bar;
	std::memcpy(&bar, content.data() + sizeof(RTKlineBlock), sizeof(bar));
	EXPECT_EQ(bar.date, 0u);
	EXPECT_EQ(bar.time, 0u);
	again.unlock();
	m5.unlock();
}

TEST_F(P10RtBlockTest, HftBlocksCreateAndReportTypes)
{
	WtDataWriter* w = new_writer();

	auto trans = w->getTransBlock(_ct, kTickDate, true);
	ASSERT_TRUE(trans);
	ASSERT_TRUE(trans->_block != NULL);
	EXPECT_EQ(std::memcmp(trans->_block->_blk_flag, BLK_FLAG, sizeof(trans->_block->_blk_flag)), 0);
	EXPECT_EQ(trans->_block->_type, (uint16_t)BT_RT_Trnsctn);
	EXPECT_EQ(trans->_block->_capacity, kHftStep);
	EXPECT_EQ(trans->_block->_size, 0u);
	EXPECT_EQ(trans->_block->_date, kTickDate);

	auto que = w->getOrdQueBlock(_ct, kTickDate, true);
	ASSERT_TRUE(que);
	ASSERT_TRUE(que->_block != NULL);
	EXPECT_EQ(que->_block->_type, (uint16_t)BT_RT_OrdQueue);
	EXPECT_EQ(que->_block->_capacity, kHftStep);
	EXPECT_EQ(que->_block->_date, kTickDate);

	auto dtl = w->getOrdDtlBlock(_ct, kTickDate, true);
	ASSERT_TRUE(dtl);
	ASSERT_TRUE(dtl->_block != NULL);
	EXPECT_EQ(dtl->_block->_type, (uint16_t)BT_RT_OrdDetail);
	EXPECT_EQ(dtl->_block->_capacity, kHftStep);
	EXPECT_EQ(dtl->_block->_date, kTickDate);

	// 三个逐笔块都落在 rt/<subdir>/<exchg>/<code>.dmb
	EXPECT_TRUE(std::filesystem::exists(_base + "rt/trans/SHFE/rb2210.dmb"));
	EXPECT_TRUE(std::filesystem::exists(_base + "rt/queue/SHFE/rb2210.dmb"));
	EXPECT_TRUE(std::filesystem::exists(_base + "rt/orders/SHFE/rb2210.dmb"));
}

TEST_F(P10RtBlockTest, NullContractAndNoAutoCreateRejected)
{
	WtDataWriter* w = new_writer();

	// ct == NULL：五个入口都先返回 NULL
	EXPECT_FALSE(w->getTickBlock(NULL, kTickDate, true));
	EXPECT_FALSE(w->getKlineBlock(NULL, KP_Minute1, true));
	EXPECT_FALSE(w->getTransBlock(NULL, kTickDate, true));
	EXPECT_FALSE(w->getOrdQueBlock(NULL, kTickDate, true));
	EXPECT_FALSE(w->getOrdDtlBlock(NULL, kTickDate, true));

	// 非法周期：switch 的 default 分支 -> cache_map 为空 -> NULL
	auto bad = w->getKlineBlock(_ct, (WTSKlinePeriod)99, true);
	EXPECT_FALSE(bad);

	// 文件不存在且 bAutoCreate=false：返回 NULL，且不建目录、不落文件
	WtDataWriter* w2 = new WtDataWriter();
	w2->_base_dir = _dir.string() + "/no_auto/";
	EXPECT_FALSE(w2->getTickBlock(_ct, kTickDate, false));
	EXPECT_FALSE(w2->getKlineBlock(_ct, KP_Minute1, false));
	EXPECT_FALSE(w2->getTransBlock(_ct, kTickDate, false));
	EXPECT_FALSE(w2->getOrdQueBlock(_ct, kTickDate, false));
	EXPECT_FALSE(w2->getOrdDtlBlock(_ct, kTickDate, false));
	EXPECT_FALSE(std::filesystem::exists(_dir.string() + "/no_auto/rt/ticks/SHFE/rb2210.dmb"));
	w2->release();
	delete w2;
}

TEST_F(P10RtBlockTest, ReleaseBlockCancelsCachedPair)
{
	WtDataWriter* w = new_writer();
	auto pair = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(pair);
	ASSERT_TRUE(pair->_block != NULL);

	// 关闭单个映射；同一个缓存 pair 会在下次取用时重开
	w->releaseBlock(pair);
	pair.unlock();

	auto fresh = w->getTickBlock(_ct, kNextDate, true);
	ASSERT_TRUE(fresh);
	ASSERT_TRUE(fresh->_block != NULL);
	EXPECT_EQ(fresh->_block->_date, kNextDate);
	EXPECT_EQ(fresh->_block->_capacity, kHftStep);
	EXPECT_EQ(fresh->_block->_size, 0u);
}

TEST_F(P10RtBlockTest, SixBlockMapsSnapshotCloseReopenAndShutdownAreSynchronized)
{
	WtDataWriter* w = new_writer();
	auto tick = w->getTickBlock(_ct, kTickDate, true);
	auto min1 = w->getKlineBlock(_ct, KP_Minute1, true);
	auto min5 = w->getKlineBlock(_ct, KP_Minute5, true);
	auto trans = w->getTransBlock(_ct, kTickDate, true);
	auto ordDtl = w->getOrdDtlBlock(_ct, kTickDate, true);
	auto ordQue = w->getOrdQueBlock(_ct, kTickDate, true);
	ASSERT_TRUE(tick);
	ASSERT_TRUE(min1);
	ASSERT_TRUE(min5);
	ASSERT_TRUE(trans);
	ASSERT_TRUE(ordDtl);
	ASSERT_TRUE(ordQue);

	WtDataWriter::TickBlockPair* tickPair = tick.get();
	WtDataWriter::KBlockPair* min1Pair = min1.get();
	WtDataWriter::KBlockPair* min5Pair = min5.get();
	WtDataWriter::TransBlockPair* transPair = trans.get();
	WtDataWriter::OrdDtlBlockPair* ordDtlPair = ordDtl.get();
	WtDataWriter::OrdQueBlockPair* ordQuePair = ordQue.get();
	tick->_block->_size = 1;
	min1->_block->_size = 1;
	min5->_block->_size = 1;
	trans->_block->_size = 1;
	ordDtl->_block->_size = 1;
	ordQue->_block->_size = 1;
	tick->_file->sync();
	min1->_file->sync();
	min5->_file->sync();
	trans->_file->sync();
	ordDtl->_file->sync();
	ordQue->_file->sync();
	tick->_lasttime = 1000;
	min1->_lasttime = 1000;
	min5->_lasttime = 1000;
	trans->_lasttime = 1000;
	ordDtl->_lasttime = 1000;
	ordQue->_lasttime = 1000;
	tick.unlock();
	min1.unlock();
	min5.unlock();
	trans.unlock();
	ordDtl.unlock();
	ordQue.unlock();

	const WtDataWriter::RtBlocksSnapshot staleSnapshot = w->snapshotRtBlocks();
	ASSERT_EQ(staleSnapshot.ticks.size(), 1u);
	ASSERT_EQ(staleSnapshot.min1.size(), 1u);
	ASSERT_EQ(staleSnapshot.min5.size(), 1u);
	ASSERT_EQ(staleSnapshot.trans.size(), 1u);
	ASSERT_EQ(staleSnapshot.order_details.size(), 1u);
	ASSERT_EQ(staleSnapshot.order_queues.size(), 1u);

	WTSContractInfo* extra = WTSContractInfo::create("rb3001", "p10 rb3001", "SHFE", "p10rb");
	ASSERT_TRUE(extra != NULL);
	extra->setCommInfo(_comm);

	std::mutex startMutex;
	std::condition_variable startCondition;
	int ready = 0;
	bool start = false;
	auto waitForStart = [&]()
	{
		std::unique_lock<std::mutex> lock(startMutex);
		++ready;
		startCondition.notify_all();
		startCondition.wait(lock, [&]() { return start; });
	};
	std::atomic<bool> snapshotsConsistent(true);
	std::thread mapWriter([&]()
	{
		waitForStart();
		{
			StdUniqueLock mapLock(w->_rt_blocks_map_mtx);
			w->_rt_ticks_blocks.reserve(4096);
			w->_rt_trans_blocks.reserve(4096);
			w->_rt_orddtl_blocks.reserve(4096);
			w->_rt_ordque_blocks.reserve(4096);
			w->_rt_min1_blocks.reserve(4096);
			w->_rt_min5_blocks.reserve(4096);
		}
		auto newTick = w->getTickBlock(extra, kTickDate, true);
		auto newMin1 = w->getKlineBlock(extra, KP_Minute1, true);
		auto newMin5 = w->getKlineBlock(extra, KP_Minute5, true);
		auto newTrans = w->getTransBlock(extra, kTickDate, true);
		auto newOrdDtl = w->getOrdDtlBlock(extra, kTickDate, true);
		auto newOrdQue = w->getOrdQueBlock(extra, kTickDate, true);
		if (!newTick || !newMin1 || !newMin5 || !newTrans || !newOrdDtl || !newOrdQue)
			snapshotsConsistent.store(false);
		else
		{
			newTick->_block->_size = 1;
			newMin1->_block->_size = 1;
			newMin5->_block->_size = 1;
			newTrans->_block->_size = 1;
			newOrdDtl->_block->_size = 1;
			newOrdQue->_block->_size = 1;
			newTick->_file->sync();
			newMin1->_file->sync();
			newMin5->_file->sync();
			newTrans->_file->sync();
			newOrdDtl->_file->sync();
			newOrdQue->_file->sync();
		}
	});
	std::thread snapshotter([&]()
	{
		waitForStart();
		for (uint32_t i = 0; i < 32; ++i)
		{
			const WtDataWriter::RtBlocksSnapshot snapshot = w->snapshotRtBlocks();
			if (snapshot.ticks.empty() || snapshot.trans.empty() || snapshot.order_details.empty()
				|| snapshot.order_queues.empty() || snapshot.min1.empty() || snapshot.min5.empty())
				snapshotsConsistent.store(false);
		}
	});
	std::thread checker([&]()
	{
		waitForStart();
		w->closeExpiredBlocks(staleSnapshot, 2000, 600);
	});
	{
		std::unique_lock<std::mutex> lock(startMutex);
		startCondition.wait(lock, [&]() { return ready == 3; });
		start = true;
	}
	startCondition.notify_all();
	mapWriter.join();
	snapshotter.join();
	checker.join();
	EXPECT_TRUE(snapshotsConsistent.load());
	const WtDataWriter::RtBlocksSnapshot currentSnapshot = w->snapshotRtBlocks();
	EXPECT_EQ(currentSnapshot.ticks.size(), 2u);
	EXPECT_EQ(currentSnapshot.trans.size(), 2u);
	EXPECT_EQ(currentSnapshot.order_details.size(), 2u);
	EXPECT_EQ(currentSnapshot.order_queues.size(), 2u);
	EXPECT_EQ(currentSnapshot.min1.size(), 2u);
	EXPECT_EQ(currentSnapshot.min5.size(), 2u);
	{
		auto newTick = w->getTickBlock(extra, kTickDate, false);
		auto newMin1 = w->getKlineBlock(extra, KP_Minute1, false);
		auto newMin5 = w->getKlineBlock(extra, KP_Minute5, false);
		auto newTrans = w->getTransBlock(extra, kTickDate, false);
		auto newOrdDtl = w->getOrdDtlBlock(extra, kTickDate, false);
		auto newOrdQue = w->getOrdQueBlock(extra, kTickDate, false);
		ASSERT_TRUE(newTick);
		ASSERT_TRUE(newMin1);
		ASSERT_TRUE(newMin5);
		ASSERT_TRUE(newTrans);
		ASSERT_TRUE(newOrdDtl);
		ASSERT_TRUE(newOrdQue);
		EXPECT_EQ(newTick->_block->_size, 1u);
		EXPECT_EQ(newMin1->_block->_size, 1u);
		EXPECT_EQ(newMin5->_block->_size, 1u);
		EXPECT_EQ(newTrans->_block->_size, 1u);
		EXPECT_EQ(newOrdDtl->_block->_size, 1u);
		EXPECT_EQ(newOrdQue->_block->_size, 1u);
	}

	// The checker closes each stale snapshot entry. Concurrent table rehashes do not
	// invalidate those pair pointers; a fresh lookup must reopen the same pair.
	{
		WtDataWriter::BlockLease<WtDataWriter::TickBlockPair> closed(staleSnapshot.ticks[0].second, NULL);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}
	{
		WtDataWriter::BlockLease<WtDataWriter::KBlockPair> closed(staleSnapshot.min1[0].second, NULL);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}
	{
		WtDataWriter::BlockLease<WtDataWriter::KBlockPair> closed(staleSnapshot.min5[0].second, NULL);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}
	{
		WtDataWriter::BlockLease<WtDataWriter::TransBlockPair> closed(staleSnapshot.trans[0].second, NULL);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}
	{
		WtDataWriter::BlockLease<WtDataWriter::OrdDtlBlockPair> closed(staleSnapshot.order_details[0].second, NULL);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}
	{
		WtDataWriter::BlockLease<WtDataWriter::OrdQueBlockPair> closed(staleSnapshot.order_queues[0].second, NULL);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}

	auto tickAgain = w->getTickBlock(_ct, kTickDate, true);
	auto min1Again = w->getKlineBlock(_ct, KP_Minute1, true);
	auto min5Again = w->getKlineBlock(_ct, KP_Minute5, true);
	auto transAgain = w->getTransBlock(_ct, kTickDate, true);
	auto ordDtlAgain = w->getOrdDtlBlock(_ct, kTickDate, true);
	auto ordQueAgain = w->getOrdQueBlock(_ct, kTickDate, true);
	ASSERT_TRUE(tickAgain);
	ASSERT_TRUE(min1Again);
	ASSERT_TRUE(min5Again);
	ASSERT_TRUE(transAgain);
	ASSERT_TRUE(ordDtlAgain);
	ASSERT_TRUE(ordQueAgain);
	EXPECT_EQ(tickAgain.get(), tickPair);
	EXPECT_EQ(min1Again.get(), min1Pair);
	EXPECT_EQ(min5Again.get(), min5Pair);
	EXPECT_EQ(transAgain.get(), transPair);
	EXPECT_EQ(ordDtlAgain.get(), ordDtlPair);
	EXPECT_EQ(ordQueAgain.get(), ordQuePair);
	EXPECT_EQ(tickAgain->_block->_size, 1u);
	EXPECT_EQ(min1Again->_block->_size, 1u);
	EXPECT_EQ(min5Again->_block->_size, 1u);
	EXPECT_EQ(transAgain->_block->_size, 1u);
	EXPECT_EQ(ordDtlAgain->_block->_size, 1u);
	EXPECT_EQ(ordQueAgain->_block->_size, 1u);
	tickAgain.unlock();
	min1Again.unlock();
	min5Again.unlock();
	transAgain.unlock();
	ordDtlAgain.unlock();
	ordQueAgain.unlock();

	extra->release();
	w->release();
	{
		StdUniqueLock mapLock(w->_rt_blocks_map_mtx);
		EXPECT_TRUE(w->_rt_ticks_blocks.empty());
		EXPECT_TRUE(w->_rt_trans_blocks.empty());
		EXPECT_TRUE(w->_rt_orddtl_blocks.empty());
		EXPECT_TRUE(w->_rt_ordque_blocks.empty());
		EXPECT_TRUE(w->_rt_min1_blocks.empty());
		EXPECT_TRUE(w->_rt_min5_blocks.empty());
	}
}

TEST_F(P10RtBlockTest, ExpiredCheckerCloseWaitsForActiveBlockLease)
{
	WtDataWriter* w = new_writer();
	auto initial = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(initial);
	WtDataWriter::TickBlockPair* pair = initial.get();
	initial->_lasttime = 1000;
	initial.unlock();

	const WtDataWriter::RtBlocksSnapshot snapshot = w->snapshotRtBlocks();
	ASSERT_EQ(snapshot.ticks.size(), 1u);
	auto active = w->getTickBlock(_ct, kTickDate, false);
	ASSERT_TRUE(active);
	ASSERT_EQ(active.get(), pair);
	active->_lasttime = 1000;

	std::mutex stateMutex;
	std::condition_variable stateCondition;
	bool attemptingClose = false;
	bool checkerFinished = false;
	std::thread checker([&]()
	{
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			attemptingClose = true;
		}
		stateCondition.notify_all();
		w->closeExpiredBlocks(snapshot, 2000, 600);
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			checkerFinished = true;
		}
		stateCondition.notify_all();
	});

	bool checkerEntered = false;
	bool checkerFinishedWhileLeased = false;
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		checkerEntered = stateCondition.wait_for(lock, std::chrono::seconds(2), [&]() { return attemptingClose; });
		if (checkerEntered)
			checkerFinishedWhileLeased = stateCondition.wait_for(lock, std::chrono::milliseconds(100), [&]() { return checkerFinished; });
	}
	EXPECT_TRUE(checkerEntered);
	EXPECT_FALSE(checkerFinishedWhileLeased);
	EXPECT_NE(active->_block, nullptr);
	EXPECT_NE(active->_file, nullptr);
	active.unlock();
	checker.join();
	EXPECT_TRUE(checkerFinished);
	{
		WtDataWriter::BlockLease<WtDataWriter::TickBlockPair> closed(snapshot.ticks[0].second, nullptr);
		EXPECT_EQ(closed->_block, nullptr);
		EXPECT_EQ(closed->_file, nullptr);
	}
}

TEST_F(P10RtBlockTest, RealCheckerClosesAndReopensAllSixExpiredMappings)
{
	P10FirstInsertSync sync;
	P10AsyncWriterSink sink;
	const size_t cacheBytes = sizeof(RTTickCache) + sizeof(TickCacheItem) * 32;
	std::vector<uint64_t> cacheStorage((cacheBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
	WtDataWriter writer;
	writer._base_dir = _base;
	writer._save_tick_log = false;
	writer._async_proc = true;
	writer._log_group_size = 1000;
	writer.IDataWriter::init(nullptr, &sink);
	writer._tick_cache_block = reinterpret_cast<RTTickCache*>(cacheStorage.data());
	writer._tick_cache_block->_capacity = 32;
	writer._tick_cache_block->_size = 0;
	{
		StdUniqueLock lifecycleLock(writer._accept_mtx);
		writer._lifecycle_state = WtDataWriter::LifecycleState::Ready;
		writer._accepting = true;
	}
	sink.arm(&writer, _ct);
	sink.expireMin5OnTickCallback();
	P10WriterGateReset gateReset{ sync, sink };
	(void)gateReset;

	WTSOrdDtlStruct detailRaw;
	std::snprintf(detailRaw.exchg, sizeof(detailRaw.exchg), "%s", _ct->getExchg());
	std::snprintf(detailRaw.code, sizeof(detailRaw.code), "%s", _ct->getCode());
	detailRaw.trading_date = kTickDate;
	detailRaw.action_date = kTickDate;
	detailRaw.action_time = 93001100;
	detailRaw.index = 4601;
	detailRaw.price = 3511.5;
	detailRaw.volume = 12;
	detailRaw.orderid = 88101;
	WTSOrdDtlData* detail = WTSOrdDtlData::create(detailRaw);
	bool detailAccepted = false;
	if (detail != nullptr)
	{
		detail->setContractInfo(_ct);
		detailAccepted = writer.writeOrderDetail(detail);
		detail->release();
	}

	WTSTransStruct transRaw;
	std::snprintf(transRaw.exchg, sizeof(transRaw.exchg), "%s", _ct->getExchg());
	std::snprintf(transRaw.code, sizeof(transRaw.code), "%s", _ct->getCode());
	transRaw.trading_date = kTickDate;
	transRaw.action_date = kTickDate;
	transRaw.action_time = 93002200;
	transRaw.index = 5602;
	transRaw.price = 3512.5;
	transRaw.volume = 13;
	transRaw.askorder = 902;
	transRaw.bidorder = 901;
	WTSTransData* trans = WTSTransData::create(transRaw);
	bool transAccepted = false;
	if (trans != nullptr)
	{
		trans->setContractInfo(_ct);
		transAccepted = writer.writeTransaction(trans);
		trans->release();
	}

	WTSOrdQueStruct queueRaw;
	std::snprintf(queueRaw.exchg, sizeof(queueRaw.exchg), "%s", _ct->getExchg());
	std::snprintf(queueRaw.code, sizeof(queueRaw.code), "%s", _ct->getCode());
	queueRaw.trading_date = kTickDate;
	queueRaw.action_date = kTickDate;
	queueRaw.action_time = 93003300;
	queueRaw.price = 3513.5;
	queueRaw.order_items = 2;
	queueRaw.qsize = 3;
	queueRaw.volumes[0] = 14;
	queueRaw.volumes[1] = 15;
	WTSOrdQueData* queue = WTSOrdQueData::create(queueRaw);
	bool queueAccepted = false;
	if (queue != nullptr)
	{
		queue->setContractInfo(_ct);
		queueAccepted = writer.writeOrderQueue(queue);
		queue->release();
	}

	WTSTickStruct tickRaw = make_tick(7);
	WTSTickData* tick = WTSTickData::create(tickRaw);
	bool tickAccepted = false;
	if (tick != nullptr)
	{
		tick->setContractInfo(_ct);
		tickAccepted = writer.writeTick(tick, 0);
		tick->release();
	}
	const bool allWritesAccepted = detailAccepted && transAccepted && queueAccepted && tickAccepted;
	const std::vector<std::string> expectedEvents = { "detail", "trans", "queue", "tick" };
	const bool tickCallbackEntered = sink.waitForTickCallback(std::chrono::seconds(3));
	const bool workerHeldMin5Lease = sink.tickMin5LeaseAcquired();
	const bool callbacksInInputOrder = sink.events() == expectedEvents;
	if (!allWritesAccepted || !tickCallbackEntered || !workerHeldMin5Lease)
	{
		// Always open the worker gate before release joins the task thread.
		sync.allowAllGates();
		sink.allowTickCallbackReturn();
		writer.release();
		EXPECT_TRUE(allWritesAccepted);
		EXPECT_TRUE(tickCallbackEntered);
		EXPECT_TRUE(workerHeldMin5Lease);
		return;
	}

	std::array<bool, 6> stalePairs = { false, false, false, false, false, false };
	{
		auto block = writer.getTickBlock(_ct, kTickDate, false);
		if (block) { block->_lasttime = 1; stalePairs[0] = block->_block != nullptr && block->_file != nullptr; }
	}
	{
		auto block = writer.getTransBlock(_ct, kTickDate, false);
		if (block) { block->_lasttime = 1; stalePairs[1] = block->_block != nullptr && block->_file != nullptr; }
	}
	{
		auto block = writer.getOrdDtlBlock(_ct, kTickDate, false);
		if (block) { block->_lasttime = 1; stalePairs[2] = block->_block != nullptr && block->_file != nullptr; }
	}
	{
		auto block = writer.getOrdQueBlock(_ct, kTickDate, false);
		if (block) { block->_lasttime = 1; stalePairs[3] = block->_block != nullptr && block->_file != nullptr; }
	}
	{
		auto block = writer.getKlineBlock(_ct, KP_Minute1, false);
		if (block) { block->_lasttime = 1; stalePairs[4] = block->_block != nullptr && block->_file != nullptr; }
	}
	stalePairs[5] = sink.tickMin5MarkedExpired();

	std::shared_ptr<WtDataWriter::TickBlockPair> tickOwner;
	std::shared_ptr<WtDataWriter::TransBlockPair> transOwner;
	std::shared_ptr<WtDataWriter::OrdDtlBlockPair> detailOwner;
	std::shared_ptr<WtDataWriter::OrdQueBlockPair> queueOwner;
	std::shared_ptr<WtDataWriter::KBlockPair> min1Owner;
	std::shared_ptr<WtDataWriter::KBlockPair> min5Owner;
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		const std::string key = _ct->getFullCode();
		auto tickIt = writer._rt_ticks_blocks.find(key);
		auto transIt = writer._rt_trans_blocks.find(key);
		auto detailIt = writer._rt_orddtl_blocks.find(key);
		auto queueIt = writer._rt_ordque_blocks.find(key);
		auto min1It = writer._rt_min1_blocks.find(key);
		auto min5It = writer._rt_min5_blocks.find(key);
		if (tickIt != writer._rt_ticks_blocks.end()) tickOwner = tickIt->second;
		if (transIt != writer._rt_trans_blocks.end()) transOwner = transIt->second;
		if (detailIt != writer._rt_orddtl_blocks.end()) detailOwner = detailIt->second;
		if (queueIt != writer._rt_ordque_blocks.end()) queueOwner = queueIt->second;
		if (min1It != writer._rt_min1_blocks.end()) min1Owner = min1It->second;
		if (min5It != writer._rt_min5_blocks.end()) min5Owner = min5It->second;
	}
	const bool allOwnersAvailable = tickOwner && transOwner && detailOwner
		&& queueOwner && min1Owner && min5Owner;
	std::weak_ptr<WtDataWriter::TickBlockPair> tickLifetime = tickOwner;
	std::weak_ptr<WtDataWriter::TransBlockPair> transLifetime = transOwner;
	std::weak_ptr<WtDataWriter::OrdDtlBlockPair> detailLifetime = detailOwner;
	std::weak_ptr<WtDataWriter::OrdQueBlockPair> queueLifetime = queueOwner;
	std::weak_ptr<WtDataWriter::KBlockPair> min1Lifetime = min1Owner;
	std::weak_ptr<WtDataWriter::KBlockPair> min5Lifetime = min5Owner;

	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &sync;
	hooks.after_order_details_copied = p10_after_order_details_copied;
	hooks.skip_initial_check_wait = true;
	const bool hooksInstalled = p10_writer_test_hooks::install(&writer, hooks);
	if (!hooksInstalled)
	{
		sync.allowAllGates();
		sink.allowTickCallbackReturn();
		writer.release();
		EXPECT_TRUE(allWritesAccepted);
		EXPECT_TRUE(callbacksInInputOrder);
		EXPECT_TRUE(allOwnersAvailable);
		EXPECT_TRUE(hooksInstalled);
		return;
	}

	bool checkerSpawned = false;
	try
	{
		writer._proc_chk.reset(new StdThread([&writer]() { writer.check_loop(); }));
		checkerSpawned = true;
	}
	catch (...)
	{
	}
	const bool checkerCopiedSnapshot = checkerSpawned && sync.waitForSnapshotCopy(std::chrono::seconds(3));
	const std::weak_ptr<void> snapshotLifetime = sync.copiedSnapshotLifetime();
	const bool snapshotRetainedDetailPair = checkerCopiedSnapshot && !snapshotLifetime.expired();
	// The test seam holds the real checker's map snapshot until the test observes it.
	// Releasing it lets the real check_loop finish copying all six maps and execute
	// its stale-time close path while the task worker still owns the min5 lease.
	sync.allowAllGates();
	const auto isClosed = [&sink](const auto& owner)
	{
		if (!owner)
			return false;
		typename WtDataWriter::BlockLease<typename std::remove_reference<decltype(*owner)>::type> observed(owner, &sink);
		return observed->_block == nullptr && observed->_file == nullptr && observed->_lasttime == 0;
	};
	const bool checkerClosedFirstFive = checkerSpawned && allOwnersAvailable
		&& wait_for_condition([&]()
		{
			return isClosed(tickOwner) && isClosed(transOwner) && isClosed(detailOwner)
				&& isClosed(queueOwner) && isClosed(min1Owner);
		}, std::chrono::seconds(3));
	const bool workerHeldMin5LeaseAfterFirstFiveClosed = checkerClosedFirstFive
		&& sink.tickMin5LeaseAcquired();
	// The real checker has closed the first five stale pairs and now waits for the
	// worker-owned min5 lease. Open the callback gate on every path before joining.
	sink.allowTickCallbackReturn();
	const bool checkerClosedAllSix = checkerSpawned && allOwnersAvailable
		&& wait_for_condition([&]()
		{
			return isClosed(tickOwner) && isClosed(transOwner) && isClosed(detailOwner)
				&& isClosed(queueOwner) && isClosed(min1Owner) && isClosed(min5Owner);
		}, std::chrono::seconds(3));

	const std::array<std::string, 6> filePaths = {
		_base + "rt/ticks/SHFE/rb2210.dmb",
		_base + "rt/trans/SHFE/rb2210.dmb",
		_base + "rt/orders/SHFE/rb2210.dmb",
		_base + "rt/queue/SHFE/rb2210.dmb",
		_base + "rt/min1/SHFE/rb2210.dmb",
		_base + "rt/min5/SHFE/rb2210.dmb"
	};
	std::array<bool, 6> filesRetained = { false, false, false, false, false, false };
	for (size_t i = 0; i < filePaths.size(); ++i)
		filesRetained[i] = std::filesystem::exists(filePaths[i]);
	bool allFilesRetained = true;
	for (bool retained : filesRetained)
		allFilesRetained = allFilesRetained && retained;

	std::vector<std::string> reopenSequence;
	bool tickContentsPreserved = false;
	bool transContentsPreserved = false;
	bool detailContentsPreserved = false;
	bool queueContentsPreserved = false;
	bool min1ContentsPreserved = false;
	bool min5ContentsPreserved = false;
	if (checkerClosedAllSix)
	{
		{
			auto block = writer.getTickBlock(_ct, kTickDate, false);
			if (block && block.get() == tickOwner.get() && block->_block != nullptr && block->_block->_size == 1)
			{
				const WTSTickStruct& written = block->_block->_ticks[0];
				tickContentsPreserved = written.trading_date == tickRaw.trading_date
					&& written.action_time == tickRaw.action_time
					&& written.total_volume == tickRaw.total_volume
					&& written.volume == tickRaw.volume
					&& written.price == tickRaw.price
					&& block->_block->_date == kTickDate;
				if (tickContentsPreserved) reopenSequence.emplace_back("tick");
			}
		}
		{
			auto block = writer.getTransBlock(_ct, kTickDate, false);
			if (block && block.get() == transOwner.get() && block->_block != nullptr && block->_block->_size == 1)
			{
				const WTSTransStruct& written = block->_block->_trans[0];
				transContentsPreserved = written.index == transRaw.index
					&& written.action_time == transRaw.action_time
					&& written.price == transRaw.price
					&& written.volume == transRaw.volume
					&& written.askorder == transRaw.askorder
					&& written.bidorder == transRaw.bidorder
					&& block->_block->_date == kTickDate;
				if (transContentsPreserved) reopenSequence.emplace_back("trans");
			}
		}
		{
			auto block = writer.getOrdDtlBlock(_ct, kTickDate, false);
			if (block && block.get() == detailOwner.get() && block->_block != nullptr && block->_block->_size == 1)
			{
				const WTSOrdDtlStruct& written = block->_block->_details[0];
				detailContentsPreserved = written.index == detailRaw.index
					&& written.action_time == detailRaw.action_time
					&& written.price == detailRaw.price
					&& written.volume == detailRaw.volume
					&& written.orderid == detailRaw.orderid
					&& block->_block->_date == kTickDate;
				if (detailContentsPreserved) reopenSequence.emplace_back("detail");
			}
		}
		{
			auto block = writer.getOrdQueBlock(_ct, kTickDate, false);
			if (block && block.get() == queueOwner.get() && block->_block != nullptr && block->_block->_size == 1)
			{
				const WTSOrdQueStruct& written = block->_block->_queues[0];
				queueContentsPreserved = written.action_time == queueRaw.action_time
					&& written.price == queueRaw.price
					&& written.order_items == queueRaw.order_items
					&& written.qsize == queueRaw.qsize
					&& written.volumes[0] == queueRaw.volumes[0]
					&& written.volumes[1] == queueRaw.volumes[1]
					&& block->_block->_date == kTickDate;
				if (queueContentsPreserved) reopenSequence.emplace_back("queue");
			}
		}
		{
			auto block = writer.getKlineBlock(_ct, KP_Minute1, false);
			if (block && block.get() == min1Owner.get() && block->_block != nullptr && block->_block->_size == 1)
			{
				const WTSBarStruct& written = block->_block->_bars[0];
				min1ContentsPreserved = written.date == tickRaw.trading_date
					&& written.close == tickRaw.price && written.vol == tickRaw.volume;
				if (min1ContentsPreserved) reopenSequence.emplace_back("min1");
			}
		}
		{
			auto block = writer.getKlineBlock(_ct, KP_Minute5, false);
			if (block && block.get() == min5Owner.get() && block->_block != nullptr && block->_block->_size == 1)
			{
				const WTSBarStruct& written = block->_block->_bars[0];
				min5ContentsPreserved = written.date == tickRaw.trading_date
					&& written.close == tickRaw.price && written.vol == tickRaw.volume;
				if (min5ContentsPreserved) reopenSequence.emplace_back("min5");
			}
		}
	}
	const std::vector<std::string> expectedReopenSequence = { "tick", "trans", "detail", "queue", "min1", "min5" };
	const bool reopenedInExpectedOrder = reopenSequence == expectedReopenSequence;
	const bool callbacksRemainInInputOrder = sink.events() == expectedEvents;

	std::array<bool, 6> filesRetainedAfterRelease = { false, false, false, false, false, false };
	tickOwner.reset();
	transOwner.reset();
	detailOwner.reset();
	queueOwner.reset();
	min1Owner.reset();
	min5Owner.reset();

	writer.release();
	p10_writer_test_hooks::uninstall(&writer);
	bool mapsCleared = false;
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		mapsCleared = writer._rt_ticks_blocks.empty() && writer._rt_trans_blocks.empty()
			&& writer._rt_orddtl_blocks.empty() && writer._rt_ordque_blocks.empty()
			&& writer._rt_min1_blocks.empty() && writer._rt_min5_blocks.empty();
	}
	bool allFilesRetainedAfterRelease = true;
	for (size_t i = 0; i < filePaths.size(); ++i)
	{
		filesRetainedAfterRelease[i] = std::filesystem::exists(filePaths[i]);
		allFilesRetainedAfterRelease = allFilesRetainedAfterRelease && filesRetainedAfterRelease[i];
	}

	EXPECT_TRUE(allWritesAccepted);
	EXPECT_TRUE(callbacksInInputOrder);
	EXPECT_TRUE(tickCallbackEntered);
	EXPECT_TRUE(workerHeldMin5Lease);
	EXPECT_TRUE(stalePairs[0]);
	EXPECT_TRUE(stalePairs[1]);
	EXPECT_TRUE(stalePairs[2]);
	EXPECT_TRUE(stalePairs[3]);
	EXPECT_TRUE(stalePairs[4]);
	EXPECT_TRUE(stalePairs[5]);
	EXPECT_TRUE(allOwnersAvailable);
	EXPECT_TRUE(hooksInstalled);
	EXPECT_TRUE(checkerSpawned);
	EXPECT_TRUE(checkerCopiedSnapshot);
	EXPECT_TRUE(snapshotRetainedDetailPair);
	EXPECT_TRUE(checkerClosedFirstFive);
	EXPECT_TRUE(workerHeldMin5LeaseAfterFirstFiveClosed);
	EXPECT_TRUE(checkerClosedAllSix);
	EXPECT_TRUE(allFilesRetained);
	EXPECT_TRUE(tickContentsPreserved);
	EXPECT_TRUE(transContentsPreserved);
	EXPECT_TRUE(detailContentsPreserved);
	EXPECT_TRUE(queueContentsPreserved);
	EXPECT_TRUE(min1ContentsPreserved);
	EXPECT_TRUE(min5ContentsPreserved);
	EXPECT_EQ(reopenSequence, expectedReopenSequence);
	EXPECT_TRUE(reopenedInExpectedOrder);
	EXPECT_TRUE(callbacksRemainInInputOrder);
	EXPECT_TRUE(mapsCleared);
	EXPECT_TRUE(snapshotLifetime.expired());
	EXPECT_TRUE(tickLifetime.expired());
	EXPECT_TRUE(transLifetime.expired());
	EXPECT_TRUE(detailLifetime.expired());
	EXPECT_TRUE(queueLifetime.expired());
	EXPECT_TRUE(min1Lifetime.expired());
	EXPECT_TRUE(min5Lifetime.expired());
	EXPECT_TRUE(allFilesRetainedAfterRelease);
}

TEST_F(P10RtBlockTest, ReleaseJoinsActiveCheckerBeforeClearingBlockMaps)
{
	WtDataWriter* w = new_writer();
	auto initial = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(initial);
	WtDataWriter::TickBlockPair* pair = initial.get();
	initial->_lasttime = 1000;
	initial.unlock();

	const WtDataWriter::RtBlocksSnapshot snapshot = w->snapshotRtBlocks();
	ASSERT_EQ(snapshot.ticks.size(), 1u);
	auto active = w->getTickBlock(_ct, kTickDate, false);
	ASSERT_TRUE(active);
	ASSERT_EQ(active.get(), pair);
	active->_lasttime = 1000;

	std::mutex stateMutex;
	std::condition_variable stateCondition;
	bool checkerReady = false;
	bool allowClose = false;
	bool attemptingClose = false;
	bool checkerFinished = false;
	w->_proc_chk.reset(new StdThread([&]()
	{
		{
			StdUniqueLock checkLock(w->_check_mtx);
			w->_check_worker_id = std::this_thread::get_id();
		}
		{
			std::unique_lock<std::mutex> lock(stateMutex);
			checkerReady = true;
			stateCondition.notify_all();
			stateCondition.wait(lock, [&]() { return allowClose; });
		}
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			attemptingClose = true;
		}
		stateCondition.notify_all();
		w->closeExpiredBlocks(snapshot, 2000, 600);
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			checkerFinished = true;
		}
		stateCondition.notify_all();
		{
			StdUniqueLock checkLock(w->_check_mtx);
			w->_check_worker_id = std::thread::id();
		}
	}));

	bool checkerIsReady = false;
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		checkerIsReady = stateCondition.wait_for(lock, std::chrono::seconds(2), [&]() { return checkerReady; });
	}
	if (!checkerIsReady)
	{
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			allowClose = true;
		}
		stateCondition.notify_all();
		active.unlock();
		w->_proc_chk->join();
		FAIL() << "checker worker did not reach its gate";
		return;
	}

	std::atomic<bool> releaseFinished(false);
	std::thread releaser([&]()
	{
		w->release();
		releaseFinished.store(true);
	});
	auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!w->_terminated.load() && std::chrono::steady_clock::now() < releaseDeadline)
		std::this_thread::yield();
	const bool releaseStarted = w->_terminated.load();
	const bool releaseFinishedBeforeChecker = releaseFinished.load();
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		allowClose = true;
	}
	stateCondition.notify_all();

	bool checkerEnteredClose = false;
	bool checkerFinishedWhileLeased = false;
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		checkerEnteredClose = stateCondition.wait_for(lock, std::chrono::seconds(2), [&]() { return attemptingClose; });
		if (checkerEnteredClose)
			checkerFinishedWhileLeased = stateCondition.wait_for(lock, std::chrono::milliseconds(100), [&]() { return checkerFinished; });
	}
	const bool releaseFinishedWhileLeased = releaseFinished.load();
	bool mapRetainedWhileLeased = false;
	{
		StdUniqueLock mapLock(w->_rt_blocks_map_mtx);
		auto it = w->_rt_ticks_blocks.find(_ct->getFullCode());
		mapRetainedWhileLeased = it != w->_rt_ticks_blocks.end() && it->second.get() == pair;
	}
	EXPECT_TRUE(releaseStarted);
	EXPECT_FALSE(releaseFinishedBeforeChecker);
	EXPECT_TRUE(checkerEnteredClose);
	EXPECT_FALSE(checkerFinishedWhileLeased);
	EXPECT_FALSE(releaseFinishedWhileLeased);
	EXPECT_TRUE(mapRetainedWhileLeased);
	EXPECT_NE(active->_block, nullptr);
	EXPECT_NE(active->_file, nullptr);

	active.unlock();
	{
		std::unique_lock<std::mutex> lock(stateMutex);
		EXPECT_TRUE(stateCondition.wait_for(lock, std::chrono::seconds(2), [&]() { return checkerFinished; }));
	}
	releaser.join();
	EXPECT_TRUE(releaseFinished.load());
	{
		StdUniqueLock mapLock(w->_rt_blocks_map_mtx);
		EXPECT_TRUE(w->_rt_ticks_blocks.empty());
	}
}

TEST_F(P10RtBlockTest, SnapshotOwnsBlockPairAcrossMapClear)
{
	WtDataWriter* w = new_writer();
	WtDataWriter::TickBlockPair* tickPair = nullptr;
	WtDataWriter::KBlockPair* min1Pair = nullptr;
	WtDataWriter::KBlockPair* min5Pair = nullptr;
	WtDataWriter::TransBlockPair* transPair = nullptr;
	WtDataWriter::OrdDtlBlockPair* ordDtlPair = nullptr;
	WtDataWriter::OrdQueBlockPair* ordQuePair = nullptr;
	{
		auto tick = w->getTickBlock(_ct, kTickDate, true);
		auto min1 = w->getKlineBlock(_ct, KP_Minute1, true);
		auto min5 = w->getKlineBlock(_ct, KP_Minute5, true);
		auto trans = w->getTransBlock(_ct, kTickDate, true);
		auto ordDtl = w->getOrdDtlBlock(_ct, kTickDate, true);
		auto ordQue = w->getOrdQueBlock(_ct, kTickDate, true);
		ASSERT_TRUE(tick);
		ASSERT_TRUE(min1);
		ASSERT_TRUE(min5);
		ASSERT_TRUE(trans);
		ASSERT_TRUE(ordDtl);
		ASSERT_TRUE(ordQue);
		tickPair = tick.get();
		min1Pair = min1.get();
		min5Pair = min5.get();
		transPair = trans.get();
		ordDtlPair = ordDtl.get();
		ordQuePair = ordQue.get();
	}

	WtDataWriter::RtBlocksSnapshot snapshot = w->snapshotRtBlocks();
	ASSERT_EQ(snapshot.ticks.size(), 1u);
	ASSERT_EQ(snapshot.min1.size(), 1u);
	ASSERT_EQ(snapshot.min5.size(), 1u);
	ASSERT_EQ(snapshot.trans.size(), 1u);
	ASSERT_EQ(snapshot.order_details.size(), 1u);
	ASSERT_EQ(snapshot.order_queues.size(), 1u);
	EXPECT_EQ(snapshot.ticks[0].second.get(), tickPair);
	EXPECT_EQ(snapshot.min1[0].second.get(), min1Pair);
	EXPECT_EQ(snapshot.min5[0].second.get(), min5Pair);
	EXPECT_EQ(snapshot.trans[0].second.get(), transPair);
	EXPECT_EQ(snapshot.order_details[0].second.get(), ordDtlPair);
	EXPECT_EQ(snapshot.order_queues[0].second.get(), ordQuePair);
	std::weak_ptr<WtDataWriter::TickBlockPair> tickLifetime = snapshot.ticks[0].second;
	std::weak_ptr<WtDataWriter::KBlockPair> min1Lifetime = snapshot.min1[0].second;
	std::weak_ptr<WtDataWriter::KBlockPair> min5Lifetime = snapshot.min5[0].second;
	std::weak_ptr<WtDataWriter::TransBlockPair> transLifetime = snapshot.trans[0].second;
	std::weak_ptr<WtDataWriter::OrdDtlBlockPair> ordDtlLifetime = snapshot.order_details[0].second;
	std::weak_ptr<WtDataWriter::OrdQueBlockPair> ordQueLifetime = snapshot.order_queues[0].second;
	EXPECT_FALSE(tickLifetime.expired());
	EXPECT_FALSE(min1Lifetime.expired());
	EXPECT_FALSE(min5Lifetime.expired());
	EXPECT_FALSE(transLifetime.expired());
	EXPECT_FALSE(ordDtlLifetime.expired());
	EXPECT_FALSE(ordQueLifetime.expired());

	w->release();
	{
		StdUniqueLock mapLock(w->_rt_blocks_map_mtx);
		EXPECT_TRUE(w->_rt_ticks_blocks.empty());
	}
	EXPECT_FALSE(tickLifetime.expired());
	EXPECT_FALSE(min1Lifetime.expired());
	EXPECT_FALSE(min5Lifetime.expired());
	EXPECT_FALSE(transLifetime.expired());
	EXPECT_FALSE(ordDtlLifetime.expired());
	EXPECT_FALSE(ordQueLifetime.expired());
	EXPECT_NE(snapshot.ticks[0].second->_block, nullptr);
	EXPECT_NE(snapshot.ticks[0].second->_file, nullptr);
	EXPECT_NE(snapshot.min1[0].second->_block, nullptr);
	EXPECT_NE(snapshot.min1[0].second->_file, nullptr);
	EXPECT_NE(snapshot.min5[0].second->_block, nullptr);
	EXPECT_NE(snapshot.min5[0].second->_file, nullptr);
	EXPECT_NE(snapshot.trans[0].second->_block, nullptr);
	EXPECT_NE(snapshot.trans[0].second->_file, nullptr);
	EXPECT_NE(snapshot.order_details[0].second->_block, nullptr);
	EXPECT_NE(snapshot.order_details[0].second->_file, nullptr);
	EXPECT_NE(snapshot.order_queues[0].second->_block, nullptr);
	EXPECT_NE(snapshot.order_queues[0].second->_file, nullptr);

	snapshot = WtDataWriter::RtBlocksSnapshot();
	EXPECT_TRUE(tickLifetime.expired());
	EXPECT_TRUE(min1Lifetime.expired());
	EXPECT_TRUE(min5Lifetime.expired());
	EXPECT_TRUE(transLifetime.expired());
	EXPECT_TRUE(ordDtlLifetime.expired());
	EXPECT_TRUE(ordQueLifetime.expired());
}

TEST_F(P10RtBlockTest, AsyncFirstOrderDetailInsertInterleavesWithRealCheckerSnapshot)
{
	P10FirstInsertSync sync;
	P10OrderDetailRaceSink sink(sync);
	WtDataWriter writer;
	writer._base_dir = _base;
	writer._save_tick_log = false;
	writer._async_proc = true;
	writer._log_group_size = 1000;
	writer.IDataWriter::init(nullptr, &sink);
	{
		StdUniqueLock lifecycleLock(writer._accept_mtx);
		writer._lifecycle_state = WtDataWriter::LifecycleState::Ready;
		writer._accepting = true;
	}

	bool mapsInitiallyEmpty = false;
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		mapsInitiallyEmpty = writer._rt_orddtl_blocks.empty();
	}
	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &sync;
	hooks.after_order_detail_insert = p10_after_order_detail_insert;
	hooks.snapshot_map_lock_contended = p10_snapshot_map_lock_contended;
	hooks.after_order_details_copied = p10_after_order_details_copied;
	hooks.skip_initial_check_wait = true;
	const bool hooksInstalled = p10_writer_test_hooks::install(&writer, hooks);
	if (!hooksInstalled)
	{
		writer.release();
		EXPECT_TRUE(mapsInitiallyEmpty);
		EXPECT_TRUE(hooksInstalled);
		return;
	}

	WTSOrdDtlStruct raw;
	std::snprintf(raw.exchg, sizeof(raw.exchg), "%s", _ct->getExchg());
	std::snprintf(raw.code, sizeof(raw.code), "%s", _ct->getCode());
	raw.trading_date = kTickDate;
	raw.action_date = kTickDate;
	raw.action_time = 93123456;
	raw.index = 62017;
	raw.price = 3591.25;
	raw.volume = 17;
	raw.orderid = 882001;
	WTSOrdDtlData* detail = WTSOrdDtlData::create(raw);
	bool writeAccepted = false;
	if (detail != nullptr)
	{
		detail->setContractInfo(_ct);
		writeAccepted = writer.writeOrderDetail(detail);
		detail->release();
	}

	const bool insertionPaused = writeAccepted && sync.waitForInsertion(std::chrono::seconds(3));
	bool checkerSpawned = false;
	if (insertionPaused)
	{
		try
		{
			writer._proc_chk.reset(new StdThread([&writer]() { writer.check_loop(); }));
			checkerSpawned = true;
		}
		catch (...)
		{
		}
	}
	const bool checkerContendedWithFirstInsert = checkerSpawned
		&& sync.waitForSnapshotLockContention(std::chrono::seconds(3));
	// The contention callback is emitted only after the real checker fails its
	// try_lock against the map mutex held by the first async getOrdDtlBlock insert.
	sync.allowInsertionGate();
	const bool snapshotAndWorkerReachedGates = checkerContendedWithFirstInsert
		&& sync.waitForSnapshotAndDetailCallback(std::chrono::seconds(5));
	const long snapshotOwners = sync.copiedSnapshotOwnerCount();
	const std::weak_ptr<void> snapshotLifetime = sync.copiedSnapshotLifetime();
	const WTSOrdDtlStruct callbackDetail = sync.copiedCallbackDetail();
	const std::thread::id callbackThread = sync.detailCallbackThread();
	std::thread::id taskWorkerId;
	std::thread::id checkWorkerId;
	{
		StdUniqueLock taskLock(writer._task_mtx);
		taskWorkerId = writer._task_worker_id;
	}
	{
		StdUniqueLock checkLock(writer._check_mtx);
		checkWorkerId = writer._check_worker_id;
	}
	const bool callbackRanOnTaskWorker = callbackThread != std::thread::id()
		&& callbackThread == taskWorkerId;
	const bool checkerIsRealWorker = checkWorkerId != std::thread::id()
		&& checkWorkerId != callbackThread;
	// This count is sampled from inside the real checker's snapshot after it copied
	// the newly inserted pair. It establishes shared ownership at that point; it is
	// not evidence that the checker actually lost a SpinMutex exchange.
	const bool snapshotRetainedInsertedPair = snapshotAndWorkerReachedGates
		&& snapshotOwners >= 3 && !snapshotLifetime.expired();
	const bool callbackMatchesInput = callbackDetail.index == raw.index
		&& callbackDetail.orderid == raw.orderid
		&& callbackDetail.action_time == raw.action_time
		&& callbackDetail.trading_date == raw.trading_date
		&& callbackDetail.price == raw.price
		&& callbackDetail.volume == raw.volume
		&& std::strcmp(callbackDetail.exchg, raw.exchg) == 0
		&& std::strcmp(callbackDetail.code, raw.code) == 0;

	// Let the checker leave its snapshot gate, then request shutdown while the
	// task worker remains inside the sink callback. Every timeout path opens all
	// gates before joining or releasing the writer.
	sync.allowSnapshotGate();
	std::atomic<bool> releaseDone{false};
	std::thread releaser;
	bool releaseSpawned = false;
	if (snapshotAndWorkerReachedGates)
	{
		try
		{
			releaser = std::thread([&]()
			{
				writer.release();
				releaseDone.store(true);
			});
			releaseSpawned = true;
		}
		catch (...)
		{
		}
	}
	const bool releaseStarted = releaseSpawned
		&& wait_for_condition([&]() { return writer._terminated.load(); }, std::chrono::seconds(2));
	const bool releaseWaitedForWorkerGate = releaseStarted && !releaseDone.load();
	sync.allowAllGates();
	if (releaser.joinable())
		releaser.join();
	else
		writer.release();
	const bool releaseCompleted = releaseDone.load() || !releaseSpawned;
	p10_writer_test_hooks::uninstall(&writer);

	bool mapCleared = false;
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		mapCleared = writer._rt_orddtl_blocks.empty();
	}
	const std::string detailFile = _base + "rt/orders/SHFE/rb2210.dmb";
	const std::string content = read_file_bytes(detailFile);
	RTDayBlockHeader diskHeader;
	WTSOrdDtlStruct diskDetail;
	bool persistedDetailMatches = false;
	if (content.size() >= sizeof(diskHeader) + sizeof(diskDetail))
	{
		std::memcpy(&diskHeader, content.data(), sizeof(diskHeader));
		std::memcpy(&diskDetail, content.data() + sizeof(diskHeader), sizeof(diskDetail));
		persistedDetailMatches = diskHeader._type == BT_RT_OrdDetail
			&& diskHeader._version == BLOCK_VERSION_RAW_V2
			&& diskHeader._size == 1
			&& diskHeader._date == kTickDate
			&& diskDetail.index == raw.index
			&& diskDetail.orderid == raw.orderid
			&& diskDetail.action_time == raw.action_time
			&& diskDetail.trading_date == raw.trading_date
			&& diskDetail.price == raw.price
			&& diskDetail.volume == raw.volume
			&& std::strcmp(diskDetail.exchg, raw.exchg) == 0
			&& std::strcmp(diskDetail.code, raw.code) == 0;
	}

	EXPECT_TRUE(mapsInitiallyEmpty);
	EXPECT_TRUE(writeAccepted);
	EXPECT_TRUE(insertionPaused);
	EXPECT_TRUE(checkerSpawned);
	EXPECT_TRUE(checkerContendedWithFirstInsert);
	EXPECT_TRUE(snapshotAndWorkerReachedGates);
	EXPECT_TRUE(snapshotRetainedInsertedPair);
	EXPECT_TRUE(callbackRanOnTaskWorker);
	EXPECT_TRUE(checkerIsRealWorker);
	EXPECT_TRUE(callbackMatchesInput);
	EXPECT_TRUE(releaseStarted);
	EXPECT_TRUE(releaseWaitedForWorkerGate);
	EXPECT_TRUE(releaseCompleted);
	EXPECT_TRUE(mapCleared);
	EXPECT_TRUE(snapshotLifetime.expired());
	EXPECT_TRUE(persistedDetailMatches);
}

TEST_F(P10RtBlockTest, AsyncWorkerFirstCreatesAllSixBlocksBeforeRealCheckerSnapshot)
{
	const size_t cacheBytes = sizeof(RTTickCache) + sizeof(TickCacheItem) * 32;
	std::vector<uint64_t> cacheStorage((cacheBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
	P10AsyncWriterSink sink;
	WtDataWriter writer;
	writer._base_dir = _base;
	writer._save_tick_log = false;
	writer._async_proc = true;
	writer._log_group_size = 1000;
	writer.IDataWriter::init(nullptr, &sink);
	writer._tick_cache_block = reinterpret_cast<RTTickCache*>(cacheStorage.data());
	writer._tick_cache_block->_capacity = 32;
	writer._tick_cache_block->_size = 0;
	{
		StdUniqueLock lifecycleLock(writer._accept_mtx);
		writer._lifecycle_state = WtDataWriter::LifecycleState::Ready;
		writer._accepting = true;
	}
	sink.arm(&writer, _ct);

	auto mapsAreInitiallyEmpty = [&]()
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		return writer._rt_ticks_blocks.empty()
			&& writer._rt_trans_blocks.empty()
			&& writer._rt_orddtl_blocks.empty()
			&& writer._rt_ordque_blocks.empty()
			&& writer._rt_min1_blocks.empty()
			&& writer._rt_min5_blocks.empty();
	};
	ASSERT_TRUE(mapsAreInitiallyEmpty());

	// Queue one write for each public async API in a known order. The single task
	// worker processes these FIFO; the last tick task creates tick, min1, and min5.
	WTSOrdDtlStruct detailRaw;
	std::snprintf(detailRaw.exchg, sizeof(detailRaw.exchg), "%s", _ct->getExchg());
	std::snprintf(detailRaw.code, sizeof(detailRaw.code), "%s", _ct->getCode());
	detailRaw.trading_date = kTickDate;
	detailRaw.action_date = kTickDate;
	detailRaw.action_time = 93001100;
	detailRaw.index = 4001;
	detailRaw.price = 3511.5;
	detailRaw.volume = 12;
	detailRaw.orderid = 801;
	WTSOrdDtlData* detail = WTSOrdDtlData::create(detailRaw);
	bool detailAccepted = false;
	if (detail != nullptr)
	{
		detail->setContractInfo(_ct);
		detailAccepted = writer.writeOrderDetail(detail);
		detail->release();
	}

	WTSTransStruct transRaw;
	std::snprintf(transRaw.exchg, sizeof(transRaw.exchg), "%s", _ct->getExchg());
	std::snprintf(transRaw.code, sizeof(transRaw.code), "%s", _ct->getCode());
	transRaw.trading_date = kTickDate;
	transRaw.action_date = kTickDate;
	transRaw.action_time = 93002200;
	transRaw.index = 5002;
	transRaw.price = 3512.5;
	transRaw.volume = 13;
	transRaw.askorder = 902;
	transRaw.bidorder = 901;
	WTSTransData* trans = WTSTransData::create(transRaw);
	bool transAccepted = false;
	if (trans != nullptr)
	{
		trans->setContractInfo(_ct);
		transAccepted = writer.writeTransaction(trans);
		trans->release();
	}

	WTSOrdQueStruct queueRaw;
	std::snprintf(queueRaw.exchg, sizeof(queueRaw.exchg), "%s", _ct->getExchg());
	std::snprintf(queueRaw.code, sizeof(queueRaw.code), "%s", _ct->getCode());
	queueRaw.trading_date = kTickDate;
	queueRaw.action_date = kTickDate;
	queueRaw.action_time = 93003300;
	queueRaw.price = 3513.5;
	queueRaw.order_items = 2;
	queueRaw.qsize = 3;
	queueRaw.volumes[0] = 14;
	queueRaw.volumes[1] = 15;
	WTSOrdQueData* queue = WTSOrdQueData::create(queueRaw);
	bool queueAccepted = false;
	if (queue != nullptr)
	{
		queue->setContractInfo(_ct);
		queueAccepted = writer.writeOrderQueue(queue);
		queue->release();
	}

	WTSTickStruct tickRaw = make_tick(7);
	WTSTickData* tick = WTSTickData::create(tickRaw);
	bool tickAccepted = false;
	if (tick != nullptr)
	{
		tick->setContractInfo(_ct);
		tickAccepted = writer.writeTick(tick, 0);
		tick->release();
	}
	const bool allWritesAccepted = detailAccepted && transAccepted && queueAccepted && tickAccepted;
	const bool tickCallbackEntered = sink.waitForTickCallback(std::chrono::seconds(3));
	const bool workerHeldMin5Lease = sink.tickMin5LeaseAcquired();
	if (!allWritesAccepted || !tickCallbackEntered || !workerHeldMin5Lease)
	{
		// The sink callback may be waiting even when another part of setup failed.
		// Always open its gate before release joins the task worker.
		sink.allowTickCallbackReturn();
		writer.release();
		EXPECT_TRUE(allWritesAccepted);
		EXPECT_TRUE(tickCallbackEntered);
		EXPECT_TRUE(workerHeldMin5Lease);
		return;
	}

	std::weak_ptr<WtDataWriter::TickBlockPair> tickLifetime;
	std::weak_ptr<WtDataWriter::TransBlockPair> transLifetime;
	std::weak_ptr<WtDataWriter::OrdDtlBlockPair> detailLifetime;
	std::weak_ptr<WtDataWriter::OrdQueBlockPair> queueLifetime;
	std::weak_ptr<WtDataWriter::KBlockPair> min1Lifetime;
	std::weak_ptr<WtDataWriter::KBlockPair> min5Lifetime;
	bool allSixMapsCreatedByWorker = false;
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		const std::string key = _ct->getFullCode();
		auto tickIt = writer._rt_ticks_blocks.find(key);
		auto transIt = writer._rt_trans_blocks.find(key);
		auto detailIt = writer._rt_orddtl_blocks.find(key);
		auto queueIt = writer._rt_ordque_blocks.find(key);
		auto min1It = writer._rt_min1_blocks.find(key);
		auto min5It = writer._rt_min5_blocks.find(key);
		allSixMapsCreatedByWorker = tickIt != writer._rt_ticks_blocks.end()
			&& transIt != writer._rt_trans_blocks.end()
			&& detailIt != writer._rt_orddtl_blocks.end()
			&& queueIt != writer._rt_ordque_blocks.end()
			&& min1It != writer._rt_min1_blocks.end()
			&& min5It != writer._rt_min5_blocks.end();
		if (allSixMapsCreatedByWorker)
		{
			tickLifetime = tickIt->second;
			transLifetime = transIt->second;
			detailLifetime = detailIt->second;
			queueLifetime = queueIt->second;
			min1Lifetime = min1It->second;
			min5Lifetime = min5It->second;
		}
	}

	bool tickContentsCorrect = false;
	{
		auto block = writer.getTickBlock(_ct, kTickDate, false);
		if (block && block->_block != nullptr && block->_block->_size == 1)
		{
			const WTSTickStruct& written = block->_block->_ticks[0];
			tickContentsCorrect = written.trading_date == tickRaw.trading_date
				&& written.action_time == tickRaw.action_time
				&& written.total_volume == tickRaw.total_volume
				&& written.volume == tickRaw.volume
				&& written.price == tickRaw.price;
		}
	}
	bool transContentsCorrect = false;
	{
		auto block = writer.getTransBlock(_ct, kTickDate, false);
		if (block && block->_block != nullptr && block->_block->_size == 1)
		{
			const WTSTransStruct& written = block->_block->_trans[0];
			transContentsCorrect = written.index == transRaw.index
				&& written.action_time == transRaw.action_time
				&& written.price == transRaw.price
				&& written.volume == transRaw.volume
				&& written.askorder == transRaw.askorder
				&& written.bidorder == transRaw.bidorder;
		}
	}
	bool detailContentsCorrect = false;
	{
		auto block = writer.getOrdDtlBlock(_ct, kTickDate, false);
		if (block && block->_block != nullptr && block->_block->_size == 1)
		{
			const WTSOrdDtlStruct& written = block->_block->_details[0];
			detailContentsCorrect = written.index == detailRaw.index
				&& written.action_time == detailRaw.action_time
				&& written.price == detailRaw.price
				&& written.volume == detailRaw.volume
				&& written.orderid == detailRaw.orderid;
		}
	}
	bool queueContentsCorrect = false;
	{
		auto block = writer.getOrdQueBlock(_ct, kTickDate, false);
		if (block && block->_block != nullptr && block->_block->_size == 1)
		{
			const WTSOrdQueStruct& written = block->_block->_queues[0];
			queueContentsCorrect = written.action_time == queueRaw.action_time
				&& written.price == queueRaw.price
				&& written.order_items == queueRaw.order_items
				&& written.qsize == queueRaw.qsize
				&& written.volumes[0] == queueRaw.volumes[0]
				&& written.volumes[1] == queueRaw.volumes[1];
		}
	}
	bool min1ContentsCorrect = false;
	{
		auto block = writer.getKlineBlock(_ct, KP_Minute1, false);
		if (block && block->_block != nullptr && block->_block->_size == 1)
		{
			const WTSBarStruct& written = block->_block->_bars[0];
			min1ContentsCorrect = written.date == tickRaw.trading_date
				&& written.close == tickRaw.price
				&& written.vol == tickRaw.volume;
		}
	}
	const bool min5ContentsCorrect = sink.tickMin5Size() == 1
		&& sink.tickMin5Date() == tickRaw.trading_date
		&& sink.tickMin5Close() == tickRaw.price
		&& sink.tickMin5Volume() == tickRaw.volume;
	const bool writeEventsInOrder = sink.events()
		== std::vector<std::string>({ "detail", "trans", "queue", "tick" });
	bool callbackRanOnTaskWorker = false;
	{
		StdUniqueLock taskLock(writer._task_mtx);
		callbackRanOnTaskWorker = writer._task_worker_id != std::thread::id()
			&& writer._task_worker_id == sink.tickWorkerId();
	}

	EXPECT_TRUE(allSixMapsCreatedByWorker);
	EXPECT_TRUE(tickContentsCorrect);
	EXPECT_TRUE(transContentsCorrect);
	EXPECT_TRUE(detailContentsCorrect);
	EXPECT_TRUE(queueContentsCorrect);
	EXPECT_TRUE(min1ContentsCorrect);
	EXPECT_TRUE(min5ContentsCorrect);
	EXPECT_TRUE(writeEventsInOrder);
	EXPECT_TRUE(callbackRanOnTaskWorker);

	auto readOwnerCounts = [&]()
	{
		std::array<long, 6> counts = { 0, 0, 0, 0, 0, 0 };
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		const std::string key = _ct->getFullCode();
		auto tickIt = writer._rt_ticks_blocks.find(key);
		auto transIt = writer._rt_trans_blocks.find(key);
		auto detailIt = writer._rt_orddtl_blocks.find(key);
		auto queueIt = writer._rt_ordque_blocks.find(key);
		auto min1It = writer._rt_min1_blocks.find(key);
		auto min5It = writer._rt_min5_blocks.find(key);
		if (tickIt != writer._rt_ticks_blocks.end()) counts[0] = tickIt->second.use_count();
		if (transIt != writer._rt_trans_blocks.end()) counts[1] = transIt->second.use_count();
		if (detailIt != writer._rt_orddtl_blocks.end()) counts[2] = detailIt->second.use_count();
		if (queueIt != writer._rt_ordque_blocks.end()) counts[3] = queueIt->second.use_count();
		if (min1It != writer._rt_min1_blocks.end()) counts[4] = min1It->second.use_count();
		if (min5It != writer._rt_min5_blocks.end()) counts[5] = min5It->second.use_count();
		return counts;
	};
	const std::array<long, 6> baselineOwnerCounts = readOwnerCounts();
	const bool onlyMapAndWorkerOwnPairs = baselineOwnerCounts[0] == 1
		&& baselineOwnerCounts[1] == 1
		&& baselineOwnerCounts[2] == 1
		&& baselineOwnerCounts[3] == 1
		&& baselineOwnerCounts[4] == 1
		&& baselineOwnerCounts[5] == 2;
	EXPECT_TRUE(onlyMapAndWorkerOwnPairs);

	// Start the production checker entry point with a coverage-only first-scan wake;
	// after the synchronized scan, the worker stays at the callback gate at min5.
	p10_writer_test_hooks::Hooks checkerHooks;
	checkerHooks.skip_initial_check_wait = true;
	const bool checkerHooksInstalled = p10_writer_test_hooks::install(&writer, checkerHooks);
	if (!checkerHooksInstalled)
	{
		sink.allowTickCallbackReturn();
		writer.release();
		EXPECT_TRUE(checkerHooksInstalled);
		return;
	}
	bool checkerSpawned = false;
	try
	{
		writer._proc_chk.reset(new StdThread([&writer]() { writer.check_loop(); }));
		checkerSpawned = true;
	}
	catch (...)
	{
		// The common cleanup below opens the sink gate before synchronously releasing.
	}
	const bool checkerReady = checkerSpawned && wait_for_condition([&]()
	{
		StdUniqueLock checkLock(writer._check_mtx);
		return writer._check_worker_id != std::thread::id();
	}, std::chrono::seconds(2));
	bool checkerReachedMin5OwnerStage = false;
	std::array<long, 6> checkerOwnerCounts = { 0, 0, 0, 0, 0, 0 };
	if (checkerReady)
	{
		// At the min5 entry, the earlier five block leases have already returned.
		// Counts 2 there mean map + checker snapshot; min5 >= 4 also includes the
		// worker callback lease and the checker's local lease owner. This observes
		// owner acquisition only; it does not prove the checker executed a failed
		// SpinMutex exchange before being descheduled.
		checkerReachedMin5OwnerStage = wait_for_condition([&]()
		{
			checkerOwnerCounts = readOwnerCounts();
			return checkerOwnerCounts[5] >= baselineOwnerCounts[5] + 2;
		}, std::chrono::seconds(18));
		if (checkerReachedMin5OwnerStage)
			checkerOwnerCounts = readOwnerCounts();
	}
	const bool checkerSnapshotOwnsAllMaps = checkerOwnerCounts[0] == baselineOwnerCounts[0] + 1
		&& checkerOwnerCounts[1] == baselineOwnerCounts[1] + 1
		&& checkerOwnerCounts[2] == baselineOwnerCounts[2] + 1
		&& checkerOwnerCounts[3] == baselineOwnerCounts[3] + 1
		&& checkerOwnerCounts[4] == baselineOwnerCounts[4] + 1
		&& checkerOwnerCounts[5] >= baselineOwnerCounts[5] + 2;

	std::atomic<bool> releaseDone{false};
	std::thread releaser;
	bool releaseStarted = false;
	bool releaseStillPendingAtOwnerStage = false;
	if (checkerReachedMin5OwnerStage)
	{
		try
		{
			releaser = std::thread([&]()
			{
				writer.release();
				releaseDone.store(true);
			});
			releaseStarted = wait_for_condition([&]() { return writer._terminated.load(); }, std::chrono::seconds(2));
			releaseStillPendingAtOwnerStage = releaseStarted && !releaseDone.load();
		}
		catch (...)
		{
			// The common cleanup opens the sink gate, then calls release directly.
		}
	}

	// This also releases the worker on every failed/timeout path before any join.
	sink.allowTickCallbackReturn();
	bool releaseCompleted = false;
	if (releaser.joinable())
	{
		releaser.join();
		releaseCompleted = releaseDone.load();
	}
	else
	{
		writer.release();
		releaseCompleted = true;
	}
	p10_writer_test_hooks::uninstall(&writer);
	bool mapsCleared = false;
	{
		StdUniqueLock mapLock(writer._rt_blocks_map_mtx);
		mapsCleared = writer._rt_ticks_blocks.empty()
			&& writer._rt_trans_blocks.empty()
			&& writer._rt_orddtl_blocks.empty()
			&& writer._rt_ordque_blocks.empty()
			&& writer._rt_min1_blocks.empty()
			&& writer._rt_min5_blocks.empty();
	}

	EXPECT_TRUE(checkerReady);
	EXPECT_TRUE(checkerReachedMin5OwnerStage);
	EXPECT_TRUE(checkerSnapshotOwnsAllMaps);
	EXPECT_TRUE(releaseStarted);
	EXPECT_TRUE(releaseStillPendingAtOwnerStage);
	EXPECT_TRUE(releaseCompleted);
	EXPECT_TRUE(mapsCleared);
	EXPECT_TRUE(tickLifetime.expired());
	EXPECT_TRUE(transLifetime.expired());
	EXPECT_TRUE(detailLifetime.expired());
	EXPECT_TRUE(queueLifetime.expired());
	EXPECT_TRUE(min1Lifetime.expired());
	EXPECT_TRUE(min5Lifetime.expired());
}

}  // namespace
