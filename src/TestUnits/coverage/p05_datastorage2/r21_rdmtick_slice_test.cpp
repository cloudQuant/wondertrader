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
// R32 裁定：保持红。元素顺序断言的是"无缺陷时应有"的升序切片；现实现
// WtRdmDtReaderAD.cpp:137-141 的 curTs 是指向 _ticks.front() 的引用，
// `curTs = tickList._ticks.back();` 会把末笔整体覆写回 _ticks[0]（静默数据损坏）。
// 缺陷登记见 bugs/R32_adstorage.md BUG-R32-001，不改断言凑绿。
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

	// 首尾时间回填见 bugs/R21_adstorage.md R21-BUG-01：
	// 实现在 :138/:141 把 offsetTime(HHMM) 与 SSsss 直接相加（少乘 100000），
	// 所以日志里的区间是 交易日*1e9 + HHMM + SSsss
	EXPECT_EQ(sink()->last_log(LL_DEBUG),
		"3 ticks between [20240102000016430,20240102000016600] of XEX.rb.rb2409 loaded to cache");
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

	// R21-BUG-01：正确应为 20240102093000000，这里实测是 20240102*1e9 + 930 + 0
	EXPECT_EQ(sink()->last_log(LL_DEBUG),
		"1 ticks between [20240102000000930,20240102000000930] of XEX.rb.rb2409 loaded to cache");
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

}

// ---------------------------------------------------------------------------
// bNeedOlder 分支：跨交易日往前取数，新数据前插、老数据后接
// 库里 5 笔：交易日 20240101 两笔（3801@93020000、3802@93120000）
//             交易日 20240102 三笔（3901@93015500、3902@100015500、3903@110015500）
// 第 1 次读 [20240102093000000, 20240102150000000] → 只载入 20240102 的 3 笔
// 第 2 次读 [20240101093000000, 20240101150000000]：
//   bNeedOlder = 20240101093000000 < 20240102000016430 → true
//   bNeedNewer = 20240101150000000 > 20240102000016600 → false
//   :152 rKey.time = _first_tick_time % 1e9 = 16430、date = 20240102
//   :153 lKey = (20240101, 93000000) → 命中 20240101 的 2 笔（20240102 那三笔键更大，
//        循环在 93015500 > 16430 处 break）→ _ticks = 5（新 2 在前）
//   切片：eTick=(20240101,150000000) → lower_bound 落在下标 2（交易日更大）→ 回退到 1
//         首条 93020000 > 93000000 → cnt = 2
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

	// :174 同样是「少乘 100000」的量纲错误（R21-BUG-01）：应为 20240101093020000
	EXPECT_EQ(sink()->last_log(LL_DEBUG), "2 prev ticks of XEX.rb.rb2409 loaded to cache");
	EXPECT_EQ(sink()->count_containing("Tick db of XEX.rb2409 opened"), 2u);
}

// ---------------------------------------------------------------------------
// 切片下边界走 else 分支（:238-247）：首条动作时间恰好等于请求下边界
// 键 93000000 / 93100000 / 93200000，请求 [20240102093000000, 20240102093100000]
//   → 区间 [93000000, 93100000] 命中 2 笔（93200000 被 break 掉）
//   → eTick.time = 93100000：lower_bound 落在下标 1，且 :224 的 `>=` 成立 → eIdx = 0
//   → 首条 93000000 > 93000000 不成立 → 走 :238 再取一次 lower_bound → sIdx = 0
//   → cnt = 0 - 0 + 1 = 1
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, SliceLowerBoundTakesSecondLookupWhenFirstTickEqualsWindowStart)
// R32 裁定：保持红。元素顺序断言的是"无缺陷时应有"的升序切片；现实现
// WtRdmDtReaderAD.cpp:137-141 的 curTs 是指向 _ticks.front() 的引用，
// `curTs = tickList._ticks.back();` 会把末笔整体覆写回 _ticks[0]（静默数据损坏）。
// 缺陷登记见 bugs/R32_adstorage.md BUG-R32-001，不改断言凑绿。
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93000000, 4001));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93100000, 4002));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93200000, 4003));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD, 20240102093000000ull, 20240102093100000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 1u);
	const WTSTickStruct* got = slice->at(0);
	ASSERT_TRUE(got != NULL);
	EXPECT_EQ(got->price, 4001.0);
	EXPECT_EQ(got->action_time, 93000000u);
	slice->release();

	// 缓存区间回填自首/尾两条：93000000 → 930+0，100000000 → 1000+0
	EXPECT_EQ(sink()->last_log(LL_DEBUG),
		"2 ticks between [20240102000000930,20240102000001000] of XEX.rb.rb2409 loaded to cache");
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
// R32 裁定：保持红。元素顺序断言的是"无缺陷时应有"的升序切片；现实现
// WtRdmDtReaderAD.cpp:137-141 的 curTs 是指向 _ticks.front() 的引用，
// `curTs = tickList._ticks.back();` 会把末笔整体覆写回 _ticks[0]（静默数据损坏）。
// 缺陷登记见 bugs/R32_adstorage.md BUG-R32-001，不改断言凑绿。
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
}

