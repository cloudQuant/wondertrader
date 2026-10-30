// r23_storage4 / WtDataStorage：WtRdmDtReader 的「按条数取数」两条公开入口
//
// 覆盖对象（src/WtDataStorage/WtRdmDtReader.cpp，q03 明确跳过、R1 未做，本文件补上）：
//  - readTickSliceByCount  :2395-2605  RT 块优先取尾 + 逐日回退拼 his/ticks/<exchg>/<date>/<code>.dsb、
//                                      缺文件累计 missingCnt 到 30 天停止、坏文件修复后的重新读取、
//                                      count==0、etime 边界
//  - readKlineSliceByCount :2219-2393  etime==0 → 203012312359、KP_DAY 无 RT 块、
//                                      indexBarFromCacheByCount 尾窗、his+rt 双块拼接
//  - indexBarFromCacheByCount :1688-1727 经公开入口驱动（eit==end 收敛 / curCnt=min 截断）
//  - getRTTickBlock :2002 / getRTKilneBlock :2162（min1 与 KP_DAY 两条 switch 分支）
//
// 手法沿用 q03/R1：WtRdmDtReader::_base_dir 是 private 且只有 init(cfg!=NULL) 才赋值
// （那条路径起 5 秒轮询线程、析构 join），故保持空串 → 生产拼出的路径退化为相对路径，
// 由 q03::Sandbox 把 CWD 切到 temp 目录。**没有访问权限宏**。
// 常规用例使用 q03::BaseDataMgr（SSE 路径不检查 product），因此不能覆盖按真实
// "exchg.product" 查表时 :2401 把合约代码当 product 的问题；严格替身见文件末尾。
#include "../q03_storage3/q03_storage3_fixture.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <gtest/internal/gtest-port.h>

#include "WtDataStorage/WtRdmDtReader.h"

namespace r23 {

using namespace q03;

namespace {

const uint32_t kCurTDate = 20240110;

inline string his_tick_file(uint32_t date, const char* code = "600000")
{
	return fmtutil::format("his/ticks/SSE/{}/{}.dsb", date, code);
}

inline string rt_tick_file(const char* code = "600000")
{
	return fmtutil::format("rt/ticks/SSE/{}.dmb", code);
}

inline string his_kline_file(const char* pname, const char* code = "600000")
{
	return fmtutil::format("his/{}/SSE/{}.dsb", pname, code);
}

inline string rt_kline_file(const char* pname, const char* code = "600000")
{
	return fmtutil::format("rt/{}/SSE/{}.dmb", pname, code);
}

// 交易日内的三段 tick：14:30 / 14:31 / 14:32
inline vector<WTSTickStruct> day_ticks(uint32_t adate, uint32_t base_seq)
{
	vector<WTSTickStruct> ay;
	ay.push_back(make_tick(adate, adate, 143000000, base_seq));
	ay.push_back(make_tick(adate, adate, 143100000, base_seq + 1));
	ay.push_back(make_tick(adate, adate, 143200000, base_seq + 2));
	return ay;
}

inline bool write_his_tick_file(uint32_t date, const vector<WTSTickStruct>& ticks)
{
	const string file = his_tick_file(date);
	return write_bytes(file, make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(ticks)));
}

} // namespace

class R23RdmCountTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r23cnt"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr				_bd;
	HotMgr					_hot;
	RdmSink					_sink{&_bd, &_hot};
	WtRdmDtReader			_reader;
};

// ------------------------------------------------------------------ readTickSliceByCount
TEST_F(R23RdmCountTest, CountWalksBackAcrossDaysUntilSatisfied)
{
	const vector<WTSTickStruct> d08 = day_ticks(20240108, 100);
	const vector<WTSTickStruct> d05 = day_ticks(20240105, 200);
	ASSERT_TRUE(write_his_tick_file(20240108, d08));
	ASSERT_TRUE(write_his_tick_file(20240105, d05));

	// etime 落在没有 RT 块的当前交易日 → RT 分支 break，剩下的全部从 his 逐日回退取
	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 6, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 6u);
	EXPECT_EQ(slice->get_block_counts(), 2u);

	// 后取到的 20240105 被 insertBlock(0,...) 前插 → 时间顺序由旧到新
	const WTSTickStruct* first = slice->at(0);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(first->action_date, 20240105u);
	EXPECT_DOUBLE_EQ(first->price, 30.0 + 200);      // day_ticks(20240105,200)[0]
	const WTSTickStruct* sixth = slice->at(5);
	ASSERT_TRUE(sixth != NULL);
	EXPECT_EQ(sixth->action_date, 20240108u);
	EXPECT_DOUBLE_EQ(sixth->price, 30.0 + 102);      // day_ticks(20240108,100)[2]
	slice->release();
}

