// Iteration 03 card 11 = B -- HisDataReplayer query-cache slices (BUG-I02-NEW-095 linkage):
// held tick / order-detail slices stay valid across clear_cache and across the
// trading-date reload that replaces the per-code query cache. Only members that
// exist in both baseline and working tree are used, so it builds for the red control.
#include <gtest/gtest.h>

#include <vector>

#include "WtBtCore/HisDataReplayer.h"
#include "Includes/WTSDataDef.hpp"

struct HisDataReplayerTestAccess
{
	static void prime(HisDataReplayer& r, uint32_t tdate, uint32_t curTime)
	{
		r._tick_enabled = true;
		r._cur_tdate = tdate;
		r._cur_date = tdate;
		r._cur_time = curTime;
		r._cur_secs = 0;
	}

	static WTSTickStruct tick(uint32_t date, uint32_t time, double price)
	{
		WTSTickStruct t{};
		t.trading_date = date;
		t.action_date = date;
		t.action_time = time;
		t.price = price;
		return t;
	}

	// Install a replay bucket; checkTicks() copies it into the query cache.
	static void bucket(HisDataReplayer& r, const char* code, uint32_t date, uint32_t n, double base)
	{
		auto& list = r._replay_ticks_cache[code][date];
		std::vector<WTSTickStruct> items;
		for (uint32_t i = 0; i < n; i++)
			items.push_back(tick(date, 93000000 + i * 1000, base + i));
		list._items = items;
		list._code = code;
		list._date = date;
		list._count = n;
		list._cursor = UINT_MAX;
	}

	static void orddtl(HisDataReplayer& r, const char* code, uint32_t date, uint32_t n, double base)
	{
		auto& list = r._orddtl_cache[code];
		std::vector<WTSOrdDtlStruct> items(n);
		for (uint32_t i = 0; i < n; i++)
		{
			items[i].trading_date = date;
			items[i].action_date = date;
			items[i].action_time = 93000000 + i * 1000;
			items[i].price = base + i;
		}
		list._items = items;
		list._code = code;
		list._date = date;
		list._count = n;
		list._cursor = UINT_MAX;
	}
};

using A = HisDataReplayerTestAccess;

TEST(I03SliceOwnerReplay, HeldTickSliceSurvivesClearCache)
{
	HisDataReplayer rp;
	A::prime(rp, 20240102, 1500);
	A::bucket(rp, "SSE.STK.600000", 20240102, 8, 10.0);

	WTSTickSlice* held = rp.get_tick_slice("SSE.STK.600000", 8, 0);
	ASSERT_NE(held, nullptr);
	ASSERT_EQ(held->size(), 8u);
	rp.clear_cache();
	for (uint32_t i = 0; i < 8; i++)
		EXPECT_DOUBLE_EQ(held->at((int32_t)i)->price, 10.0 + i);
	held->release();
}

TEST(I03SliceOwnerReplay, HeldTickSliceSurvivesTradingDateReload)
{
	HisDataReplayer rp;
	A::prime(rp, 20240102, 1500);
	A::bucket(rp, "SSE.STK.600000", 20240102, 4, 10.0);
	A::bucket(rp, "SSE.STK.600000", 20240103, 4, 50.0);	// same size: old code overwrites in place
	A::bucket(rp, "SSE.STK.600001", 20240102, 4, 10.0);
	A::bucket(rp, "SSE.STK.600001", 20240103, 4096, 70.0);	// larger: old code reallocates

	WTSTickSlice* heldSame = rp.get_tick_slice("SSE.STK.600000", 4, 0);
	WTSTickSlice* heldGrow = rp.get_tick_slice("SSE.STK.600001", 4, 0);
	ASSERT_NE(heldSame, nullptr);
	ASSERT_NE(heldGrow, nullptr);

	A::prime(rp, 20240103, 1500);
	WTSTickSlice* nextSame = rp.get_tick_slice("SSE.STK.600000", 4, 0);
	WTSTickSlice* nextGrow = rp.get_tick_slice("SSE.STK.600001", 4, 0);
	ASSERT_NE(nextSame, nullptr);
	ASSERT_NE(nextGrow, nullptr);
	EXPECT_DOUBLE_EQ(nextSame->at(0)->price, 50.0);

	for (uint32_t i = 0; i < 4; i++)
	{
		EXPECT_DOUBLE_EQ(heldSame->at((int32_t)i)->price, 10.0 + i) << "same-size reload, index " << i;
		EXPECT_DOUBLE_EQ(heldGrow->at((int32_t)i)->price, 10.0 + i) << "growing reload, index " << i;
	}
	nextSame->release();
	nextGrow->release();
	heldSame->release();
	heldGrow->release();
}

TEST(I03SliceOwnerReplay, HeldOrderDetailSliceSurvivesClearCache)
{
	HisDataReplayer rp;
	A::prime(rp, 20240102, 1500);
	A::orddtl(rp, "SSE.STK.600000", 20240102, 6, 20.0);
	WTSOrdDtlSlice* held = rp.get_order_detail_slice("SSE.STK.600000", 6, 0);
	ASSERT_NE(held, nullptr);
	ASSERT_EQ(held->size(), 6u);
	rp.clear_cache();
	for (uint32_t i = 0; i < 6; i++)
		EXPECT_DOUBLE_EQ(held->at((int32_t)i)->price, 20.0 + i);
	held->release();
}