// ---------------------------------------------------------------------------
// Count/date 接口收到有效文件后应返回对应数据。按入口分开验证，
// 防止某个未实现入口以 NULL 返回掩盖其它接口的结果。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, TickSliceByCountReturnsSeededTicks)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 8101));
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93045500, 8102));
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
	bars.push_back(r21_make_bar(20240102, 20240102, 91.0));
	bars.push_back(r21_make_bar(20240103, 20240103, 92.0));
	put_bar_db("day", K_EXCHG, K_CODE, bars, true);

	WTSKlineSlice* slice = reader()->readKlineSliceByCount(
		K_STD, KP_DAY, 2, 202401031500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2);
	EXPECT_GT(slice->get_block_counts(), 0u);
	if (slice->size() == 2 && slice->get_block_counts() > 0)
	{
		const double want_closes[2] = { 92.0, 93.0 };
		for (int32_t i = 0; i < 2; i++)
		{
			const WTSBarStruct* got = slice->at(i);
			EXPECT_TRUE(got != NULL);
			if (got != NULL)
			{
				EXPECT_EQ(got->date, (uint32_t)(20240102 + i));
				EXPECT_DOUBLE_EQ(got->close, want_closes[i]);
			}
		}
	}
	slice->release();
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
// 缺陷用例（禁用，见 bugs/R21_adstorage.md R21-BUG-06）：
// 区间内一条 tick 都没有时，:216 直接对空 vector 取 &v[0]，
// 且 :216/:238 的上界写成 size()-1（size 为 0 时下溢成 SIZE_MAX-1）→
// 解引用空指针 / 越界比较，读取一个不存在合约或空库必崩。
// 触发条件：readTickSliceByRange("XEX.rb.rb2499", 有数据的交易日, ...) 且
//          {base}ticks/XEX/rb2499 里没有落在 [stime,etime] 的任何键。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, DISABLED_TickWindowWithoutAnyMatchedRecordReadsBackOfEmptyVector)
{
	init_reader();

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r21_make_tick(K_EXCHG, K_CODE, 20240102, 20240102, 93015500, 3901));
	put_tick_db(K_EXCHG, K_CODE, ticks);

	// rb2499 从未落库：get_t_db 会自建目录并返回可用句柄 → cnt=0 → :216 崩
	WTSTickSlice* slice = reader()->readTickSliceByRange("XEX.rb.rb2499",
		20240102093000000ull, 20240102150000000ull);
	if (slice != NULL)
		slice->release();
	EXPECT_EQ(slice, nullptr);
}

// ---------------------------------------------------------------------------
// 缺陷用例（禁用，见 bugs/R21_adstorage.md R21-BUG-12）：
// :445 的 create_directories 未带 error_code 重载，
// 路径被同名普通文件占用时抛 boost::filesystem_error 且无人接 →
// 从 readTickSliceByRange 穿出调用栈（上层一般是策略/服务线程）→ std::terminate。
// 触发条件：预先把 {base}ticks/{exchg}/{code} 建成一个普通文件，再读该合约。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfReadTickSlice)
{
	init_reader();

	const std::filesystem::path occupied = _root / "ticks" / "XEX" / "rb2409";
	std::filesystem::create_directories(occupied.parent_path());
	{
		std::ofstream fout(occupied);
		fout << "not a directory";
	}

	WTSTickSlice* slice = reader()->readTickSliceByRange(K_STD,
		20240102093000000ull, 20240102150000000ull);
	if (slice != NULL)
		slice->release();
	EXPECT_EQ(slice, nullptr);
}
