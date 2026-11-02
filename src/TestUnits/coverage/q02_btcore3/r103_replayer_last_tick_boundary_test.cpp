/**
 * Direct `get_last_tick` boundary tests for BUG-R103-002.
 *
 * Replay time is set before the first query so each test exercises the first
 * tick-cache lookup through the public replayer API.
 */
#include "r91_common.h"

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDate = 20200102u;

	class TickLoader : public r91::Loader
	{
	public:
		std::vector<WTSTickStruct> ticks;
		uint32_t calls { 0 };

		bool loadRawHisTicks(void* obj, const char*, uint32_t date, FuncReadTicks cb) override
		{
			++calls;
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

	std::vector<WTSTickStruct> make_ticks()
	{
		return {
			r91::mk_tick(kDate, 931u, 10.0, 100.0),
			r91::mk_tick(kDate, 932u, 11.0, 200.0),
			r91::mk_tick(kDate, 933u, 12.0, 300.0)
		};
	}

	class R103LastTickBoundaryTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			_root = r91::root();
			_base = r91::temp_dir("r103_last_tick_data");
			_out = r91::temp_dir("r103_last_tick_out");
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

		bool init(uint64_t stime, TickLoader& loader, HisDataReplayer& replayer, r91::Sink& sink)
		{
			r91::CfgOpts opts;
			opts.mode = "csv";
			opts.tick = true;
			replayer.set_time_range(stime, 202001021500ull);
			WTSVariant* cfg = r91::make_cfg(_root, _base, stime, 202001021500ull, opts);
			const bool initialized = replayer.init(cfg, nullptr, &loader);
			cfg->release();
			if (!initialized)
				return false;
			replayer.register_sink(&sink, "r103_last_tick_boundary");
			return replayer.prepare();
		}

		std::filesystem::path _root;
		std::filesystem::path _base;
		std::filesystem::path _out;
	};
}

TEST_F(R103LastTickBoundaryTest, EmptyTickListReturnsNull)
{
	TickLoader loader;
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));

	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, BeforeFirstTickReturnsNull)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020930ull, loader, replayer, sink));

	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, ExactCurrentTickIsReturned)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020932ull, loader, replayer, sink));

	WTSTickData* tick = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, tick);
	EXPECT_EQ(kDate, tick->actiondate());
	EXPECT_EQ(93200000u, tick->actiontime());
	EXPECT_DOUBLE_EQ(11.0, tick->price());
	tick->release();
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, EmptyHistoricalSliceDoesNotHideCurrentTick)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020932ull, loader, replayer, sink));

	// An earlier slice has an empty prefix but must not change a later query.
	WTSTickSlice* slice = replayer.get_tick_slice(kCode, 1u, 202001020930ull);
	ASSERT_EQ(nullptr, slice);
	WTSTickData* tick = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, tick);
	EXPECT_EQ(93200000u, tick->actiontime());
	EXPECT_DOUBLE_EQ(11.0, tick->price());
	tick->release();
	EXPECT_EQ(1u, loader.calls);
}

TEST_F(R103LastTickBoundaryTest, AfterTailReturnsTheDocumentedEmptyValue)
{
	TickLoader loader;
	loader.ticks = make_ticks();
	HisDataReplayer replayer;
	r91::Sink sink;
	ASSERT_TRUE(init(202001020935ull, loader, replayer, sink));

	EXPECT_EQ(nullptr, replayer.get_last_tick(kCode));
	EXPECT_EQ(1u, loader.calls);
}