TEST_F(R23RdmCountTest, CountStopsAfterThirtyMissingDays)
{
	const vector<WTSTickStruct> d08 = day_ticks(20240108, 100);
	const vector<WTSTickStruct> d07 = day_ticks(20240107, 300);
	ASSERT_TRUE(write_his_tick_file(20240108, d08));
	ASSERT_TRUE(write_his_tick_file(20240107, d07));

	// 要 100 条，实际只有 6 条：20240106 起连续缺文件，缺满 30 天（:2481）就停
	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 100, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 6u) << "缺文件计数上限没有生效（missingCnt>=30）";
	EXPECT_EQ(slice->get_block_counts(), 2u);

	// 最后一条应该是 20240108 的第三条
	const WTSTickStruct* last = slice->at(-1);
	ASSERT_TRUE(last != NULL);
	EXPECT_EQ(last->action_date, 20240108u);
	EXPECT_EQ(last->action_time, 143200000u);
	slice->release();
}

TEST_F(R23RdmCountTest, CountTakesRealtimeTailFirstThenOneHisTick)
{
	// RT 块：当日 3 条（14:30/14:31/14:32），容量 10
	const vector<WTSTickStruct> rt = day_ticks(kCurTDate, 500);
	ASSERT_TRUE(write_bytes(rt_tick_file(), make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 10, rt)));
	// 前一交易日 3 条
	const vector<WTSTickStruct> his = day_ticks(20240109, 100);
	ASSERT_TRUE(write_his_tick_file(20240109, his));

	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 4, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 4u);
	EXPECT_EQ(slice->get_block_counts(), 2u);

	// 缺口 1 条由 20240109 的尾部补齐（eIdx=2 → sIdx=2 → 只取第 3 条）
	const WTSTickStruct* first = slice->at(0);
	ASSERT_TRUE(first != NULL);
	EXPECT_EQ(first->action_date, 20240109u);
	EXPECT_EQ(first->action_time, 143200000u);
	EXPECT_DOUBLE_EQ(first->price, 30.0 + 102);

	const WTSTickStruct* second = slice->at(1);
	ASSERT_TRUE(second != NULL);
	EXPECT_EQ(second->action_date, kCurTDate);
	EXPECT_EQ(second->action_time, 143000000u);
	slice->release();
}

TEST_F(R23RdmCountTest, CountZeroReturnsEmptySliceWithoutTouchingFiles)
{
	const vector<WTSTickStruct> rt = day_ticks(kCurTDate, 500);
	ASSERT_TRUE(write_bytes(rt_tick_file(), make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 10, rt)));

	// count==0：left=0，RT 分支仍会跑一次（while(hasToday){...break}），insertBlock 插 0 条
	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 0, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_TRUE(slice->empty());
	EXPECT_EQ(slice->size(), 0u);
	// insertBlock 在 count==0 时直接 return false（WTSDataDef.hpp:1438-1444）→ 一个块都不挂
	EXPECT_EQ(slice->get_block_counts(), 0u);
	slice->release();
}

TEST_F(R23RdmCountTest, HisWindowBoundaryKeepsTickExactlyAtEtime)
{
	// etime 落在 20240108（< curTDate）→ hasToday 为假，直接走 his 逐日回退
	const vector<WTSTickStruct> d08 = day_ticks(20240108, 100);
	const vector<WTSTickStruct> d07 = day_ticks(20240107, 300);
	ASSERT_TRUE(write_his_tick_file(20240108, d08));
	ASSERT_TRUE(write_his_tick_file(20240107, d07));

	// 截止 14:31，应返回当日 14:30 和 14:31，不回补前一日数据。
	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 2, 20240108143100000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	const WTSTickStruct* first = slice->at(0);
	const WTSTickStruct* last = slice->at(-1);
	ASSERT_TRUE(first != NULL);
	ASSERT_TRUE(last != NULL);
	EXPECT_EQ(first->action_date, 20240108u);
	EXPECT_EQ(first->action_time, 143000000u);
	EXPECT_EQ(last->action_date, 20240108u);
	EXPECT_EQ(last->action_time, 143100000u);
	slice->release();

	// 命中该历史块的末条时也保留边界记录，不从前一日补数。
	WTSTickSlice* lastWindow = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240108143200000ull);
	ASSERT_TRUE(lastWindow != NULL);
	EXPECT_EQ(lastWindow->size(), 3u);
	EXPECT_EQ(lastWindow->get_block_counts(), 1u);
	EXPECT_EQ(lastWindow->at(0)->action_time, 143000000u);
	EXPECT_EQ(lastWindow->at(-1)->action_date, 20240108u);
	EXPECT_EQ(lastWindow->at(-1)->action_time, 143200000u);
	lastWindow->release();
}

