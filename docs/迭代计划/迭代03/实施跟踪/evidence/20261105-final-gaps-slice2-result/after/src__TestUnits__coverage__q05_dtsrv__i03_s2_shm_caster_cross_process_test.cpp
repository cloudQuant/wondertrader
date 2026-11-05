/*
 * BUG-R18-011（迭代03 最终缺口，slice2；决策 K21：进程内串行化 = 单生产者协议）
 *
 * 真实 ShmCaster 作为生产者：4 个 parser 线程并发 broadcast（K21 写锁串行化），fork 出的
 * 子进程按 ParserShm 的读取方式（读 _readable → acquire → 读槽位）跨进程消费。
 * 每条 tick 携带（线程号, 线程内序号）与覆盖整个结构体的校验和；生产者按消费进度限速，
 * 总量 20000 超过队列容量 8192，覆盖环形回绕而不溢出。子进程断言：
 *   - 读到的每个槽位类型正确、校验和一致（不读到未发布或撕裂的槽位）；
 *   - 每个生产线程的序号按 0,1,2… 连续出现（不丢、不重、线程内顺序保持）；
 *   - 总数恰为 20000。
 * 对照：单生产线程同样运行一次（原验收的“正常单 producer 对照”）。
 * “慢消费者落后超过容量”的处置不在 K21 范围内，本用例以限速避开。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <WtDtCore/ShmCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/BoostMappingFile.hpp>

using namespace wtp;
namespace fs = std::filesystem;

namespace i03s2cast {

constexpr uint64_t kCapacity = 8 * 1024;
constexpr uint64_t kThrottle = kCapacity - 512;

struct Shared
{
	std::atomic<uint64_t> consumed;		// 消费端已读条数
	std::atomic<uint32_t> consumer_ready;
};

uint32_t fnv1a(const unsigned char* p, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

uint32_t tick_checksum(const WTSTickStruct& t)
{
	WTSTickStruct c = t;
	c.open_interest = 0;
	return fnv1a(reinterpret_cast<const unsigned char*>(&c), sizeof(c));
}

void fill_tick(WTSTickStruct& t, uint32_t tid, uint32_t seq)
{
	memset(&t, 0, sizeof(t));
	strcpy(t.exchg, "SHFE");
	strcpy(t.code, "rb2510");
	t.trading_date = 20240102;
	t.action_date = tid;
	t.action_time = seq;
	t.price = 3000.0 + tid * 100000 + seq;
	t.total_volume = seq * 7.0 + tid;
	for (int i = 0; i < 10; i++)
	{
		t.bid_prices[i] = t.price - 1 - i;
		t.ask_prices[i] = t.price + 1 + i;
		t.bid_qty[i] = (double)(seq + i + tid);
		t.ask_qty[i] = (double)(seq * 3 + i);
	}
	t.open_interest = (double)tick_checksum(t);
}

// 子进程：ParserShm 的读取规则（_readable 发布后 acquire，再读 ≤ _readable 的槽位）
int consume(const std::string& path, Shared* shared, uint32_t producers, uint32_t perThread)
{
	BoostMappingFile map;
	if (!map.map(path.c_str()) || map.size() < sizeof(ShmCaster::CastQueue))
		return 30;
	ShmCaster::CastQueue* q = static_cast<ShmCaster::CastQueue*>(map.addr());
	std::vector<uint32_t> next(producers, 0);
	const uint64_t total = (uint64_t)producers * perThread;
	uint64_t nextIdx = 0;
	shared->consumer_ready.store(1, std::memory_order_release);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (nextIdx < total)
	{
		if (std::chrono::steady_clock::now() > deadline)
			return 31;
		const uint64_t readable = q->_readable;
		std::atomic_thread_fence(std::memory_order_acquire);
		if (readable == UINT64_MAX || readable < nextIdx)
		{
			std::this_thread::yield();
			continue;
		}
		for (; nextIdx <= readable; nextIdx++)
		{
			const ShmCaster::DataItem& item = q->_items[nextIdx % kCapacity];
			if (item._type != 0)
				return 32;
			const WTSTickStruct t = item._tick;
			if ((uint32_t)t.open_interest != tick_checksum(t))
				return 33;	// 未发布或撕裂的槽位
			if (t.action_date >= producers || t.action_time != next[t.action_date])
				return 34;	// 线程内序号缺口/重复
			next[t.action_date]++;
			shared->consumed.store(nextIdx + 1, std::memory_order_release);
		}
	}
	for (uint32_t tid = 0; tid < producers; tid++)
		if (next[tid] != perThread)
			return 35;
	return 0;
}

int run_case(uint32_t producers, uint32_t perThread)
{
	const fs::path dir = fs::temp_directory_path() /
		("i03_s2_cast_" + std::to_string((long)::getpid()) + "_" + std::to_string(producers));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const std::string path = (dir / "cast_queue.bin").string();

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("path", path.c_str());
	ShmCaster caster;
	const bool inited = caster.init(cfg);
	cfg->release();
	if (!inited)
		return 40;

	Shared* shared = static_cast<Shared*>(::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
	if (shared == MAP_FAILED)
		return 41;
	new (shared) Shared();
	shared->consumed.store(0);
	shared->consumer_ready.store(0);

	const pid_t pid = ::fork();
	if (pid < 0)
		return 42;
	if (pid == 0)
		::_exit(consume(path, shared, producers, perThread));

	const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (shared->consumer_ready.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < readyDeadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	std::atomic<uint64_t> produced{ 0 };
	std::atomic<bool> stalled{ false };
	std::vector<std::thread> threads;
	for (uint32_t tid = 0; tid < producers; tid++)
	{
		threads.emplace_back([&, tid]() {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			for (uint32_t seq = 0; seq < perThread; seq++)
			{
				while (produced.load() - shared->consumed.load(std::memory_order_acquire) >= kThrottle)
				{
					if (std::chrono::steady_clock::now() > deadline)
					{
						stalled = true;
						return;
					}
					std::this_thread::yield();
				}
				WTSTickStruct ts;
				fill_tick(ts, tid, seq);
				WTSTickData* tick = WTSTickData::create(ts);
				caster.broadcast(tick);
				tick->release();
				produced.fetch_add(1);
			}
		});
	}
	for (auto& th : threads)
		th.join();

	int status = 0;
	const pid_t waited = ::waitpid(pid, &status, 0);
	::munmap(shared, sizeof(Shared));
	fs::remove_all(dir, ec);
	if (stalled)
		return 43;
	if (waited != pid || !WIFEXITED(status))
		return 44;
	return WEXITSTATUS(status);
}

} // namespace i03s2cast

TEST(I03S2ShmCasterCrossProcess, ConcurrentProducersReachForkedConsumerCompleteAndInOrderAcrossWrap)
{
	const int rc = i03s2cast::run_case(4, 5000);
	EXPECT_EQ(0, rc) << "33 = torn/unpublished slot, 34 = per-producer sequence gap, 31 = consumer timeout";
}

TEST(I03S2ShmCasterCrossProcess, SingleProducerControl)
{
	EXPECT_EQ(0, i03s2cast::run_case(1, 20000));
}
