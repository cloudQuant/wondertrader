// r21 / WtDataStorageAD：WtRdmDtReaderAD::readKlineSliceByRange 与 get_k_db
//
// 覆盖对象（src/WtDataStorageAD/WtRdmDtReaderAD.cpp）：
//  - :266-295   stdCode/时间换算 + get_k_db + 空句柄早退
//  - :296-316   首次全量载入（哨兵键 [0, 0xffffffff]）
//  - :317-338   bNeedNewer 分支（_last_bar_time 语义，见 R21-BUG-07）
//  - :340-391   切片窗口裁剪（日线按 date、分钟线按 time）
//  - :393-434   get_k_db 三分支路由 + 未知周期 + std::move 掏空缓存
//
// bar.time 的分钟线语义与写入侧一致：(自然日-19900000)*10000 + HHMM
// （TimeUtils::timeToMinBar，见 R4 manifest 的 m1 键 3301030931）。
#include "r21_ad_fixture.h"

namespace {

static const char* const K2_EXCHG = "XEX";
static const char* const K2_CODE = "rb2409";
static const char* const K2_STD = "XEX.rb.rb2409";

// 四根日线：date == time（与 R4-BUG-13 的落库形态一致），close = 100 + i
[[maybe_unused]] static std::vector<WTSBarStruct> four_day_bars(void)
{
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 4; i++)
	{
		const uint32_t dt = 20240102 + i;
		bars.push_back(r21_make_bar(dt, dt, 100 + i));
	}
	return bars;
}

[[maybe_unused]] static std::vector<WTSBarStruct> three_min1_bars(void)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r21_make_bar(20230103, r21_min_bar_time(20230103, 931), 101));
	bars.push_back(r21_make_bar(20230103, r21_min_bar_time(20230103, 932), 102));
	bars.push_back(r21_make_bar(20230103, r21_min_bar_time(20230103, 933), 103));
	return bars;
}

}

// ---------------------------------------------------------------------------
// 日线首读：全量载入 4 根，请求 [20240102, 20240104] → 切片 3 根
//   eBar.date = etime/10000 = 20240104 → lower_bound(前 3 个元素) 落在下标 2，
//   pBar->date == eBar.date 不回退；
//   sBar.date = 20240102 == 首根 date → 走 :376 第二次 lower_bound → sIdx = 0
//   → cnt = 2 - 0 + 1 = 3
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, DayKlineFirstReadLoadsAllBarsAndClipsToRequestedWindow)
{
	init_reader();
	put_bar_db("day", K2_EXCHG, K2_CODE, four_day_bars(), true);

	WTSKlineSlice* slice = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401041500ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_counts(), (std::size_t)1);
	EXPECT_EQ(slice->get_block_size(0), 3u);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_TRUE(out != NULL);
	const uint32_t want_date[3] = { 20240102, 20240103, 20240104 };
	for (uint32_t i = 0; i < 3; i++)
	{
		EXPECT_EQ(out[i].date, want_date[i]);
		// make_bar: close = base + 1.0，base = 100 + i
		EXPECT_NEAR(out[i].close, 101.0 + i, 1e-9);
	}
	slice->release();

	// WtRdmDtReaderAD.cpp:298 的格式串取 PERIOD_NAME[period]，而 PERIOD_NAME
	// （WTSTypes.h:115-123）里 KP_DAY 位是 "day" 不是 "d1" ⇒ 期望按产品原文修正
	EXPECT_EQ(sink()->last_log(LL_DEBUG), "Reading back day bars of XEX.rb2409...");
	EXPECT_EQ(sink()->count_containing("day db of XEX opened"), 1u);
	// endTDate/beginTDate 各调一次 calcTradingDate
	EXPECT_EQ(bd()->tdate_calls(), 2u);
}

// ---------------------------------------------------------------------------
// 请求下边界早于全部缓存 bar：走 :369-373（pHead 直接取首根、cnt = idx + 1）
//   bar date 20240103/04/05，请求 [20240102, 20240105]
//   eBar.date = 20240105 → lower_bound(前 3 个元素 [0,3)) 落在下标 2 → 不回退
//   首根 20240103 > sBar.date 20240102 → cnt = 3
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, DayKlineWindowBeforeFirstBarReturnsWholeLoadedRange)
{
	init_reader();

	std::vector<WTSBarStruct> bars;
	bars.push_back(r21_make_bar(20240103, 20240103, 200));
	bars.push_back(r21_make_bar(20240104, 20240104, 201));
	bars.push_back(r21_make_bar(20240105, 20240105, 202));
	put_bar_db("day", K2_EXCHG, K2_CODE, bars, true);

	WTSKlineSlice* slice = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401051500ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_size(0), 3u);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_TRUE(out != NULL);
	EXPECT_EQ(out[0].date, 20240103u);
	EXPECT_EQ(out[2].date, 20240105u);
	slice->release();
}