TEST_F(R23RdmCountTest, RtWindowBoundaryKeepsTickExactlyAtEtime)
{
	// 同样的截止点契约也适用于当日 RT 块：etime 相等的 tick 应保留。
	const vector<WTSTickStruct> rt = day_ticks(kCurTDate, 100);
	ASSERT_TRUE(write_bytes(rt_tick_file(), make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 10, rt)));

	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 2, 20240110143100000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_EQ(slice->get_block_counts(), 1u);
	const WTSTickStruct* first = slice->at(0);
	const WTSTickStruct* last = slice->at(-1);
	ASSERT_TRUE(first != NULL);
	ASSERT_TRUE(last != NULL);
	// lower_bound 命中下标 1，不回落 → 取 [0..1]
	EXPECT_EQ(first->action_time, 143000000u);
	EXPECT_EQ(last->action_time, 143100000u);
	EXPECT_DOUBLE_EQ(last->price, 30.0 + 101);
	slice->release();

	// 边界压到块内最后一条时，RT 路径也应将它算进来。
	WTSTickSlice* whole = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110143200000ull);
	ASSERT_TRUE(whole != NULL);
	EXPECT_EQ(whole->size(), 3u);
	EXPECT_EQ(whole->at(-1)->action_time, 143200000u);
	whole->release();
}

TEST_F(R23RdmCountTest, HisEarlierActionDateWithLaterClockStaysBeforeEtime)
{
	// action_date 早于截止日，因此即使时刻数值更大也应保留末条历史 tick。
	vector<WTSTickStruct> history;
	history.push_back(make_tick(20240109, 20240108, 143000000, 100));
	history.push_back(make_tick(20240109, 20240108, 143200000, 101));
	ASSERT_TRUE(write_his_tick_file(20240109, history));

	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 1, 20240109143100000ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 1u);
	EXPECT_EQ(slice->at(0)->action_date, 20240108u);
	EXPECT_EQ(slice->at(0)->action_time, 143200000u);
	slice->release();
}

TEST_F(R23RdmCountTest, RtEarlierActionDateWithLaterClockStaysBeforeEtime)
{
	// RT 块头属于当前交易日，但 tick 的 action_date 更早；日期优先于时刻比较。
	vector<WTSTickStruct> rt;
	rt.push_back(make_tick(kCurTDate, 20240109, 143000000, 100));
	rt.push_back(make_tick(kCurTDate, 20240109, 143200000, 101));
	ASSERT_TRUE(write_bytes(rt_tick_file(),
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, (uint32_t)rt.size(), (uint32_t)rt.size(), rt)));

	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 1, 20240110143100000ull);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->size(), 1u);
	EXPECT_EQ(slice->at(0)->action_date, 20240109u);
	EXPECT_EQ(slice->at(0)->action_time, 143200000u);
	slice->release();
}

