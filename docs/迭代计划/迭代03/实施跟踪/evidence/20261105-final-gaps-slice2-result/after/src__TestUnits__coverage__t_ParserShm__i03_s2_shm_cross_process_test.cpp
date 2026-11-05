/*
 * BUG-R18-011（迭代03 最终缺口，slice2；决策 K21：单生产者/进程内串行化）
 *
 * 跨进程消费者验证：真实 ParserShm 在本进程的工作线程里消费共享内存队列；fork 出的子进程
 * 按 ShmCaster::broadcast 的协议写入（槽位写完 → release 栅栏 → 发布 _readable）。
 * 每条 tick 携带递增序号与覆盖整个结构体的校验和；生产者按消费进度限速，使总量
 * （20000）超过队列容量（8192）而不溢出，覆盖两次环形回绕。断言：
 *   - 收到的序号从 0 连续到 N-1，不丢、不重；
 *   - 每条记录的校验和与序号一致（未读到未写完/撕裂的槽位）。
 * 同时修复消费端：ParserShm 读取 _readable 后加 acquire 栅栏，与生产端的 release 成对
 * （内存序缺陷在本机运行中不保证可复现，红控见证据说明）。
 * “慢消费者落后超过队列容量”的处置不在 K21 范围内，本用例以限速避开该场景。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ParserShm/ParserShm.h"

#include "Includes/FasterDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

namespace i03s2shm {

constexpr uint32_t kTotal = 20000;
constexpr uint64_t kCapacity = 8 * 1024;
constexpr uint64_t kThrottle = kCapacity - 512;

struct SharedProgress
{
	std::atomic<int64_t> last_seq;		// 消费端最后收到的序号，-1 表示尚无
	std::atomic<uint32_t> consumer_ready;	// 消费端已连接（WPE_Login）
};

uint32_t fnv1a(const unsigned char* p, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

// 校验和覆盖整个 tick（校验字段 open_interest 置 0 后计算）
uint32_t tick_checksum(const WTSTickStruct& t)
{
	WTSTickStruct c = t;
	c.open_interest = 0;
	return fnv1a(reinterpret_cast<const unsigned char*>(&c), sizeof(c));
}

void fill_tick(WTSTickStruct& t, uint32_t seq)
{
	memset(&t, 0, sizeof(t));
	strcpy(t.exchg, "SHFE");
	strcpy(t.code, "ag2512");
	t.trading_date = 20240102;
	t.action_date = 20240102;
	t.action_time = seq;
	t.price = 5000.0 + seq;
	t.open = t.price - 1;
	t.high = t.price + 2;
	t.low = t.price - 3;
	t.total_volume = seq * 3.0;
	t.volume = (double)(seq % 97);
	for (int i = 0; i < 10; i++)
	{
		t.bid_prices[i] = t.price - 1 - i;
		t.ask_prices[i] = t.price + 1 + i;
		t.bid_qty[i] = (double)(seq + i);
		t.ask_qty[i] = (double)(seq * 2 + i);
	}
	t.open_interest = (double)tick_checksum(t);
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

class CheckingSink : public IParserSpi
{
public:
	CheckingSink(WTSContractInfo* ct, SharedProgress* progress) : _bd(ct), _progress(progress) {}
	IBaseDataMgr* getBaseDataMgr() override { return &_bd; }
	void handleSymbolList(const WTSArray*) override {}
	void handleParserLog(WTSLogLevel, const char*) override {}
	void handleEvent(WTSParserEvent e, int32_t) override
	{
		if (e == WPE_Login)
			_progress->consumer_ready.store(1, std::memory_order_release);
	}
	void handleQuote(WTSTickData* tick, uint32_t) override
	{
		const WTSTickStruct& t = tick->getTickStruct();
		std::lock_guard<std::mutex> lock(_mtx);
		_seqs.push_back(t.action_time);
		if ((uint32_t)t.open_interest != tick_checksum(t))
			_bad_checksums++;
		_progress->last_seq.store((int64_t)t.action_time, std::memory_order_release);
	}

	std::vector<uint32_t> seqs() { std::lock_guard<std::mutex> lock(_mtx); return _seqs; }
	uint32_t bad() { std::lock_guard<std::mutex> lock(_mtx); return _bad_checksums; }

private:
	BdMgr _bd;
	SharedProgress* _progress;
	std::mutex _mtx;
	std::vector<uint32_t> _seqs;
	uint32_t _bad_checksums = 0;
};

// 子进程：按 ShmCaster::broadcast(WTSTickData*) 的单生产者协议写入，按消费进度限速
int produce(const std::string& path, SharedProgress* progress)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (progress->consumer_ready.load(std::memory_order_acquire) == 0)
	{
		if (std::chrono::steady_clock::now() > deadline)
			return 20;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// 留出消费线程看到“尚无数据”（NODATA → 从 0 开始读）的时间
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	BoostMappingFile map;
	if (!map.map(path.c_str()) || map.size() < sizeof(ParserShm::CastQueue))
		return 21;
	ParserShm::CastQueue* q = static_cast<ParserShm::CastQueue*>(map.addr());
	for (uint32_t seq = 0; seq < kTotal; seq++)
	{
		while ((int64_t)seq - (progress->last_seq.load(std::memory_order_acquire) + 1) >= (int64_t)kThrottle)
		{
			if (std::chrono::steady_clock::now() > deadline)
				return 22;
			std::this_thread::yield();
		}
		const uint64_t wIdx = q->_writable++;
		ParserShm::DataItem& item = q->_items[wIdx % kCapacity];
		item._type = 0;
		fill_tick(item._tick, seq);
		std::atomic_thread_fence(std::memory_order_release);	// 与 ShmCaster 相同
		q->_readable = wIdx;
	}
	return 0;
}

} // namespace i03s2shm

using namespace i03s2shm;

TEST(I03S2ShmCrossProcess, ForkedProducerItemsArriveCompleteContiguousAndAcrossWrap)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / ("i03_s2_shm_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const std::string path = (dir / "cast_queue.bin").string();

	// 与 ShmCaster::init 相同：先建文件并定长，再原地构造空队列（_readable = UINT64_MAX）
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f.seekp(sizeof(ParserShm::CastQueue) - 1);
		f.put('\0');
	}
	{
		BoostMappingFile map;
		ASSERT_TRUE(map.map(path.c_str()));
		ParserShm::CastQueue* q = new (map.addr()) ParserShm::CastQueue();
		q->_pid = (uint32_t)::getpid();
	}

	SharedProgress* progress = static_cast<SharedProgress*>(::mmap(nullptr, sizeof(SharedProgress),
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
	ASSERT_NE(MAP_FAILED, (void*)progress);
	new (progress) SharedProgress();
	progress->last_seq.store(-1);
	progress->consumer_ready.store(0);

	WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "TRADING", "CHINA");
	WTSContractInfo* ct = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
	ct->setCommInfo(comm);
	ct->setTotalIndex(3);

	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
		::_exit(produce(path, progress));

	CheckingSink sink(ct, progress);
	{
		ParserShm parser;
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", path.c_str());
		cfg->append("checkspan", (uint32_t)50);
		ASSERT_TRUE(parser.init(cfg));
		cfg->release();
		parser.registerSpi(&sink);
		CodeSet codes;
		codes.insert("SHFE.ag2512");
		parser.subscribe(codes);
		ASSERT_TRUE(parser.connect());

		int status = 0;
		ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
		ASSERT_TRUE(WIFEXITED(status)) << "producer was signalled";
		ASSERT_EQ(0, WEXITSTATUS(status)) << "producer failed (20 = consumer never connected, 22 = throttle timeout)";

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (progress->last_seq.load() < (int64_t)kTotal - 1 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		parser.disconnect();
		parser.release();
	}

	const std::vector<uint32_t> seqs = sink.seqs();
	ASSERT_EQ((size_t)kTotal, seqs.size()) << "every published item must be delivered exactly once";
	for (uint32_t i = 0; i < kTotal; i++)
	{
		if (seqs[i] != i)
		{
			ADD_FAILURE() << "sequence gap/duplicate at position " << i << ": got " << seqs[i];
			break;
		}
	}
	EXPECT_EQ(0u, sink.bad()) << "a slot was read before its payload was complete";

	ct->release();
	comm->release();
	::munmap(progress, sizeof(SharedProgress));
	fs::remove_all(dir, ec);
}