// ---------------------------------------------------------------------------
// 第三次取同交易所同周期的库：命中分支 :417 `return std::move(it->second)`
// 把缓存项掏空 → 第 3 次拿到空句柄 → :293-294 静默返回 NULL
//   第 1 次：miss → open + 存键 "XEX" → 有效句柄 → 切片 3 根
//   第 2 次：find("XEX") 命中 → 搬走缓存项 → 仍有效 → 切片 3 根（开环次数仍为 1）
//   第 3 次：find("XEX") 命中但值为空 → 返回 NULL
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, ThirdKlineLookupOfSameExchangeReturnsNullAfterCacheHandleMovedOut)
{
	init_reader();
	put_bar_db("day", K2_EXCHG, K2_CODE, four_day_bars(), true);

	WTSKlineSlice* first = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401041500ull);
	ASSERT_TRUE(first != NULL);
	ASSERT_EQ(first->get_block_size(0), 3u);
	first->release();

	WTSKlineSlice* second = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401041500ull);
	ASSERT_TRUE(second != NULL);
	EXPECT_EQ(second->get_block_size(0), 3u);
	second->release();

	WTSKlineSlice* third = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401041500ull);
	// R21-BUG-05（与 R4-BUG-02 同形）：第三次起静默丢数据，没有任何 LL_ERROR 日志
	EXPECT_EQ(third, nullptr);
	EXPECT_EQ(sink()->count_of(LL_ERROR), 0u);
	EXPECT_EQ(sink()->count_containing("day db of XEX opened"), 1u);
}

// ---------------------------------------------------------------------------
// 同一交易所、不同合约：交易所级共享一个库句柄 → 第 3 个合约直接查不到数
//   rb2409 首读（开环 1 次）、rb2410 次读（命中被掏空前的句柄，仍可用）、
//   rb2411 三读（空句柄 → NULL）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, ThirdCodeOfSameExchangeFailsBecauseOneHandleServesTheWholeExchange)
{
	init_reader();

	std::vector<WTSBarStruct> a;
	a.push_back(r21_make_bar(20240102, 20240102, 300));
	a.push_back(r21_make_bar(20240103, 20240103, 301));
	put_bar_db("day", K2_EXCHG, "rb2409", a, true);

	std::vector<WTSBarStruct> b;
	b.push_back(r21_make_bar(20240102, 20240102, 400));
	put_bar_db("day", K2_EXCHG, "rb2410", b, true);

	std::vector<WTSBarStruct> c;
	c.push_back(r21_make_bar(20240102, 20240102, 500));
	put_bar_db("day", K2_EXCHG, "rb2411", c, true);

	WTSKlineSlice* sa = reader()->readKlineSliceByRange("XEX.rb.rb2409", KP_DAY,
		202401020930ull, 202401031500ull);
	ASSERT_TRUE(sa != NULL);
	EXPECT_EQ(sa->get_block_size(0), 2u);
	sa->release();

	WTSKlineSlice* sb = reader()->readKlineSliceByRange("XEX.rb.rb2410", KP_DAY,
		202401020930ull, 202401021500ull);
	ASSERT_TRUE(sb != NULL);
	// 单根 bar：lower_bound 区间 [0,0) 为空 → idx=0；date == eBar.date 不回退；
	// 首根 date 20240102 == sBar.date → :376 分支 → sIdx = 0 → cnt = 1
	EXPECT_EQ(sb->get_block_size(0), 1u);
	const WTSBarStruct* out_b = (const WTSBarStruct*)sb->get_block_addr(0);
	ASSERT_TRUE(out_b != NULL);
	EXPECT_NEAR(out_b[0].close, 401.0, 1e-9);
	sb->release();

	WTSKlineSlice* sc = reader()->readKlineSliceByRange("XEX.rb.rb2411", KP_DAY,
		202401020930ull, 202401021500ull);
	EXPECT_EQ(sc, nullptr);
	EXPECT_EQ(sink()->count_containing("day db of XEX opened"), 1u);
}