TEST_F(R23RdmCountTest, CountSizeCheckFailureCanRecoverAfterFileRepair)
{
	// 只有 8 字节头 → 小于 sizeof(HisTickBlock) → :2538 长度校验失败
	const string bad = his_tick_file(20240108);
	BlockHeader head;
	std::memset(&head, 0, sizeof(head));
	std::memcpy(head._blk_flag, BLK_FLAG, FLAG_SIZE);
	head._type = BT_HIS_Ticks;
	head._version = BLOCK_VERSION_RAW_V2;
	string bytes((const char*)&head, 8);        // 故意只写 8 字节
	ASSERT_TRUE(write_bytes(bad, bytes));

	WTSTickSlice* s1 = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
	// 短文件首次读取按失败契约返回 NULL 或稳定空切片，不跨过坏文件拼接上一日数据。
	EXPECT_TRUE(s1 == NULL || (s1->empty() && s1->get_block_counts() == 0u));
	EXPECT_EQ(_sink.count_of(LL_ERROR), 1u);
	EXPECT_NE(_sink.last_log().find("Sizechecking of his tick data file"), string::npos);
	if (s1 != NULL)
		s1->release();

	// 修复文件后同一 reader 应重新读取该日，而不是被首次失败留下的缓存条目永久挡住。
	const vector<WTSTickStruct> repaired = day_ticks(20240108, 100);
	ASSERT_TRUE(write_his_tick_file(20240108, repaired));
	const uint32_t err_before = _sink.count_of(LL_ERROR);
	WTSTickSlice* s2 = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->size(), 3u);
	EXPECT_EQ(s2->get_block_counts(), 1u);
	const WTSTickStruct* first = s2->at(0);
	const WTSTickStruct* last = s2->at(-1);
	EXPECT_TRUE(first != NULL);
	EXPECT_TRUE(last != NULL);
	if (first != NULL)
	{
		EXPECT_EQ(first->action_date, 20240108u);
		EXPECT_EQ(first->action_time, 143000000u);
		EXPECT_DOUBLE_EQ(first->price, 130.0);
	}
	if (last != NULL)
	{
		EXPECT_EQ(last->action_date, 20240108u);
		EXPECT_EQ(last->action_time, 143200000u);
		EXPECT_DOUBLE_EQ(last->price, 132.0);
	}
	EXPECT_EQ(_sink.count_of(LL_ERROR), err_before)
		<< "有效修复文件不应继续触发尺寸校验错误";
	s2->release();
}

TEST_F(R23RdmCountTest, CountWithNoDataAtAllReturnsEmptySlice)
{
	// 既没有 RT 块也没有任何 his 文件
	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 5, 20240110150000000ull);
	ASSERT_TRUE(slice != NULL) << ":2604 恒返回切片对象，空结果也不该是 NULL";
	EXPECT_TRUE(slice->empty());
	EXPECT_EQ(slice->get_block_counts(), 0u);
	slice->release();
}

// ------------------------------------------------------------------ readKlineSliceByCount
TEST_F(R23RdmCountTest, KlineCountIsTailWindowOfHisDayBars)
{
	vector<WTSBarStruct> bars;
	const uint32_t dates[] = {20240105, 20240108, 20240109, 20240110};
	for (uint32_t i = 0; i < 4; i++)
		bars.push_back(make_bar(dates[i], 0, i));
	ASSERT_TRUE(write_bytes(his_kline_file("day"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// KP_DAY → getRTKilneBlock 直接返回 NULL（:2164），全部来自 his
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_DAY, 2, 202401102359);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	const WTSBarStruct* first = slice->at(0);
	const WTSBarStruct* last = slice->at(-1);
	ASSERT_TRUE(first != NULL);
	ASSERT_TRUE(last != NULL);
	EXPECT_EQ(first->date, 20240109u);      // 尾窗两条
	EXPECT_EQ(last->date, 20240110u);
	EXPECT_DOUBLE_EQ(last->open, 13.0);      // make_bar(date, 0, idx=3)
	slice->release();

	// count 大于可用条数 → 被 min() 收敛到全部 4 条
	WTSKlineSlice* all = _reader.readKlineSliceByCount("SSE.STK.600000", KP_DAY, 100, 202401102359);
	ASSERT_TRUE(all != NULL);
	EXPECT_EQ(all->size(), 4u);
	EXPECT_EQ(all->at(0)->date, 20240105u);
	all->release();
}

TEST_F(R23RdmCountTest, KlineCountWithZeroEtimeUsesYear2030Sentinel)
{
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240105, 0, 0));
	bars.push_back(make_bar(20240108, 0, 1));
	ASSERT_TRUE(write_bytes(his_kline_file("day"),
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// etime==0 → :2238 置为 203012312359，尾窗落在最后一天
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_DAY, 1, 0);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 1u);
	const WTSBarStruct* only = slice->at(0);
	ASSERT_TRUE(only != NULL);
	EXPECT_EQ(only->date, 20240108u);
	EXPECT_DOUBLE_EQ(only->close, 11.5);
	slice->release();
}

