/*!
 * \file s1_new006_uft_order_clock_test.cpp
 * \brief I03-NEW-006（slice1 final gaps）UFT 部分：本地委托号的时钟快照失败时 fail-closed
 *
 * UFT TraderAdapter 没有 identity journal，每笔委托都先取一次校验过的时钟快照
 * （TimeUtils::tryGetDateTime，测试经 setOrderClockForTest 注入）；快照失败
 * （时钟或时区不可用）时在 provider 调用之前拒绝，不分配委托号、不记在途，
 * 时钟恢复后下一笔委托可以正常发出。口径与 WtCore 的
 * S1TraderEntryGate.New006ClockFailureRejectsEntryBeforeTheProvider 一致。
 *
 * 只用 q04 的内存替身（FakeTraderApi/FakeBaseDataMgr/FakeSink），不装载厂商库、不真实下单。
 */
#include "q04_trader_fixture.h"

#include <atomic>
#include <climits>
#include <string>

using namespace wtp;

namespace {

std::atomic<bool> g_s1_uft_clock_fails{ false };
std::atomic<uint32_t> g_s1_uft_clock_calls{ 0 };

bool s1UftOrderClock(uint32_t& date, uint32_t& time, uint64_t& epochMillis)
{
	g_s1_uft_clock_calls.fetch_add(1);
	if (g_s1_uft_clock_fails.load())
		return false;
	return TimeUtils::tryGetDateTime(date, time, epochMillis);
}

struct S1UftClockEnv
{
	q04::TempRoot tmp{ "s1_new006" };
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	q04::FakeSink sink;
	TraderAdapter adapter;

	explicit S1UftClockEnv(const char* id)
	{
		bd.add("SHFE", "ag", "ag2406");
		const std::string path = tmp.file("policy.json");
		tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(policy.init(path.c_str()));
		EXPECT_TRUE(adapter.initExt(id, &api, &bd, &policy));
		adapter.addSink(&sink);
		EXPECT_TRUE(adapter.run());
		adapter.onLoginResult(true, "ok", 20240315);
	}

	~S1UftClockEnv()
	{
		adapter.setOrderClockForTest(nullptr);
		g_s1_uft_clock_fails = false;
	}
};

const char* const kS1UftCode = "SHFE.ag2406";

} // namespace

TEST(S1New006UftOrderClock, ClockFailureRejectsEntryBeforeTheProvider)
{
	S1UftClockEnv env("s1_new006_a");
	env.adapter.setOrderClockForTest(&s1UftOrderClock);
	g_s1_uft_clock_calls = 0;

	g_s1_uft_clock_fails = true;
	EXPECT_EQ(UINT_MAX, env.adapter.openLong(kS1UftCode, 5000.0, 1.0, 0));
	EXPECT_EQ(1u, g_s1_uft_clock_calls.load()) << "every UFT entry takes one checked clock snapshot";
	EXPECT_TRUE(env.api._entrusts.empty()) << "no request reaches the provider without a valid clock";
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getUndoneQty(kS1UftCode)) << "nothing is reserved for a rejected entry";

	g_s1_uft_clock_fails = false;
	const uint32_t id = env.adapter.openLong(kS1UftCode, 5000.0, 1.0, 0);
	EXPECT_NE(UINT_MAX, id);
	EXPECT_NE(0u, id) << "0 is the no-order sentinel";
	ASSERT_EQ(1u, env.api._entrusts.size()) << "the next entry retries once the clock is available";
	EXPECT_EQ("wtp.s1_new006_a." + std::to_string(id), env.api._entrusts[0]._usertag);
	EXPECT_DOUBLE_EQ(1.0, env.adapter.getUndoneQty(kS1UftCode));
}

TEST(S1New006UftOrderClock, EveryDirectionIsGatedAndTheDefaultClockIsRestored)
{
	S1UftClockEnv env("s1_new006_b");
	env.adapter.setOrderClockForTest(&s1UftOrderClock);

	g_s1_uft_clock_calls = 0;
	g_s1_uft_clock_fails = true;
	EXPECT_EQ(UINT_MAX, env.adapter.openLong(kS1UftCode, 5000.0, 1.0, 0));
	EXPECT_EQ(UINT_MAX, env.adapter.openShort(kS1UftCode, 5000.0, 1.0, 0));
	EXPECT_EQ(UINT_MAX, env.adapter.closeLong(kS1UftCode, 5000.0, 1.0, false, 0));
	EXPECT_EQ(UINT_MAX, env.adapter.closeShort(kS1UftCode, 5000.0, 1.0, false, 0));
	EXPECT_EQ(4u, g_s1_uft_clock_calls.load()) << "all four actions reach the clock gate";
	EXPECT_TRUE(env.api._entrusts.empty());
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getUndoneQty(kS1UftCode));

	// NULL restores the production clock (TimeUtils::tryGetDateTime).
	env.adapter.setOrderClockForTest(nullptr);
	const uint32_t id = env.adapter.openLong(kS1UftCode, 5000.0, 1.0, 0);
	EXPECT_NE(UINT_MAX, id);
	EXPECT_NE(0u, id);
	EXPECT_EQ(1u, env.api._entrusts.size());
}
