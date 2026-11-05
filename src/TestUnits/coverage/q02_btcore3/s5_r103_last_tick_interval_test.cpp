/**
 * s5_r103_last_tick_interval_test.cpp  (BUG-R103-002, E-class engineering contract)
 *
 * Contract chosen for the historical (non-dispatched) get_last_tick cursor:
 *   - each record owns the left-closed, right-open time interval
 *     [t_i, t_{i+1}); a query at replay time t returns the record whose
 *     interval contains t, i.e. the last record with time <= t;
 *   - before the first record, and strictly after the last record, the
 *     result is empty (NULL);
 *   - records with the same timestamp keep their insertion (load) order, so
 *     at exactly that time the last inserted one is visible; earlier
 *     same-time records own an empty interval [t, t).
 * Dispatched-order visibility for a subscribed code during run() is covered
 * by r103_replayer_last_tick_boundary_test.cpp.
 */
#include "r91_common.h"

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDate = 20200102u;

	class FixedTickLoader : public r91::Loader
	{
	public:
		std::vector<WTSTickStruct> ticks;
		bool loadRawHisTicks(void* obj, const char*, uint32_t date, FuncReadTicks cb) override
		{
			if (!ticks.empty())
			{
				for (WTSTickStruct& tick : ticks)
				{
					tick.action_date = date;
					tick.trading_date = date;
				}
				cb(obj, ticks.data(), static_cast<uint32_t>(ticks.size()));
			}
			return true;
		}
	};

	class S5R103LastTickInterval : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			_root = r91::root();
			_base = r91::temp_dir("s5_r103_data");
			_out = r91::temp_dir("s5_r103_out");
			WtHelper::setInstDir("");
			WtHelper::setOutputDir(_out.string().c_str());
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			WtHelper::setInstDir("");
			std::error_code ec;
			std::filesystem::remove_all(_base, ec);
			std::filesystem::remove_all(_out, ec);
		}

		// Replay clock is set to stime (minute precision, seconds 0).
		double query_at(uint64_t stime, const std::vector<WTSTickStruct>& ticks)
		{
			FixedTickLoader loader;
			loader.ticks = ticks;
			HisDataReplayer replayer;
			r91::Sink sink;
			r91::CfgOpts opts;
			opts.mode = "csv";
			opts.tick = true;
			replayer.set_time_range(stime, 202001021500ull);
			WTSVariant* cfg = r91::make_cfg(_root, _base, stime, 202001021500ull, opts);
			const bool initialized = replayer.init(cfg, nullptr, &loader);
			cfg->release();
			if (!initialized)
				return -100.0;
			replayer.register_sink(&sink, "s5_r103_interval");
			if (!replayer.prepare())
				return -200.0;
			WTSTickData* tick = replayer.get_last_tick(kCode);
			const double price = tick ? tick->price() : -1.0;
			if (tick) tick->release();
			return price;
		}

		std::filesystem::path _root;
		std::filesystem::path _base;
		std::filesystem::path _out;
	};

	std::vector<WTSTickStruct> same_time_group()
	{
		return {
			r91::mk_tick(kDate, 931u, 10.0, 100.0),
			r91::mk_tick(kDate, 931u, 11.0, 200.0),
			r91::mk_tick(kDate, 933u, 12.0, 300.0),
			r91::mk_tick(kDate, 933u, 13.0, 400.0)
		};
	}
}

// Exactly at a same-time group (not the tail): the last inserted record.
TEST_F(S5R103LastTickInterval, ExactSameTimestampReturnsLastInsertedRecord)
{
	EXPECT_DOUBLE_EQ(11.0, query_at(202001020931ull, same_time_group()));
}

// Exactly at the tail same-time group: still inside the data, last inserted.
TEST_F(S5R103LastTickInterval, ExactTailSameTimestampReturnsLastInsertedRecord)
{
	EXPECT_DOUBLE_EQ(13.0, query_at(202001020933ull, same_time_group()));
}

// Between two groups: the previous record, i.e. the last of the earlier group.
TEST_F(S5R103LastTickInterval, BetweenGroupsReturnsLastRecordOfPreviousGroup)
{
	EXPECT_DOUBLE_EQ(11.0, query_at(202001020932ull, same_time_group()));
}

// Strictly after the tail: empty.
TEST_F(S5R103LastTickInterval, AfterTailReturnsNull)
{
	EXPECT_DOUBLE_EQ(-1.0, query_at(202001020934ull, same_time_group()));
}

// Before the first record: empty.
TEST_F(S5R103LastTickInterval, BeforeFirstReturnsNull)
{
	EXPECT_DOUBLE_EQ(-1.0, query_at(202001020930ull, same_time_group()));
}

// Single-record list: exact hit returns it, one minute later is after the tail.
TEST_F(S5R103LastTickInterval, SingleRecordExactAndAfterTail)
{
	const std::vector<WTSTickStruct> one = { r91::mk_tick(kDate, 931u, 10.0, 100.0) };
	EXPECT_DOUBLE_EQ(10.0, query_at(202001020931ull, one));
	EXPECT_DOUBLE_EQ(-1.0, query_at(202001020932ull, one));
}
