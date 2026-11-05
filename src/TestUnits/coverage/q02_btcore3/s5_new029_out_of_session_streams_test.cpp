/**
 * s5_new029_out_of_session_streams_test.cpp  (BUG-I02-NEW-029, E-class contract)
 *
 * Contract: for each HFT stream separately (tick, order detail, order queue,
 * transaction) a record outside the code's trading session (pre-open, lunch
 * break, after close, or with an invalid action time) is
 *   - not dispatched to the sink,
 *   - not reconciled (no price/dispatch bookkeeping, which only follows a
 *     dispatch), and
 *   - observable: counted per stream (get_out_of_session_count) and reported
 *     once per stream/code/bucket with a WARN.
 * In-session records of every stream are still dispatched (positive control).
 * SSE.STK uses session SD0930 (09:30-11:30, 13:00-15:00).
 */
#include "r91_common.h"

#include <set>

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDay = 20200102u;

	// pre-open, in-session, invalid second 60, lunch break, in-session, after close
	const uint32_t kTimes[] = { 92500000u, 93100000u, 93160000u, 120000000u, 130100000u, 150100000u };

	template <typename T>
	std::vector<T> make_items(T (*mk)(uint32_t, uint32_t, double, uint32_t, const char*))
	{
		std::vector<T> items;
		double px = 10.0;
		for (uint32_t t : kTimes)
		{
			T item = mk(kDay, 0u, px, 1u, "000001");
			item.action_time = t;
			items.push_back(item);
			px += 0.1;
		}
		return items;
	}

	WTSTickStruct mk_tick_any(uint32_t date, uint32_t hhmm, double price, uint32_t volume, const char* code)
	{
		return r91::mk_tick(date, hhmm, price, static_cast<double>(volume), code);
	}

	std::multiset<uint32_t> tick_times(const std::vector<r91::TickRec>& recs)
	{
		std::multiset<uint32_t> out;
		for (const auto& r : recs) out.insert(r.time);
		return out;
	}

	std::multiset<uint32_t> hft_times(const std::vector<r91::HftRec>& recs)
	{
		std::multiset<uint32_t> out;
		for (const auto& r : recs) out.insert(r.time);
		return out;
	}

	class S5New029OutOfSession : public r91::Fixture
	{
	protected:
		void write_all()
		{
			ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000001", kDay, make_items<WTSTickStruct>(&mk_tick_any)));
			ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDay, make_items<WTSOrdDtlStruct>(&r91::mk_orddtl)));
			ASSERT_TRUE(r91::write_ordque(_base, "SSE", "000001", kDay, make_items<WTSOrdQueStruct>(&r91::mk_ordque)));
			ASSERT_TRUE(r91::write_trans(_base, "SSE", "000001", kDay, make_items<WTSTransStruct>(&r91::mk_trans)));
		}

		void replay(HisDataReplayer& replayer, r91::Sink& sink)
		{
			r91::CfgOpts opts;
			opts.mode = "bin";
			opts.tick = true;
			WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020900ull, 202001021530ull, opts);
			ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
			cfg->release();
			replayer.register_sink(&sink, "s5_new029");
			ASSERT_TRUE(replayer.prepare());
			replayer.sub_tick(1u, kCode);
			replayer.sub_order_detail(2u, kCode);
			replayer.sub_order_queue(3u, kCode);
			replayer.sub_transaction(4u, kCode);
			replayer.run(false);
		}
	};
}

// Each stream: only the two in-session records are dispatched.
TEST_F(S5New029OutOfSession, EachStreamDispatchesOnlyInSessionRecords)
{
	write_all();
	HisDataReplayer replayer;
	r91::Sink sink;
	replay(replayer, sink);
	const std::multiset<uint32_t> expected = { 931u, 1301u };
	EXPECT_EQ(expected, tick_times(sink.ticks)) << "tick stream";
	EXPECT_EQ(expected, hft_times(sink.details)) << "order detail stream";
	EXPECT_EQ(expected, hft_times(sink.queues)) << "order queue stream";
	EXPECT_EQ(expected, hft_times(sink.trans)) << "transaction stream";
	EXPECT_EQ(1u, sink.doneCalls);
}

#ifndef WT_S5_NO_OOS_COUNTER
// Each stream: the four skipped records are observable through the counter.
TEST_F(S5New029OutOfSession, EachStreamCountsSkippedRecords)
{
	write_all();
	HisDataReplayer replayer;
	r91::Sink sink;
	replay(replayer, sink);
	for (uint32_t kind = 0; kind < 4; ++kind)
		EXPECT_EQ(4u, replayer.get_out_of_session_count(kind)) << "stream kind " << kind;
	EXPECT_EQ(0u, replayer.get_out_of_session_count(4u));
	// A new generation starts from zero.
	ASSERT_TRUE(replayer.prepare());
	for (uint32_t kind = 0; kind < 4; ++kind)
		EXPECT_EQ(0u, replayer.get_out_of_session_count(kind)) << "stream kind " << kind;
}
#endif
