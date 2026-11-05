/*
 * 迭代03 K51（BUG-R18-011 子项，用户裁定 2026-11-06）：ParserShm 读端落后超过队列容量时，
 * 检测落后、丢弃最旧数据并告警，不再交付被覆盖的格子。
 *
 * 合同：
 *  - 读下一格之前核对写端位置；若写端已经覆盖（或正在覆盖）该格，跳到最早未被覆盖的位置，
 *    打一条 WARN（含本次丢弃条数），累计丢弃数。
 *  - 读完一格后再次核对写端位置；若读的过程中该格被覆盖，丢弃这次读取（计入丢弃），不交付。
 *  - 交付的数据保持严格递增、不重复、内容完整；最后一条就是写端最后发布的那条。
 *  - 不改共享内存布局，不改写端；没有落后时行为不变（见 I03S2ShmCrossProcess 跨进程用例）。
 *
 * 构造：单进程。读端在收到第 0 条时阻塞在 handleQuote 里，测试线程按 ShmCaster 协议
 * 连续写入 3 倍容量的数据，再放开读端。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "ParserShm/ParserShm.h"

#include "Includes/FasterDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

namespace i03k51 {

constexpr uint64_t kCapacity = 8 * 1024;
constexpr uint32_t kLapped = 3 * (uint32_t)kCapacity;	// 读端阻塞期间再写入的条数

void fill_tick(WTSTickStruct& t, uint32_t seq)
{
	memset(&t, 0, sizeof(t));
	strcpy(t.exchg, "SHFE");
	strcpy(t.code, "ag2512");
	t.trading_date = 20240102;
	t.action_date = 20240102;
	t.action_time = seq;
	t.price = 5000.0 + seq;
	t.total_volume = seq * 3.0;
	t.open_interest = (double)(seq ^ 0x5a5a5a5au);
}

class BdMgr : public IBaseDataMgr
{
public:
	explicit BdMgr(WTSContractInfo* ct) : _ct(ct) {}
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }
	WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t) override
	{
		if (code != nullptr && exchg != nullptr && strcmp(code, _ct->getCode()) == 0 && strcmp(exchg, _ct->getExchg()) == 0)
			return _ct;
		return nullptr;
	}
	WTSArray* getContracts(const char*, uint32_t) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t d, uint32_t, bool) override { return d; }
	uint64_t getBoundaryTime(const char*, uint32_t d, bool, bool) override { return d; }
	uint32_t getGlobalSize() override { return 8; }
private:
	WTSContractInfo* _ct;
};

class GatedSink : public IParserSpi
{
public:
	explicit GatedSink(WTSContractInfo* ct) : _bd(ct) {}
	IBaseDataMgr* getBaseDataMgr() override { return &_bd; }
	void handleSymbolList(const WTSArray*) override {}
	void handleParserLog(WTSLogLevel ll, const char* message) override
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (ll == LL_WARN && message != nullptr)
			_warns.emplace_back(message);
	}
	void handleEvent(WTSParserEvent e, int32_t) override
	{
		if (e == WPE_Login)
			_ready.store(true, std::memory_order_release);
	}
	void handleQuote(WTSTickData* tick, uint32_t) override
	{
		const WTSTickStruct& t = tick->getTickStruct();
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_seqs.push_back(t.action_time);
			if ((uint32_t)t.open_interest != (t.action_time ^ 0x5a5a5a5au) || t.price != 5000.0 + t.action_time)
				_bad++;
		}
		_last.store((int64_t)t.action_time, std::memory_order_release);
		if (!_gate_passed)
		{
			_gate_passed = true;
			_blocked.store(true, std::memory_order_release);
			std::unique_lock<std::mutex> lock(_gate_mtx);
			_gate_cv.wait(lock, [this]() { return _open; });
		}
	}
	void open()
	{
		{
			std::lock_guard<std::mutex> lock(_gate_mtx);
			_open = true;
		}
		_gate_cv.notify_all();
	}

	std::vector<uint32_t> seqs() { std::lock_guard<std::mutex> lock(_mtx); return _seqs; }
	std::vector<std::string> warns() { std::lock_guard<std::mutex> lock(_mtx); return _warns; }
	uint32_t bad() { std::lock_guard<std::mutex> lock(_mtx); return _bad; }

	std::atomic<bool> _ready { false };
	std::atomic<bool> _blocked { false };
	std::atomic<int64_t> _last { -1 };

private:
	BdMgr _bd;
	std::mutex _mtx;
	std::vector<uint32_t> _seqs;
	std::vector<std::string> _warns;
	uint32_t _bad = 0;
	bool _gate_passed = false;
	std::mutex _gate_mtx;
	std::condition_variable _gate_cv;
	bool _open = false;
};

void publish(ParserShm::CastQueue* q, uint32_t seq)
{
	const uint64_t wIdx = q->_writable++;
	ParserShm::DataItem& item = q->_items[wIdx % kCapacity];
	item._type = 0;
	fill_tick(item._tick, seq);
	std::atomic_thread_fence(std::memory_order_release);	// 与 ShmCaster 相同
	q->_readable = wIdx;
}

bool wait_for(const std::function<bool()>& pred, int seconds = 30)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!pred())
	{
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

} // namespace i03k51

using namespace i03k51;

TEST(I03K51ShmSlowConsumer, LappedConsumerDropsOldestWarnsAndDeliversContiguousTail)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / ("i03_k51_shm_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const std::string path = (dir / "cast_queue.bin").string();
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f.seekp(sizeof(ParserShm::CastQueue) - 1);
		f.put('\0');
	}
	BoostMappingFile map;
	ASSERT_TRUE(map.map(path.c_str()));
	ParserShm::CastQueue* q = new (map.addr()) ParserShm::CastQueue();
	q->_pid = (uint32_t)::getpid();

	WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "TRADING", "CHINA");
	WTSContractInfo* ct = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
	ct->setCommInfo(comm);
	ct->setTotalIndex(3);

	GatedSink sink(ct);
	{
		ParserShm parser;
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", path.c_str());
		cfg->append("checkspan", (uint32_t)1);
		ASSERT_TRUE(parser.init(cfg));
		cfg->release();
		parser.registerSpi(&sink);
		CodeSet codes;
		codes.insert("SHFE.ag2512");
		parser.subscribe(codes);
		ASSERT_TRUE(parser.connect());
		ASSERT_TRUE(wait_for([&]() { return sink._ready.load(); }));
		// 让读端先看到“尚无数据”，之后从 0 开始读
		std::this_thread::sleep_for(std::chrono::milliseconds(200));

		publish(q, 0);
		ASSERT_TRUE(wait_for([&]() { return sink._blocked.load(); })) << "consumer never received item 0";
		for (uint32_t seq = 1; seq <= kLapped; seq++)
			publish(q, seq);
		sink.open();

		const bool caughtUp = wait_for([&]() { return sink._last.load() >= (int64_t)kLapped; });
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		parser.disconnect();
		parser.release();
		ASSERT_TRUE(caughtUp) << "consumer did not reach the last published item";
	}

	const std::vector<uint32_t> seqs = sink.seqs();
	ASSERT_FALSE(seqs.empty());
	EXPECT_EQ(0u, seqs.front());
	EXPECT_EQ(kLapped, seqs.back());
	for (size_t i = 1; i < seqs.size(); i++)
	{
		if (seqs[i] <= seqs[i - 1])
		{
			ADD_FAILURE() << "out-of-order or duplicate delivery at " << i << ": " << seqs[i - 1] << " then " << seqs[i];
			break;
		}
	}
	// 0 之后交付的必须是连续的一段尾部，且不多于一个容量
	ASSERT_GE(seqs.size(), 2u);
	const uint32_t tailStart = seqs[1];
	EXPECT_EQ(seqs.size() - 1, (size_t)(kLapped - tailStart + 1)) << "tail is not contiguous";
	EXPECT_LE(seqs.size() - 1, (size_t)kCapacity);
	EXPECT_EQ(0u, sink.bad()) << "an overwritten or torn slot was delivered";

	const uint64_t dropped = (uint64_t)tailStart - 1;
	uint32_t lagWarns = 0;
	bool countMentioned = false;
	for (const std::string& w : sink.warns())
	{
		if (w.find("lagged behind") != std::string::npos)
		{
			lagWarns++;
			if (w.find(std::to_string(dropped)) != std::string::npos)
				countMentioned = true;
		}
	}
	EXPECT_GE(lagWarns, 1u) << "no lag warning was logged";
	EXPECT_TRUE(countMentioned) << "lag warning does not mention the " << dropped << " dropped items";

	ct->release();
	comm->release();
	fs::remove_all(dir, ec);
}