// ---------------------------------------------------------------------------
// 1 分钟线：键尾是 bar.time（(日-19900000)*10000+HHMM），切片按 time 比较
//   bar time 3301030931/32/33，请求 [202301030931, 202301030932]
//   eBar.time = (20230103-19900000)*10000 + 932 = 3301030932 → lower_bound 落在下标 1
//   首根 3301030931 == sBar.time → :376 分支 → sIdx = 0 → cnt = 2
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, Min1KlineKeysByBarTimeAndSlicesTwoBars)
{
	init_reader();
	put_bar_db("min1", K2_EXCHG, K2_CODE, three_min1_bars(), false);

	WTSKlineSlice* slice = reader()->readKlineSliceByRange(K2_STD, KP_Minute1,
		202301030931ull, 202301030932ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->get_block_size(0), 2u);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_TRUE(out != NULL);
	EXPECT_EQ(out[0].time, r21_min_bar_time(20230103, 931));
	EXPECT_EQ(out[1].time, r21_min_bar_time(20230103, 932));
	EXPECT_NEAR(out[1].close, 103.0, 1e-9);
	slice->release();

	// 同上：PERIOD_NAME[KP_Minute1] == "min1"（WTSTypes.h:115-123），不是 "m1"
	EXPECT_EQ(sink()->last_log(LL_DEBUG), "Reading back min1 bars of XEX.rb2409...");
	EXPECT_EQ(sink()->count_containing("min1 db of XEX opened"), 1u);
	// 分钟线的 etime 还要减 19000000（:283），只影响 bNeedNewer 判定
}

// ---------------------------------------------------------------------------
// m1 / m5 / d1 各自一张 map：三条读取互不影响，都命中有效句柄
//   m5 bar time 3301030935/3301030940，请求 [202301030935, 202301030940]
//   → eBar.time = 3301030940 → lower_bound([0,1)) 落在下标 1 → 不回退
//   → 首根 3301030935 == sBar.time → :376 → sIdx=0 → cnt = 2
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, ThreePeriodMapsAreIndependentSoEachFirstLookupOpensOnce)
{
	init_reader();

	std::vector<WTSBarStruct> m5;
	m5.push_back(r21_make_bar(20230103, r21_min_bar_time(20230103, 935), 600));
	m5.push_back(r21_make_bar(20230103, r21_min_bar_time(20230103, 940), 601));
	put_bar_db("min5", K2_EXCHG, K2_CODE, m5, false);
	put_bar_db("min1", K2_EXCHG, K2_CODE, three_min1_bars(), false);
	put_bar_db("day", K2_EXCHG, K2_CODE, four_day_bars(), true);

	WTSKlineSlice* s5 = reader()->readKlineSliceByRange(K2_STD, KP_Minute5,
		202301030935ull, 202301030940ull);
	ASSERT_TRUE(s5 != NULL);
	EXPECT_EQ(s5->get_block_size(0), 2u);
	s5->release();

	WTSKlineSlice* s1 = reader()->readKlineSliceByRange(K2_STD, KP_Minute1,
		202301030931ull, 202301030933ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(s1->get_block_size(0), 3u);
	s1->release();

	WTSKlineSlice* sd = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401051500ull);
	ASSERT_TRUE(sd != NULL);
	// 4 根日线全在 [20240102,20240105]：eBar.date = 20240105 = 末根 date → 不回退
	// 首根 date == sBar.date → :376 → sIdx = 0 → cnt = 3 - 0 + 1 = 4
	EXPECT_EQ(sd->get_block_size(0), 4u);
	sd->release();

	EXPECT_EQ(sink()->count_containing("opened"), 3u);
	EXPECT_EQ(sink()->count_of(LL_ERROR), 0u);
}

// ---------------------------------------------------------------------------
// 周期路由之外的周期（KP_Hour）不建库、不查库，直接返回 NULL（:412-413）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, HourPeriodHasNoRoutedDbAndKlineReadReturnsNull)
{
	init_reader();

	WTSKlineSlice* slice = reader()->readKlineSliceByRange(K2_STD, KP_Hour,
		202401020930ull, 202401051500ull);
	EXPECT_EQ(slice, nullptr);
	// 连 "Reading back" 的日志都在取库之后，故一条 DEBUG 都没有
	EXPECT_EQ(sink()->count_of(LL_DEBUG), 0u);
	EXPECT_FALSE(std::filesystem::exists(_root / "min1"));
}

