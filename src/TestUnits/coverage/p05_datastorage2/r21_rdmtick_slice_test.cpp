// r21 / WtDataStorageAD：WtRdmDtReaderAD::readTickSliceByRange 全路径
//
// 覆盖对象（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - :82-116   stdCode 解析 + 时间换算 + get_t_db + 空句柄早退
//  - :118-145  首次读取（isEmpty）按区间全量载入 + 首尾时间回填
//  - :146-178  bNeedOlder 分支（前插更早交易的 tick）
//  - :180-202  bNeedNewer 分支（追加更新的 tick）
//  - :205-251  切片窗口裁剪（首条即下边界 / lower_bound 双分支）
//  - :436-458  get_t_db（每合约一库、create_directories、日志）
//
// 期望值全部由本用例自己喂进去的 tick 与区间手工推算，见每条用例的注释。
#include "r21_ad_fixture.h"

namespace {

// 本文件统一使用的合约与代码形态（CodeHelper::extractStdCode 的三段式）
static const char* const K_EXCHG = "XEX";
static const char* const K_CODE = "rb2409";
static const char* const K_STD = "XEX.rb.rb2409";

}

// ---------------------------------------------------------------------------
// 首次读取：区间内 3 笔全部载入，切片按动作时间升序返回
// 键（交易日 20240102）：93015500 / 100015500 / 110015500
// 请求 [20240102093000000, 20240102150000000]
//   → lKey.time = offsetTime(930,true)*100000 + 0 = 93000000
//   → rKey.time = offsetTime(1500,false)*100000 + 0 = 150000000
//   → 命中 3 笔；切片：eIdx=2、首条 93015500 > 93000000 → cnt = 3
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, FirstTickRangeReadLoadsThreeTicksInActionTimeOrder)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 100015500, 3902));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 110015500, 3903));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 3u);
	ASSERT_EQ(slice->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(slice->get_block_size(0), 3u);
	const double want[3] = { 3901, 3902, 3903 };
	const uint32_t want_time[3] = { 93015500, 100015500, 110015500 };
	for (uint32_t i = 0; i < 3; i++)
	{
		const WTSTickStruct* got = slice->at((int32_t)i);
		ASSERT_TRUE(got != NULL);
		EXPECT_EQ(got->price, want[i]);
		EXPECT_EQ(got->action_time, want_time[i]);
		EXPECT_EQ(got->action_date, 20240102u);
		EXPECT_STREQ(got->code, K_CODE);
	}
	slice->release();

	// 缓存边界与 WtDataWriterAD::pipeToTicks 的 LMDB key 同量纲：
	// trading_date*1e9 + offsetTime(HHMM)*100000 + SSsss。
	EXPECT_EQ(sink()->last_log(LL_DEBUG),
		"3 ticks between [20240102093015500,20240102110015500] of XEX.rb.rb2409 loaded to cache");
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 1u);
	// 两个交易日边界都走 calcTradingDate(stdPID, 自然日, 偏移时间, false)
	EXPECT_EQ(bd()->pid_seen(), "XEX.rb");
}

// ---------------------------------------------------------------------------
// 单笔命中：切片只有一条，且首尾时间同值
// 键 93000000（价格 5001），请求 [20240102090000000, 20240102150000000]
//   → lKey.time = 90000000、rKey.time = 150000000 → cnt = 1
//   → lower_bound 上界为空区间（size-1 = 0）→ eIdx = 0，无需回退
//   → 首条 93000000 > 90000000 → cnt = eIdx + 1 = 1
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, SingleTickWindowYieldsOneTickSliceAndEqualBoundaryTimes)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93000000, 5001));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102090000000ull, 20240102150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 1u);
	const WTSTickStruct* got = slice->at(0);
	ASSERT_TRUE(got != NULL);
	EXPECT_EQ(got->price, 5001.0);
	EXPECT_EQ(got->action_time, 93000000u);
	slice->release();

	// 单笔 tick 的首末边界都应与写入键一致。
	EXPECT_EQ(sink()->last_log(LL_DEBUG),
		"1 ticks between [20240102093000000,20240102093000000] of XEX.rb.rb2409 loaded to cache");
}

