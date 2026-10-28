// N05 lane：WtExeFact 订单监控器 WtOrdMon 纯逻辑测试
// 目标文件: src/WtExeFact/WtOrdMon.cpp / WtOrdMon.h
// 覆盖符号: push_order / erase_order / has_order / check_orders / clear_orders / enumOrder
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "WtExeFact/WtOrdMon.h"

namespace
{
// 构造一组连续本地订单号
std::vector<uint32_t> mkIds(uint32_t begin, uint32_t cnt)
{
	std::vector<uint32_t> ids;
	ids.reserve(cnt);
	for (uint32_t i = 0; i < cnt; i++)
		ids.push_back(begin + i);
	return ids;
}
}

//////////////////////////////////////////////////////////////////////////
// push_order / has_order
//////////////////////////////////////////////////////////////////////////
TEST(n05_ordmon, push_then_has_order)
{
	WtOrdMon mon;
	EXPECT_FALSE(mon.has_order());      // 空监控器，任意订单=false
	EXPECT_FALSE(mon.has_order(1001));  // 空监控器，指定订单=false

	auto ids = mkIds(1001, 3);
	mon.push_order(ids.data(), ids.size(), 20220101093000000ull);
	EXPECT_TRUE(mon.has_order());
	EXPECT_TRUE(mon.has_order(1001));
	EXPECT_TRUE(mon.has_order(1002));
	EXPECT_TRUE(mon.has_order(1003));
	EXPECT_FALSE(mon.has_order(1004));
}

TEST(n05_ordmon, push_zero_cnt_is_noop)
{
	WtOrdMon mon;
	uint32_t one = 5000;
	mon.push_order(&one, 0, 100);
	EXPECT_FALSE(mon.has_order());
	EXPECT_FALSE(mon.has_order(5000));
}

TEST(n05_ordmon, push_duplicate_id_overwrites)
{
	WtOrdMon mon;
	uint32_t id = 7001;
	mon.push_order(&id, 1, 1000, true);
	mon.push_order(&id, 1, 2000, false);

	std::vector<std::tuple<uint32_t, uint64_t, bool>> got;
	mon.enumOrder([&](uint32_t lid, uint64_t et, bool cc) {
		got.emplace_back(lid, et, cc);
	});
	ASSERT_EQ(got.size(), 1u);
	EXPECT_EQ(std::get<0>(got[0]), 7001u);
	EXPECT_EQ(std::get<1>(got[0]), 2000u);   // 后来者覆盖 entertime
	EXPECT_FALSE(std::get<2>(got[0]));        // 后来者覆盖 cancancel
}

//////////////////////////////////////////////////////////////////////////
// erase_order / clear_orders
//////////////////////////////////////////////////////////////////////////
TEST(n05_ordmon, erase_removes_only_target)
{
	WtOrdMon mon;
	auto ids = mkIds(2001, 4);
	mon.push_order(ids.data(), ids.size(), 100);

	mon.erase_order(2002);
	EXPECT_FALSE(mon.has_order(2002));
	EXPECT_TRUE(mon.has_order(2001));
	EXPECT_TRUE(mon.has_order(2003));
	EXPECT_TRUE(mon.has_order(2004));
	EXPECT_TRUE(mon.has_order());
}

TEST(n05_ordmon, erase_missing_id_is_noop)
{
	WtOrdMon mon;
	auto ids = mkIds(3001, 2);
	mon.push_order(ids.data(), ids.size(), 100);
	mon.erase_order(9999);  // 不在监控器中，安全返回
	EXPECT_TRUE(mon.has_order(3001));
	EXPECT_TRUE(mon.has_order(3002));
}

TEST(n05_ordmon, clear_orders_empties)
{
	WtOrdMon mon;
	auto ids = mkIds(4001, 5);
	mon.push_order(ids.data(), ids.size(), 100);
	mon.clear_orders();
	EXPECT_FALSE(mon.has_order());
	EXPECT_FALSE(mon.has_order(4001));
}

