// Iteration 03 card 11 = B (shared owner) -- WtRdmDtReader behavior tests.
// Only public reader APIs are used here, so the same file builds against the
// pre-change baseline snapshot (red control) and the working tree (green).
// Owner-introspection checks are guarded by WT_I03_GREEN.
#include "q03_storage3_fixture.h"
#include "WtDataStorage/WtRdmDtReader.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace q03 {

class I03SliceOwnerRdm : public ::testing::Test {
protected:
	void SetUp() override { sandbox.reset(new Sandbox("i03owner")); reader.init(nullptr, &sink); }
	void TearDown() override { reader.clearCache(); sandbox.reset(); }

	bool write_his_tick(uint32_t seq)
	{
		const auto t = make_tick(20240108, 20240108, 93000000, seq);
		return write_bytes("his/ticks/SSE/20240108/600000.dsb",
			make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks({ t })));
	}

	std::unique_ptr<Sandbox> sandbox;
	BaseDataMgr bd;
	HotMgr hot;
	RdmSink sink{ &bd, &hot };
	WtRdmDtReader reader;
};

// Held historical tick slices survive repeated clear + reload; each keeps its own generation.
TEST_F(I03SliceOwnerRdm, HistoricalTickHeldAcrossRepeatedReloads)
{
	std::vector<WTSTickSlice*> held;
	for (uint32_t i = 1; i <= 40; i++)
	{
		ASSERT_TRUE(write_his_tick(i));
		reader.clearCache();
		WTSTickSlice* s = reader.readTickSliceByDate("SSE.STK.600000", 20240108);
		ASSERT_NE(s, nullptr);
		ASSERT_EQ(s->size(), 1u);
		EXPECT_DOUBLE_EQ(s->at(0)->price, 30.0 + i);
		held.push_back(s);
	}
	reader.clearCache();
	for (uint32_t i = 0; i < held.size(); i++)
		EXPECT_DOUBLE_EQ(held[i]->at(0)->price, 30.0 + (i + 1)) << "generation " << i + 1;
	for (auto* s : held)
		s->release();
}

// Historical day bars: a held kline slice survives clearCache and the reload sees new data.
TEST_F(I03SliceOwnerRdm, HistoricalKlineHeldAcrossClear)
{
	std::vector<WTSBarStruct> oldBars{ make_bar(20240102, 0, 1), make_bar(20240103, 0, 2) };
	std::vector<WTSBarStruct> newBars{ make_bar(20240102, 0, 7), make_bar(20240103, 0, 8) };
	ASSERT_TRUE(write_bytes("his/day/SSE/600000.dsb", make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(oldBars))));
	WTSKlineSlice* saved = reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY, 202401010000ull, 202401090000ull);
	ASSERT_NE(saved, nullptr);
	ASSERT_EQ(saved->size(), 2);
	EXPECT_DOUBLE_EQ(saved->at(1)->close, oldBars[1].close);

	ASSERT_TRUE(write_bytes("his/day/SSE/600000.dsb", make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(newBars))));
	reader.clearCache();
	EXPECT_DOUBLE_EQ(saved->at(0)->close, oldBars[0].close);
	EXPECT_DOUBLE_EQ(saved->at(1)->close, oldBars[1].close);

	WTSKlineSlice* fresh = reader.readKlineSliceByRange("SSE.STK.600000", KP_DAY, 202401010000ull, 202401090000ull);
	ASSERT_NE(fresh, nullptr);
	ASSERT_EQ(fresh->size(), 2);
	EXPECT_DOUBLE_EQ(fresh->at(1)->close, newBars[1].close);
	fresh->release();
	saved->release();
}

// Concurrent readers hold/release historical tick and transaction slices while another
// thread repeatedly clears the cache. Run under TSan/ASan.
TEST_F(I03SliceOwnerRdm, ConcurrentReadersAndClearCache)
{
	ASSERT_TRUE(write_his_tick(3));
	const auto tr = make_trans(20240108, 20240108, 93000000, 5);
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240108/600000.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(std::vector<WTSTransStruct>{ tr }))));

	std::atomic<bool> stop{ false };
	std::atomic<uint32_t> bad{ 0 };
	std::atomic<uint64_t> reads{ 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 3; t++)
	{
		threads.emplace_back([&]() {
			std::vector<WTSTickSlice*> keep;
			while (!stop.load())
			{
				WTSTickSlice* s = reader.readTickSliceByDate("SSE.STK.600000", 20240108);
				if (s == nullptr || s->size() != 1 || s->at(0)->price != 33.0)
					bad++;
				if (s != nullptr)
					keep.push_back(s);
				WTSTransSlice* ts = reader.readTransSliceByRange("SSE.STK.600000", 20240108090000000ull, 20240108100000000ull);
				if (ts == nullptr || ts->size() != 1 || ts->at(0)->price != tr.price)
					bad++;
				if (ts != nullptr)
					ts->release();
				if (keep.size() >= 8)
				{
					for (auto* k : keep)
					{
						if (k->at(0)->price != 33.0)
							bad++;
						k->release();
					}
					keep.clear();
				}
				reads++;
			}
			for (auto* k : keep)
				k->release();
		});
	}
	std::thread clearer([&]() {
		// keep clearing until the readers have actually overlapped with the clears
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		for (int i = 0; (i < 300 || reads.load() < 3000) && std::chrono::steady_clock::now() < deadline; i++)
		{
			reader.clearCache();
			std::this_thread::yield();
		}
		stop = true;
	});
	clearer.join();
	for (auto& th : threads)
		th.join();
	EXPECT_EQ(bad.load(), 0u);
	EXPECT_GE(reads.load(), 3000u);
}

#if defined(WT_I03_GREEN)
// Introspection only available with the new owner API.
TEST_F(I03SliceOwnerRdm, SlicesCarryOwners)
{
	ASSERT_TRUE(write_his_tick(2));
	WTSTickSlice* his = reader.readTickSliceByDate("SSE.STK.600000", 20240108);
	ASSERT_NE(his, nullptr);
	EXPECT_EQ(his->ownerCount(), 1u);

	const auto tick = make_tick(20240110, 20240110, 93000000, 1);
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600000.dmb", make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, 1, 8, { tick })));
	WTSTickSlice* rt = reader.readTickSliceByDate("SSE.STK.600000", 20240110);
	ASSERT_NE(rt, nullptr);
	EXPECT_EQ(rt->ownerCount(), 1u);
	reader.clearCache();
	EXPECT_DOUBLE_EQ(rt->at(0)->price, tick.price);
	rt->release();
	his->release();
}
#endif

}
