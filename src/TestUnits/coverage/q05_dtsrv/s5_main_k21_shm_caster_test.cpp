/*!
 * 迭代03 第五会话主线程：决策 K21（BUG-R18-011）——ShmCaster 进程内多生产者串行化。
 * 多个 parser 线程并发广播时，写下标不得丢失增量，每个槽位都是完整写入的 tick。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <WtDtCore/ShmCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

using namespace wtp;
namespace fs = std::filesystem;

TEST(S5MainK21ShmCaster, ConcurrentBroadcastsAreSerializedWithoutLostSlots)
{
	const fs::path dir = fs::temp_directory_path() / ("s5_k21_shm_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);
	const fs::path file = dir / "cast_queue.bin";

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("path", file.string().c_str());
	ShmCaster caster;
	ASSERT_TRUE(caster.init(cfg));
	cfg->release();

	constexpr int kThreads = 4;
	constexpr int kPerThread = 1500;	// 6000 < 8192，环形缓冲不回绕
	std::vector<std::thread> producers;
	for (int t = 0; t < kThreads; t++)
	{
		producers.emplace_back([&caster, t]() {
			for (int i = 0; i < kPerThread; i++)
			{
				WTSTickData* tick = WTSTickData::create("SHFE.rb2510");
				tick->getTickStruct().price = t * 100000 + i + 1;
				caster.broadcast(tick);
				tick->release();
			}
		});
	}
	for (auto& th : producers)
		th.join();

	std::ifstream in(file, std::ios::binary);
	ASSERT_TRUE(in.good());
	std::unique_ptr<ShmCaster::CastQueue> q(new ShmCaster::CastQueue());
	in.read(reinterpret_cast<char*>(q.get()), sizeof(ShmCaster::CastQueue));
	ASSERT_EQ(static_cast<std::streamsize>(sizeof(ShmCaster::CastQueue)), in.gcount());

	const uint64_t total = static_cast<uint64_t>(kThreads) * kPerThread;
	EXPECT_EQ(total, q->_writable) << "a concurrent increment of the write index was lost";
	EXPECT_EQ(total - 1, q->_readable);
	std::set<double> prices;
	for (uint64_t i = 0; i < total; i++)
	{
		EXPECT_EQ(0u, static_cast<uint32_t>(q->_items[i]._type));
		prices.insert(q->_items[i]._tick.price);
	}
	EXPECT_EQ(static_cast<size_t>(total), prices.size()) << "a slot was overwritten by another producer";
	fs::remove_all(dir, ec);
}