TEST_F(R23RdmCountTest, KlineCountSplicesHisTailWithRtMin1Block)
{
	vector<WTSBarStruct> his;
	// 三条 min1：20240108 的 14:58/14:59/15:00
	his.push_back(make_bar(20240108, TimeUtils::timeToMinBar(20240108, 1458), 0));
	his.push_back(make_bar(20240108, TimeUtils::timeToMinBar(20240108, 1459), 1));
	his.push_back(make_bar(20240108, TimeUtils::timeToMinBar(20240108, 1500), 2));
	ASSERT_TRUE(write_bytes(his_kline_file("min1"),
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(his))));

	// RT 块：当日 3 条，时间在 etime(20240110 15:00) 之前
	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(kCurTDate, TimeUtils::timeToMinBar(kCurTDate, 1450), 10));
	rt.push_back(make_bar(kCurTDate, TimeUtils::timeToMinBar(kCurTDate, 1451), 11));
	rt.push_back(make_bar(kCurTDate, TimeUtils::timeToMinBar(kCurTDate, 1452), 12));
	ASSERT_TRUE(write_bytes(rt_kline_file("min1"),
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 3, 10, rt)));

	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute1, 4, 202401101500);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 4u);
	EXPECT_EQ(slice->get_block_counts(), 2u);
	const WTSBarStruct* first = slice->at(0);
	ASSERT_TRUE(first != NULL);
	// his 只补 1 条（4-3）→ 取 his 的尾条 15:00
	EXPECT_EQ(first->time % 10000, 1500u);
	EXPECT_EQ(slice->at(1)->time % 10000, 1450u);
	EXPECT_EQ(slice->at(3)->time % 10000, 1452u);
	EXPECT_DOUBLE_EQ(slice->at(3)->open, 22.0);   // make_bar(..., seq=12)
	slice->release();

	// count 恰好等于 RT 条数 → 不再回补 his
	WTSKlineSlice* only_rt = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute1, 3, 202401101500);
	ASSERT_TRUE(only_rt != NULL);
	EXPECT_EQ(only_rt->size(), 3u);
	EXPECT_EQ(only_rt->get_block_counts(), 1u);
	EXPECT_EQ(only_rt->at(0)->time % 10000, 1450u);
	only_rt->release();
}

TEST_F(R23RdmCountTest, KlineCountWithNoBarsAnywhereReturnsEmptyResult)
{
	// 有效空历史文件且没有实时 K 线时，不能把请求数当成历史条数。
	vector<WTSBarStruct> none;
	ASSERT_TRUE(write_bytes(his_kline_file("min5"),
		make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(none))));
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute5, 3, 202401101500);
	EXPECT_TRUE(slice == NULL || slice->size() == 0)
		<< "空历史和空实时数据不应返回 count 大于零的切片";
	if (slice != NULL)
		slice->release();

	// 另一周期既没有历史文件也没有实时块，同样应返回空结果。
	WTSKlineSlice* slice2 = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute1, 5, 202401101500);
	EXPECT_TRUE(slice2 == NULL || slice2->size() == 0)
		<< "无历史文件且无实时数据不应返回非空幻影切片";
	if (slice2 != NULL)
		slice2->release();
}

TEST_F(R23RdmCountTest, KlineCountWithEmptyHistoryKeepsRealtimeBars)
{
	// 历史文件合法但没有 bars；较少的实时 bars 仍应完整返回。
	const vector<WTSBarStruct> none;
	ASSERT_TRUE(write_bytes(his_kline_file("min1"),
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(none))));

	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(kCurTDate, TimeUtils::timeToMinBar(kCurTDate, 1450), 10));
	rt.push_back(make_bar(kCurTDate, TimeUtils::timeToMinBar(kCurTDate, 1451), 11));
	ASSERT_TRUE(write_bytes(rt_kline_file("min1"),
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, (uint32_t)rt.size(), (uint32_t)rt.size(), rt)));

	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute1, 5, 202401101500);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	EXPECT_EQ(slice->get_block_counts(), 1u);
	ASSERT_TRUE(slice->at(0) != NULL);
	ASSERT_TRUE(slice->at(-1) != NULL);
	EXPECT_EQ(slice->at(0)->time % 10000, 1450u);
	EXPECT_EQ(slice->at(-1)->time % 10000, 1451u);
	slice->release();
}