// ---------------------------------------------------------------------------
// 复读同一天线区间：:289 的 bNeedNewer 依赖 barsList._last_bar_time，
// 而全函数从未给它赋过值（构造后恒为 0）→ 每次都是「增量分支 + 哨兵下界 0」
// 实为全量重载，且 :330 用新条数 resize 覆盖整个 vector（不是追加）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated)
{
	init_reader();
	put_bar_db("day", K2_EXCHG, K2_CODE, four_day_bars(), true);

	WTSKlineSlice* first = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401041500ull);
	ASSERT_TRUE(first != NULL);
	ASSERT_EQ(first->get_block_size(0), 3u);
	first->release();

	WTSKlineSlice* second = reader()->readKlineSliceByRange(K2_STD, KP_DAY,
		202401020930ull, 202401041500ull);
	ASSERT_TRUE(second != NULL);
	// R21-BUG-07：本应「只补 0930 之后新增的 bar」，实测把 4 根全部重读一遍，
	// 结果与第一次相同（条数没变才没被 :330 的 resize 截断）
	EXPECT_EQ(second->get_block_size(0), 3u);
	const WTSBarStruct* out = (const WTSBarStruct*)second->get_block_addr(0);
	ASSERT_TRUE(out != NULL);
	EXPECT_EQ(out[0].date, 20240102u);
	EXPECT_EQ(out[2].date, 20240104u);
	second->release();

	// 两次「Reading back」日志 = 两次全量 get_range；开环只有 1 次
	EXPECT_EQ(sink()->count_containing("Reading back"), 2u);
	EXPECT_EQ(sink()->count_containing("day db of XEX opened"), 1u);
}

// ---------------------------------------------------------------------------
// 不同合约各自一条缓存：stdCode#period 作为 _bars_cache 的键
// rb2409 与 rb2410 在同一 day 库里互不串味（键里有 exchg/code）
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, BarCacheIsKeyedByStdCodeAndPeriodSoCodesDoNotMix)
{
	init_reader();

	std::vector<WTSBarStruct> a;
	a.push_back(r21_make_bar(20240102, 20240102, 700));
	a.push_back(r21_make_bar(20240103, 20240103, 701));
	put_bar_db("day", K2_EXCHG, "rb2409", a, true);

	std::vector<WTSBarStruct> b;
	b.push_back(r21_make_bar(20240102, 20240102, 800));
	b.push_back(r21_make_bar(20240104, 20240104, 801));
	put_bar_db("day", K2_EXCHG, "rb2410", b, true);

	// 第 1 次取库（rb2409）
	WTSKlineSlice* sa = reader()->readKlineSliceByRange("XEX.rb.rb2409", KP_DAY,
		202401020930ull, 202401031500ull);
	ASSERT_TRUE(sa != NULL);
	EXPECT_EQ(sa->get_block_size(0), 2u);
	const WTSBarStruct* outa = (const WTSBarStruct*)sa->get_block_addr(0);
	ASSERT_TRUE(outa != NULL);
	EXPECT_NEAR(outa[1].close, 702.0, 1e-9);
	sa->release();

	// 第 2 次取库（rb2410，同一交易所 → 命中被 move 的缓存项，句柄仍可用）
	WTSKlineSlice* sb = reader()->readKlineSliceByRange("XEX.rb.rb2410", KP_DAY,
		202401020930ull, 202401041500ull);
	ASSERT_TRUE(sb != NULL);
	// 载入 2 根（date 20240102、20240104）：eBar.date = 20240104 = 末根 → 不回退；
	// 首根 date == sBar.date → :376 → sIdx = 0 → cnt = 1 - 0 + 1 = 2
	EXPECT_EQ(sb->get_block_size(0), 2u);
	const WTSBarStruct* outb = (const WTSBarStruct*)sb->get_block_addr(0);
	ASSERT_TRUE(outb != NULL);
	EXPECT_EQ(outb[0].date, 20240102u);
	EXPECT_NEAR(outb[1].close, 802.0, 1e-9);
	sb->release();
}

// ---------------------------------------------------------------------------
// 缺陷用例（禁用，见 bugs/R21_adstorage.md R21-BUG-06）：
// 库里没有该合约的 bar 时 :296 载入 0 根，:353 仍对空 vector 取 &v[0] 并把
// 上界写成 size()-1（0 → 下溢）→ 空指针解引用，崩在读路径上。
// 触发条件：{base}day/{exchg}/ 存在且可读，但里面没有目标合约的任何键，
//          然后 readKlineSliceByRange(该合约, KP_DAY, ...)。
// ---------------------------------------------------------------------------
TEST_F(R21ReaderADTest, DISABLED_KlineReadWithoutAnyBarCrashesOnEmptyVectorSubscript)
{
	init_reader();

	std::vector<WTSBarStruct> a;
	a.push_back(r21_make_bar(20240102, 20240102, 700));
	put_bar_db("day", K2_EXCHG, "rb2409", a, true);

	// rb2499 在该库里一条都没有 → cnt=0 → _bars 为空 → :353 崩
	WTSKlineSlice* slice = reader()->readKlineSliceByRange("XEX.rb.rb2499", KP_DAY,
		202401020930ull, 202401051500ull);
	if (slice != NULL)
		slice->release();
	EXPECT_EQ(slice, nullptr);
}
