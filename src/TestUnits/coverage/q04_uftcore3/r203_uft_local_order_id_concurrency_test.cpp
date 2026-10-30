// Run this test alone in a fresh process to observe concurrent first orders through
// TraderAdapter::openLong -> doEntrust -> the production local-ID generator.
#include "q04_trader_fixture.h"

#include <atomic>
#include <memory>
#include <set>
#include <thread>

using namespace wtp;

namespace {

struct LocalOrderIdEnv
{
	q04::TempRoot tmp{ "local_id" };
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	q04::FakeSink sink;
	TraderAdapter adapter;

	explicit LocalOrderIdEnv(const std::string& id)
	{
		bd.add("SHFE", "ag", "ag2406");
		const std::string path = tmp.file("policy.json");
		tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(policy.init(path.c_str()));
		EXPECT_TRUE(adapter.initExt(id.c_str(), &api, &bd, &policy));
		adapter.addSink(&sink);
		EXPECT_TRUE(adapter.run());
		adapter.onLoginResult(true, "ok", 20240315);
	}
};

} // namespace

TEST(R203UftLocalOrderId, ConcurrentFirstOrdersHaveUniqueIds)
{
	constexpr size_t kThreadCount = 24;
	std::vector<std::unique_ptr<LocalOrderIdEnv>> envs;
	envs.reserve(kThreadCount);
	for (size_t i = 0; i < kThreadCount; ++i)
		envs.emplace_back(std::make_unique<LocalOrderIdEnv>("r203_" + std::to_string(i)));

	std::atomic<size_t> ready{ 0 };
	std::atomic<bool> start{ false };
	std::vector<uint32_t> ids(kThreadCount, UINT_MAX);
	std::vector<std::thread> workers;
	workers.reserve(kThreadCount);
	for (size_t i = 0; i < kThreadCount; ++i)
	{
		workers.emplace_back([&, i]() {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			ids[i] = envs[i]->adapter.openLong("SHFE.ag2406", 5000.0, 1.0, 0);
		});
	}
	while (ready.load(std::memory_order_acquire) != kThreadCount)
		std::this_thread::yield();
	start.store(true, std::memory_order_release);
	for (std::thread& worker : workers)
		worker.join();

	std::set<uint32_t> uniqueIds(ids.begin(), ids.end());
	EXPECT_EQ(kThreadCount, uniqueIds.size())
		<< "并发首次通过 TraderAdapter::openLong 生成的本地委托号必须互异";
	for (size_t i = 0; i < kThreadCount; ++i)
	{
		ASSERT_NE(UINT_MAX, ids[i]);
		ASSERT_NE(0u, ids[i]) << "0 保留为无订单哨兵";
		ASSERT_EQ(1u, envs[i]->api._entrusts.size());
		EXPECT_EQ("wtp.r203_" + std::to_string(i) + "." + std::to_string(ids[i]),
			envs[i]->api._entrusts[0]._usertag);
	}
}