//////////////////////////////////////////////////////////////////////////
// check_orders 超时判定（纯算术，不涉及墙钟）
//////////////////////////////////////////////////////////////////////////
TEST(n05_ordmon, check_orders_notifies_only_expired)
{
	WtOrdMon mon;
	// id 6001 于 t=10000 挂单，id 6002 于 t=50000 挂单
	uint32_t a = 6001, b = 6002;
	mon.push_order(&a, 1, 10000);
	mon.push_order(&b, 1, 50000);

	std::vector<uint32_t> expired;
	// 超时 60s = 60000ms；当前时间 70000
	// 6001: 70000-10000=60000 >= 60000 -> 过期（边界为 "< expire*1000 则跳过"）
	// 6002: 70000-50000=20000 < 60000 -> 未过期
	mon.check_orders(60, 70000, [&](uint32_t lid) { expired.push_back(lid); });
	ASSERT_EQ(expired.size(), 1u);
	EXPECT_EQ(expired[0], 6001u);
}

TEST(n05_ordmon, check_orders_skips_non_cancellable)
{
	WtOrdMon mon;
	uint32_t a = 6101, b = 6102;
	mon.push_order(&a, 1, 0, true);    // 可撤
	mon.push_order(&b, 1, 0, false);   // 不可撤（如涨跌停挂单）

	std::vector<uint32_t> expired;
	mon.check_orders(10, 1000000, [&](uint32_t lid) { expired.push_back(lid); });
	// 即使早已超时，bCanCancel=false 的订单一律跳过
	EXPECT_EQ(expired.size(), 1u);
	EXPECT_EQ(expired[0], 6101u);
}

TEST(n05_ordmon, check_orders_empty_monitor_no_callback)
{
	WtOrdMon mon;
	int hits = 0;
	mon.check_orders(0, 999999999, [&](uint32_t) { hits++; });
	EXPECT_EQ(hits, 0);
}

TEST(n05_ordmon, check_orders_zero_expire_notifies_all_cancellable)
{
	WtOrdMon mon;
	auto ids = mkIds(6201, 3);
	mon.push_order(ids.data(), ids.size(), 5000);
	std::vector<uint32_t> expired;
	mon.check_orders(0, 5000, [&](uint32_t lid) { expired.push_back(lid); });
	EXPECT_EQ(expired.size(), 3u);  // curTime-enter(=0) 不小于 0*1000，全部过期
}

//////////////////////////////////////////////////////////////////////////
// enumOrder
//////////////////////////////////////////////////////////////////////////
TEST(n05_ordmon, enum_order_reports_full_detail)
{
	WtOrdMon mon;
	uint32_t a = 7101, b = 7102, c = 7103;
	mon.push_order(&a, 1, 111, true);
	mon.push_order(&b, 1, 222, false);
	mon.push_order(&c, 1, 333);  // 默认 cancancel=true

	std::vector<uint32_t> lids;
	std::vector<uint64_t> ents;
	std::vector<bool> cancels;
	mon.enumOrder([&](uint32_t lid, uint64_t et, bool cc) {
		lids.push_back(lid);
		ents.push_back(et);
		cancels.push_back(cc);
	});
	ASSERT_EQ(lids.size(), 3u);
	// unordered_map 顺序不定，按 id 校验对应 entertime/cancancel
	for (size_t i = 0; i < lids.size(); i++)
	{
		switch (lids[i])
		{
		case 7101: EXPECT_EQ(ents[i], 111u); EXPECT_TRUE(cancels[i]); break;
		case 7102: EXPECT_EQ(ents[i], 222u); EXPECT_FALSE(cancels[i]); break;
		case 7103: EXPECT_EQ(ents[i], 333u); EXPECT_TRUE(cancels[i]); break;
		default: FAIL() << "unexpected localid " << lids[i];
		}
	}
}

TEST(n05_ordmon, enum_order_empty_no_callback)
{
	WtOrdMon mon;
	int hits = 0;
	mon.enumOrder([&](uint32_t, uint64_t, bool) { hits++; });
	EXPECT_EQ(hits, 0);
}