// ------------------------------------------------------------------ 崩溃类收口
namespace {

// 只接受真实 WTSBaseDataMgr 的 "exchg.product" 键形态；合约代码不能命中商品表。
class StrictBDMgr : public BaseDataMgr
{
public:
	StrictBDMgr()
	{
		_stk = WTSCommodityInfo::create("STK", "股票", "SSE", "SSE_DAY", "CHN");
		_stk->setSessionInfo(BaseDataMgr::getSession("SSE_DAY"));
		_stk->setCategory(ContractCategory::CC_Stock);
	}

	virtual ~StrictBDMgr() { _stk->release(); }

	void set_session_available(bool available) { _session_available = available; }

	WTSSessionInfo* getSession(const char* sid) override
	{
		return _session_available ? BaseDataMgr::getSession(sid) : NULL;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		if (exchg != NULL && pid != NULL
			&& strcmp(exchg, "SSE") == 0 && strcmp(pid, "STK") == 0)
			return _stk;
		return NULL;
	}

private:
	WTSCommodityInfo*	_stk = NULL;
	bool	_session_available = true;
};

const char* const kStrictProbeDirectoryEnv = "R23_RDM_COUNT_STRICT_BDM_DIR";

inline bool is_threadsafe_death_test_child()
{
	const std::vector<std::string> args = ::testing::internal::GetArgvs();
	const std::string prefix = "--gtest_internal_run_death_test=";
	for (const std::string& arg : args)
	{
		if (arg.compare(0, prefix.size(), prefix) == 0)
			return true;
	}
	return false;
}

class OwnedProbeDirectory
{
public:
	explicit OwnedProbeDirectory(const std::string& path) : _path(path) {}

	~OwnedProbeDirectory()
	{
		if (!_path.empty())
		{
			std::error_code ec;
			std::filesystem::remove_all(_path, ec);
		}
	}

private:
	std::string _path;
};

class ScopedEnvironmentVariable
{
public:
	explicit ScopedEnvironmentVariable(const char* name) : _name(name)
	{
		const char* oldValue = std::getenv(name);
		_hadPreviousValue = oldValue != NULL;
		if (_hadPreviousValue)
			_previousValue = oldValue;
	}

	bool set(const std::string& value)
	{
		if (::setenv(_name.c_str(), value.c_str(), 1) != 0)
			return false;
		_active = true;
		return true;
	}

