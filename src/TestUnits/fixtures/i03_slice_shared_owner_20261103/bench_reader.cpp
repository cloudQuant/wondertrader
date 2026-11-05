// Reader-level micro-benchmark: cache-hit readTickSliceByDate / readTransSliceByRange
// (+release) through the real WtRdmDtReader. Built against baseline and working tree.
#include "q03_storage3_fixture.h"
#include "WtDataStorage/WtRdmDtReader.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace q03;

template <typename F>
static double ns_per_op(F&& f, int iters)
{
	std::vector<double> samples;
	for (int r = 0; r < 7; r++)
	{
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < iters; i++)
			f();
		auto t1 = std::chrono::steady_clock::now();
		samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
	}
	std::sort(samples.begin(), samples.end());
	return samples[samples.size() / 2];
}

static volatile double g_sink = 0;

TEST(I03SliceReaderBench, CacheHitSliceCost)
{
	Sandbox sb("i03bench");
	BaseDataMgr bd; HotMgr hot; RdmSink sink(&bd, &hot);
	WtRdmDtReader reader;
	reader.init(nullptr, &sink);
	std::vector<WTSTickStruct> ticks;
	std::vector<WTSTransStruct> trans;
	for (uint32_t i = 0; i < 4000; i++)
	{
		ticks.push_back(make_tick(20240108, 20240108, 93000000 + i * 500, i));
		trans.push_back(make_trans(20240108, 20240108, 93000000 + i * 500, i));
	}
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240108/600000.dsb", make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(ticks))));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240108/600000.dsb", make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));
	const int N = 200000;
	double byDate = ns_per_op([&]() {
		WTSTickSlice* s = reader.readTickSliceByDate("SSE.STK.600000", 20240108);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	double trRange = ns_per_op([&]() {
		WTSTransSlice* s = reader.readTransSliceByRange("SSE.STK.600000", 20240108093000000ull, 20240108100000000ull);
		g_sink = g_sink + s->at(-1)->price;
		s->release();
	}, N);
	std::printf("reader_variant=%s tick_by_date_hit_ns=%.1f trans_by_range_hit_ns=%.1f\n",
#if defined(WT_I03_GREEN)
		"new",
#else
		"baseline",
#endif
		byDate, trRange);
	reader.clearCache();
}