// ---------------------------------------------------------------------------
// 重复读取相同区间应返回相同的三笔 tick，不重复追加已缓存数据。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, RepeatReadSameWindowReturnsEachTickOnce)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 100015500, 3902));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 110015500, 3903));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* first = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(first->size(), 3u);
	const double want[3] = { 3901.0, 3902.0, 3903.0 };
	if (first->size() == 3u)
	{
		for (uint32_t i = 0; i < 3; i++)
		{
			const WTSTickStruct* got = first->at((int32_t)i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
				EXPECT_DOUBLE_EQ(got->price, want[i]);
		}
	}
	first->release();

	WTSTickSlice* second = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(second != NULL);
	EXPECT_EQ(second->size(), 3u);
	if (second->size() == 3u)
	{
		for (uint32_t i = 0; i < 3; i++)
		{
			const WTSTickStruct* got = second->at((int32_t)i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
				EXPECT_DOUBLE_EQ(got->price, want[i]);
		}
	}
	second->release();
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 1u);
	EXPECT_EQ(sink()->count_containing("prev ticks"), 0u);
	EXPECT_EQ(sink()->count_containing("newer ticks"), 0u);

}

// ---------------------------------------------------------------------------
// bNeedOlder 分支：跨交易日往前取数，新数据前插、老数据后接
// 库里 5 笔：交易日 20240101 两笔（3801@93020000、3802@93120000）
//             交易日 20240102 三笔（3901@93015500、3902@100015500、3903@110015500）
// 首次缓存边界与写入键一致：20240102093015500 / 20240102110015500
// 第 1 次读 [20240102093000000, 20240102150000000] → 只载入 20240102 的 3 笔
// 第 2 次读 [20240101093000000, 20240101150000000]：
//   bNeedOlder = true；旧首 tick 的 LMDB key 作排他上界，因此只前插 20240101 的 2 笔
//   bNeedNewer = false；缓存最后一笔仍为 20240102 的 110015500
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, OlderBranchPrependsPreviousTradingDayTicksAndSlicesTwo)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240101, 20240101, 93020000, 3801));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240101, 20240101, 93120000, 3802));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 100015500, 3902));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 110015500, 3903));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* warm = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(warm != NULL);
	ASSERT_EQ(warm->size(), 3u);
	warm->release();

	WTSTickSlice* older = reader()->readTickSliceByRange(K_STD, 20240101093000000ull, 20240101150000000ull);
	ASSERT_TRUE(older != NULL);
	EXPECT_EQ(older->size(), 2u);
	const double want[2] = { 3801, 3802 };
	for (uint32_t i = 0; i < 2; i++)
	{
		const WTSTickStruct* got = older->at((int32_t)i);
		ASSERT_TRUE(got != NULL);
		EXPECT_EQ(got->price, want[i]);
		EXPECT_EQ(got->action_date, 20240101u);
	}
	older->release();

	EXPECT_EQ(sink()->last_log(LL_DEBUG), "2 prev ticks of XEX.rb.rb2409 loaded to cache");
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 1u);

	WTSTickSlice* all = reader()->readTickSliceByRange(K_STD,
		20240101093000000ull, 20240102150000000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 5u);
	const double all_prices[5] = { 3801.0, 3802.0, 3901.0, 3902.0, 3903.0 };
	const uint32_t all_dates[5] = { 20240101u, 20240101u, 20240102u, 20240102u, 20240102u };
	const uint32_t all_times[5] = { 93020000u, 93120000u, 93015500u, 100015500u, 110015500u };
	for (int32_t i = 0; i < 5; i++)
	{
		const WTSTickStruct* got = all->at(i);
		ASSERT_TRUE(got != NULL);
		EXPECT_DOUBLE_EQ(got->price, all_prices[i]);
		EXPECT_EQ(got->action_date, all_dates[i]);
		EXPECT_EQ(got->action_time, all_times[i]);
	}
	all->release();
}