	~ScopedEnvironmentVariable()
	{
		if (!_active)
			return;
		if (_hadPreviousValue)
			::setenv(_name.c_str(), _previousValue.c_str(), 1);
		else
			::unsetenv(_name.c_str());
	}

private:
	std::string	_name;
	std::string	_previousValue;
	bool		_hadPreviousValue = false;
	bool		_active = false;
};

inline void emit_probe_stage(const char* stage)
{
	std::fprintf(stderr, "%s\n", stage);
	std::fflush(stderr);
}

int run_strict_commodity_probe(const std::string& directory)
{
	struct rlimit coreLimit;
	coreLimit.rlim_cur = 0;
	coreLimit.rlim_max = 0;
	if (::setrlimit(RLIMIT_CORE, &coreLimit) != 0)
	{
		emit_probe_stage("R23_STAGE_CORE_LIMIT_FAILED");
		return 10;
	}
	emit_probe_stage("R23_STAGE_CORE_DISABLED");

	std::error_code ec;
	const std::filesystem::path previousDirectory = std::filesystem::current_path(ec);
	if (ec)
	{
		emit_probe_stage("R23_STAGE_CWD_READ_FAILED");
		return 11;
	}
	std::filesystem::current_path(directory, ec);
	if (ec)
	{
		emit_probe_stage("R23_STAGE_CWD_SET_FAILED");
		return 12;
	}
	emit_probe_stage("R23_STAGE_CWD_READY");

	const int result = [&]() -> int
	{
		StrictBDMgr bd;
		HotMgr hot;
		RdmSink sink(&bd, &hot);
		WtRdmDtReader reader;
		reader.init(NULL, &sink);

		const vector<WTSTickStruct> rt = day_ticks(kCurTDate, 100);
		if (!write_bytes(rt_tick_file(), make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 10, rt)))
		{
			emit_probe_stage("R23_STAGE_RT_INPUT_FAILED");
			reader.clearCache();
			return 13;
		}
		emit_probe_stage("R23_STAGE_RT_INPUT_READY");
		emit_probe_stage("R23_STAGE_BEFORE_READ");

		WTSTickSlice* slice = reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
		emit_probe_stage("R23_STAGE_READ_RETURNED");
		if (slice == NULL)
		{
			reader.clearCache();
			emit_probe_stage("R23_STAGE_NULL_SLICE");
			return 14;
		}

		const WTSTickStruct* first = slice->at(0);
		const WTSTickStruct* last = slice->at(-1);
		const bool valid = slice->size() == 3u
			&& first != NULL && last != NULL
			&& first->action_date == kCurTDate && first->action_time == 143000000u
			&& last->action_date == kCurTDate && last->action_time == 143200000u
			&& first->price == 130.0 && last->price == 132.0;
		slice->release();
		reader.clearCache();
		if (!valid)
		{
			emit_probe_stage("R23_STAGE_SLICE_ASSERTIONS_FAILED");
			return 15;
		}

		// The product is absent from the strict commodity map. The public reader
		// contract is a safe NULL result rather than dereferencing a missing entry.
		WTSTickSlice* unknown = reader.readTickSliceByCount("SSE.BAD.600000", 3, 20240110150000000ull);
		if (unknown != NULL)
		{
			unknown->release();
			reader.clearCache();
			emit_probe_stage("R23_STAGE_UNKNOWN_COMMODITY_RETURNED_DATA");
			return 17;
		}
		bd.set_session_available(false);
		WTSTickSlice* missingSession = reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
		if (missingSession != NULL)
		{
			missingSession->release();
			reader.clearCache();
			emit_probe_stage("R23_STAGE_MISSING_SESSION_RETURNED_DATA");
			return 18;
		}

		emit_probe_stage("R23_STAGE_VALIDATION_COMPLETE");
		return 0;
	}();

	std::filesystem::current_path(previousDirectory, ec);
	if (ec)
	{
		emit_probe_stage("R23_STAGE_CWD_RESTORE_FAILED");
		return 16;
	}
	return result;
}

} // namespace

TEST(R23RdmCountStrictProbe, CountUsesProductAndSafelyRejectsUnknownCommodity)
{
	const bool deathTestChild = is_threadsafe_death_test_child();
	std::string probeDirectory;
	std::unique_ptr<OwnedProbeDirectory> ownedDirectory;
	std::unique_ptr<ScopedEnvironmentVariable> scopedDirectoryEnv;
	if (deathTestChild)
	{
		const char* inheritedDirectory = std::getenv(kStrictProbeDirectoryEnv);
		if (inheritedDirectory == NULL || inheritedDirectory[0] == '\0')
		{
			ADD_FAILURE() << "threadsafe death-test child did not inherit the parent-owned probe directory";
			return;
		}
		probeDirectory = inheritedDirectory;
	}
	else
	{
		std::error_code ec;
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			ADD_FAILURE() << "could not locate temporary directory: " << ec.message();
			return;
		}
		const std::string pattern = (tempRoot / "r23_strict_bdm_XXXXXX").string();
		std::vector<char> writablePattern(pattern.begin(), pattern.end());
		writablePattern.push_back('\0');
		char* createdDirectory = ::mkdtemp(writablePattern.data());
		if (createdDirectory == NULL)
		{
			ADD_FAILURE() << "mkdtemp failed for strict commodity probe: " << std::strerror(errno);
			return;
		}
		probeDirectory = createdDirectory;
		ownedDirectory.reset(new OwnedProbeDirectory(probeDirectory));

		scopedDirectoryEnv.reset(new ScopedEnvironmentVariable(kStrictProbeDirectoryEnv));
		if (!scopedDirectoryEnv->set(probeDirectory))
		{
			ADD_FAILURE() << "could not pass strict commodity probe directory to death-test child: " << std::strerror(errno);
			return;
		}
	}

	const std::string oldDeathTestStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(std::_Exit(run_strict_commodity_probe(probeDirectory)),
		::testing::ExitedWithCode(0), "R23_STAGE_VALIDATION_COMPLETE");
	GTEST_FLAG_SET(death_test_style, oldDeathTestStyle);
}

} // namespace r23