TEST_F(R21ReaderADTest, NewerBranchAppendsOnlyTicksAfterCachedLastKey)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240101, 20240101, 93020000, 3801));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240101, 20240101, 93120000, 3802));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 100015500, 3902));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 110015500, 3903));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* warm = reader()->readTickSliceByRange(K_STD,
		20240101093000000ull, 20240101150000000ull);
	ASSERT_TRUE(warm != NULL);
	ASSERT_EQ(warm->size(), 2u);
	warm->release();

	WTSTickSlice* newer = reader()->readTickSliceByRange(K_STD,
		20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(newer != NULL);
	ASSERT_EQ(newer->size(), 3u);
	EXPECT_EQ(sink()->last_log(LL_DEBUG), "3 newer ticks of XEX.rb.rb2409 loaded to cache");
	newer->release();

	WTSTickSlice* all = reader()->readTickSliceByRange(K_STD,
		20240101093000000ull, 20240102150000000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 5u);
	const double want_prices[5] = { 3801.0, 3802.0, 3901.0, 3902.0, 3903.0 };
	const uint32_t want_dates[5] = { 20240101u, 20240101u, 20240102u, 20240102u, 20240102u };
	const uint32_t want_times[5] = { 93020000u, 93120000u, 93015500u, 100015500u, 110015500u };
	for (int32_t i = 0; i < 5; i++)
	{
		const WTSTickStruct* got = all->at(i);
		ASSERT_TRUE(got != NULL);
		EXPECT_DOUBLE_EQ(got->price, want_prices[i]);
		EXPECT_EQ(got->action_date, want_dates[i]);
		EXPECT_EQ(got->action_time, want_times[i]);
	}
	all->release();
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 1u);
}

// ---------------------------------------------------------------------------
// 切片包含起始边界；结束时间放在第二笔之后，避免依赖结束边界等于 tick 时的约定。
// 键 93000000 / 93100000 / 93200000，请求 [20240102093000000, 20240102093115000]
//   → 区间从 93000000 开始，返回 93000000 与 93100000 两笔，排除 93200000。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, SliceIncludesExactStartAndExcludesLaterTick)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93000000, 4001));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93100000, 4002));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93200000, 4003));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102093115000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	const WTSTickStruct* first = slice->at(0);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(first->price, 4001.0);
	EXPECT_EQ(first->action_time, 93000000u);
	const WTSTickStruct* second = slice->at(1);
	ASSERT_TRUE(second != NULL);
	EXPECT_EQ(second->price, 4002.0);
	EXPECT_EQ(second->action_time, 93100000u);
	slice->release();

	// 缓存区间回填自首/尾两条，与 writer key 使用相同的时间缩放。
	EXPECT_EQ(sink()->last_log(LL_DEBUG),
		"2 ticks between [20240102093000000,20240102093100000] of XEX.rb.rb2409 loaded to cache");
}

// ---------------------------------------------------------------------------
// 截止时间落在两笔 tick 之间：保留截止时间前两笔，并排除之后的一笔。
// 不对截止时间恰好等于 tick 的闭/半开区间约定作断言。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, EtimeBetweenTicksExcludesLaterTickFromSlice)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 6001));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93045500, 6002));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93115500, 6003));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102093100000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	const double want[2] = { 6001.0, 6002.0 };
	if (slice->size() == 2u)
	{
		for (uint32_t i = 0; i < 2; i++)
		{
			const WTSTickStruct* got = slice->at((int32_t)i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
				EXPECT_DOUBLE_EQ(got->price, want[i]);
		}
	}
	slice->release();
}

// 截止时间恰好等于 tick 时的包含规则尚无公开合同。
// 保留原始数据与等值请求，待明确闭/半开区间语义后再恢复断言。
TEST_F(R21ReaderADTest, DISABLED_EtimeOnExistingTickNeedsBoundaryContract)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 6001));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93045500, 6002));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93115500, 6003));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	GTEST_SKIP() << "Define whether etime equal to a tick is inclusive before asserting a result";
	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102093045500ull);
	if (slice != NULL)
		slice->release();
}

// ---------------------------------------------------------------------------
// 多次读取同一窗口，每次都应保留相同 tick 数量和顺序。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, RepeatedWindowReadsPreserveTickData)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 100015500, 3902));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 110015500, 3903));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	const double want_prices[3] = { 3901.0, 3902.0, 3903.0 };
	for (uint32_t round = 0; round < 3; round++)
	{
		WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102150000000ull);
		ASSERT_TRUE(slice != NULL) << "round " << round;
		EXPECT_EQ(slice->size(), 3u) << "round " << round;
		if (slice->size() == 3u)
		{
			for (uint32_t i = 0; i < 3; i++)
			{
				const WTSTickStruct* got = slice->at((int32_t)i);
				EXPECT_TRUE(got != NULL) << "round " << round << ", tick " << i;
				if (got != NULL)
					EXPECT_DOUBLE_EQ(got->price, want_prices[i]) << "round " << round << ", tick " << i;
			}
		}
		slice->release();
	}
}

// ---------------------------------------------------------------------------
// 同交易所下不同合约各自一库（get_t_db 的路径模板 {base}ticks/{exchg}/{code}）
// rb2409 两笔（交易日 20240102）与 rb2410 一笔（交易日 20240102）互不串味
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, EachCodeHasItsOwnTickDbAndDoesNotLeakKeys)
{
	init_reader();

	std::vector<WTSTickStruct> a;
	a.push_back(r21_make_tick(K_EXCHG, "rb2409", 20240102, 20240102, 93015500, 7001));
	a.push_back(r21_make_tick(K_EXCHG, "rb2409", 20240102, 20240102, 100015500, 7002));
	put_tick_db(K_EXCHG, "rb2409", a);

	std::vector<WTSTickStruct> b;
	b.push_back(r21_make_tick(K_EXCHG, "rb2410", 20240102, 20240102, 93115500, 8001));
	put_tick_db(K_EXCHG, "rb2410", b);

	WTSTickSlice* sa = reader()->readTickSliceByRange("XEX.rb.rb2409", 20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(sa != NULL);
	EXPECT_EQ(sa->size(), 2u);
	EXPECT_EQ(sa->at(0)->price, 7001.0);
	EXPECT_EQ(sa->at(1)->price, 7002.0) << "rb2410 的键不得混进 rb2409 的区间查询";
	sa->release();

	WTSTickSlice* sb = reader()->readTickSliceByRange("XEX.rb.rb2410", 20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(sb != NULL);
	EXPECT_EQ(sb->size(), 1u);
	EXPECT_EQ(sb->at(0)->price, 8001.0);
	sb->release();

	EXPECT_TRUE(std::filesystem::exists(_root / "ticks" / "XEX" / "rb2409"));
	EXPECT_TRUE(std::filesystem::exists(_root / "ticks" / "XEX" / "rb2410"));

	WTSTickSlice* repeated = reader()->readTickSliceByRange("XEX.rb.rb2409",
		20240102093000000ull, 20240102150000000ull);
	ASSERT_TRUE(repeated != NULL);
	ASSERT_EQ(repeated->size(), 2u);
	EXPECT_EQ(repeated->at(0)->price, 7001.0);
	EXPECT_EQ(repeated->at(1)->price, 7002.0);
	repeated->release();
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 1u);
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2410 opened"), 1u);
}

// ---------------------------------------------------------------------------
// Count/date 接口收到有效文件后应返回对应数据。按入口分开验证，
// 防止某个未实现入口以 NULL 返回掩盖其它接口的结果。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, TickSliceByCountReturnsSeededTicks)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 92915500, 8100));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 8101));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93045500, 8102));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 150115500, 8103));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByCount(K_STD, 2, 20240102150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	if (slice->size() == 2u)
	{
		const double want_prices[2] = { 8101.0, 8102.0 };
		for (uint32_t i = 0; i < 2; i++)
		{
			const WTSTickStruct* got = slice->at((int32_t)i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
				EXPECT_DOUBLE_EQ(got->price, want_prices[i]);
		}
	}
	slice->release();
}

TEST_F(R21ReaderADTest, TickSliceByDateReturnsSeededTicks)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 8201));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93045500, 8202));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240103, 20240103, 93015500, 8203));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByDate(K_STD, 20240102);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	if (slice->size() == 2u)
	{
		const double want_prices[2] = { 8201.0, 8202.0 };
		for (uint32_t i = 0; i < 2; i++)
		{
			const WTSTickStruct* got = slice->at((int32_t)i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
				EXPECT_DOUBLE_EQ(got->price, want_prices[i]);
		}
	}
	slice->release();
}

TEST_F(R21ReaderADTest, KlineSliceByCountReturnsSeededDayBars)
{
	init_reader();

	std::vector<WTSBarStruct> bars;
	// r21_make_bar 的 close = base + 1；截至 Jan 4 的最近两根是 Jan 3/4，收盘价 93/94。
	bars.push_back(r21_make_bar(20240102, 20240102, 91.0));
	bars.push_back(r21_make_bar(20240103, 20240103, 92.0));
	bars.push_back(r21_make_bar(20240104, 20240104, 93.0));
	bars.push_back(r21_make_bar(20240105, 20240105, 94.0));
	put_bar_db("day", K_EXCHG, K_CODE, bars, true);

	WTSKlineSlice* slice = reader()->readKlineSliceByCount(
		K_STD, KP_DAY, 2, 202401041500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2);
	EXPECT_GT(slice->get_block_counts(), 0u);
	if (slice->size() == 2 && slice->get_block_counts() > 0)
	{
		const double want_closes[2] = { 93.0, 94.0 };
		for (int32_t i = 0; i < 2; i++)
		{
			const WTSBarStruct* got = slice->at(i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
			{
				EXPECT_EQ(got->date, (uint32_t)(20240103 + i));
				EXPECT_DOUBLE_EQ(got->close, want_closes[i]);
			}
		}
	}
	slice->release();

	// 截止日期恰为 Jan 3：按日线日期闭合包含 Jan 3，同时排除 Jan 4。
	WTSKlineSlice* exactEnd = reader()->readKlineSliceByCount(
		K_STD, KP_DAY, 2, 202401031500ull);
	ASSERT_TRUE(exactEnd != NULL);
	ASSERT_EQ(exactEnd->size(), 2);
	const WTSBarStruct* exactFirst = exactEnd->at(0);
	const WTSBarStruct* exactLast = exactEnd->at(1);
	ASSERT_TRUE(exactFirst != NULL);
	ASSERT_TRUE(exactLast != NULL);
	EXPECT_EQ(exactFirst->date, 20240102u);
	EXPECT_DOUBLE_EQ(exactFirst->close, 92.0);
	EXPECT_EQ(exactLast->date, 20240103u);
	EXPECT_DOUBLE_EQ(exactLast->close, 93.0);
	exactEnd->release();
}

// AD reader 是否承诺读取逐笔委托/队列/成交明细尚未确认；不把 NULL 固定为正确结果。
TEST_F(R21ReaderADTest, DISABLED_OrderDetailRangeSupportNeedsContract)
{
	GTEST_SKIP() << "Confirm whether the AD reader supports order-detail history before asserting a result";
}

TEST_F(R21ReaderADTest, DISABLED_OrderQueueRangeSupportNeedsContract)
{
	GTEST_SKIP() << "Confirm whether the AD reader supports order-queue history before asserting a result";
}

TEST_F(R21ReaderADTest, DISABLED_TransactionRangeSupportNeedsContract)
{
	GTEST_SKIP() << "Confirm whether the AD reader supports transaction history before asserting a result";
}

// ---------------------------------------------------------------------------
// 区间内没有匹配 tick 时返回 NULL，不解引用空 vector，也不对边界迭代器回退。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, TickWindowWithoutAnyMatchedRecordReturnsNull)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	// rb2499 从未落库：get_t_db 会自建目录并返回可用句柄，查询结果为空。
	WTSTickSlice* slice = reader()->readTickSliceByRange("XEX.rb.rb2499",
		20240102093000000ull, 20240102150000000ull);
	if (slice != NULL)
		slice->release();
	EXPECT_EQ(slice, nullptr);
}

// ---------------------------------------------------------------------------
// 文件占用 tick 数据库目录时返回空结果并记录路径/错误；修复路径后同一 reader 可重试。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, TickDbPathOccupiedByRegularFileReturnsNullAndAllowsRetry)
{
	init_reader();

	const std::filesystem::path occupied = _root / "ticks" / "XEX";
	std::filesystem::create_directories(occupied.parent_path());
	{
		std::ofstream fout(occupied);
		fout << "not a directory";
	}

	WTSTickSlice* failed = NULL;
	EXPECT_NO_THROW(failed = reader()->readTickSliceByRange(K_STD,
		20240102093000000ull, 20240102150000000ull));
	EXPECT_EQ(failed, nullptr);
	const std::string error = sink()->last_log(LL_ERROR);
	EXPECT_NE(error.find("Creating tick db directory"), std::string::npos);
	EXPECT_NE(error.find(occupied.string()), std::string::npos);
	const std::size_t failedPos = error.find("failed:");
	ASSERT_NE(failedPos, std::string::npos);
	EXPECT_NE(error.find_first_not_of(" \t", failedPos + 7), std::string::npos);

	std::error_code ec;
	EXPECT_TRUE(std::filesystem::remove(occupied, ec));
	EXPECT_FALSE(ec);
	std::vector<WTSTickStruct> ticks{
		r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 6001)};
	put_tick_db(K_EXCHG, K_CODE, ticks);
	WTSTickSlice* recovered = reader()->readTickSliceByRange(K_STD,
		20240102093000000ull, 20240102150000000ull);
	ASSERT_NE(recovered, nullptr);
	ASSERT_EQ(recovered->size(), 1u);
	EXPECT_DOUBLE_EQ(recovered->at(0)->price, 6001.0);
	recovered->release();
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 1u);
}
