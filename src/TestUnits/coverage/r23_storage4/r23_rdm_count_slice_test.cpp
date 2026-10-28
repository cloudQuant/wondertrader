// r23_storage4 / WtDataStorage：WtRdmDtReader 的「按条数取数」两条公开入口
//
// 覆盖对象（src/WtDataStorage/WtRdmDtReader.cpp，q03 明确跳过、R1 未做，本文件补上）：
//  - readTickSliceByCount  :2395-2605  RT 块优先取尾 + 逐日回退拼 his/ticks/<exchg>/<date>/<code>.dsb、
//                                      缺文件累计 missingCnt 到 30 天停止、长度校验失败、
//                                      尺寸校验失败后 _his_tick_map 留空条目、count==0、etime 边界
//  - readKlineSliceByCount :2219-2393  etime==0 → 203012312359、KP_DAY 无 RT 块、
//                                      indexBarFromCacheByCount 尾窗、his+rt 双块拼接
//  - indexBarFromCacheByCount :1688-1727 经公开入口驱动（eit==end 收敛 / curCnt=min 截断）
//  - getRTTickBlock :2002 / getRTKilneBlock :2162（min1 与 KP_DAY 两条 switch 分支）
//
// 手法沿用 q03/R1：WtRdmDtReader::_base_dir 是 private 且只有 init(cfg!=NULL) 才赋值
// （那条路径起 5 秒轮询线程、析构 join），故保持空串 → 生产拼出的路径退化为相对路径，
// 由 q03::Sandbox 把 CWD 切到 temp 目录。**没有访问权限宏**。
// 注意 q03::BaseDataMgr::getCommodity 恒返回非空，所以 :2401 那句无条件解引用不会被本文件触发
// （见 bugs/R23_storage.md BUG-R23-003）。
#include "../q03_storage3/q03_storage3_fixture.h"

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

TEST_F(R23RdmCountTest, HisWindowBoundaryDropsTickExactlyAtEtime)
{
	// etime 落在 20240108（< curTDate）→ hasToday 为假，直接走 his 逐日回退
	const vector<WTSTickStruct> d08 = day_ticks(20240108, 100);
	const vector<WTSTickStruct> d07 = day_ticks(20240107, 300);
	ASSERT_TRUE(write_his_tick_file(20240108, d08));
	ASSERT_TRUE(write_his_tick_file(20240107, d07));

	// 边界正好压在 14:31 那条上：his 分支判的是 `>=`（:2585）→ 边界那条被退掉
	WTSTickSlice* slice = _reader.readTickSliceByCount("SSE.STK.600000", 2, 20240108143100000ull);
	ASSERT_TRUE(slice != NULL);
	EXPECT_EQ(slice->size(), 2u);
	const WTSTickStruct* first = slice->at(0);
	const WTSTickStruct* last = slice->at(-1);
	ASSERT_TRUE(first != NULL);
	ASSERT_TRUE(last != NULL);
	// 20240108 只拿到 14:30 一条，缺口由 20240107 的尾条前补
	EXPECT_EQ(last->action_date, 20240108u);
	EXPECT_EQ(last->action_time, 143000000u);
	EXPECT_EQ(first->action_date, 20240107u);
	EXPECT_EQ(first->action_time, 143200000u);
	slice->release();
}

TEST_F(R23RdmCountTest, RtWindowBoundaryKeepsTickExactlyAtEtime)
{
	// 同一份 3 条 tick，这次放在当日 RT 块里：RT 分支判的是 `>`（:2459）→ 边界那条被保留
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

	// 边界压到块内最后一条：RT 分支仍然把它算进来（`>` 不成立）
	WTSTickSlice* whole = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110143200000ull);
	ASSERT_TRUE(whole != NULL);
	EXPECT_EQ(whole->size(), 3u);
	EXPECT_EQ(whole->at(-1)->action_time, 143200000u);
	whole->release();
}

TEST_F(R23RdmCountTest, CountSizeCheckFailureLeavesEmptyEntryForever)
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

	const vector<WTSTickStruct> d07 = day_ticks(20240107, 300);
	ASSERT_TRUE(write_his_tick_file(20240107, d07));

	WTSTickSlice* s1 = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
	ASSERT_TRUE(s1 != NULL);
	// 坏文件那一天被跳过，只拿到 20240107 的 3 条
	EXPECT_EQ(s1->size(), 3u);
	EXPECT_EQ(_sink.count_of(LL_ERROR), 1u);
	EXPECT_NE(_sink.last_log().find("Sizechecking of his tick data file"), string::npos);
	s1->release();

	// 第二次同样请求：_his_tick_map 里那条 key 还在，但 _block 为空 → 直接跳过，连日志都不再打
	const uint32_t err_before = _sink.count_of(LL_ERROR);
	WTSTickSlice* s2 = _reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->size(), 3u);
	EXPECT_EQ(_sink.count_of(LL_ERROR), err_before)
		<< "坏文件条目未被清掉：缓存里留下一个空 _block，该交易日此后永久跳过（见 BUG-R23-004）";
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

TEST_F(R23RdmCountTest, KlineCountWithNoBarsAnywhereReturnsPhantomSlice)
{
	// 既无 his 文件也无 RT 块 → indexBarFromCacheByCount 在 `_bars.empty()` 处 return NULL，
	// 但它**没有把出参 count 归零**（WtRdmDtReader.cpp:1695-1696），于是
	// readKlineSliceByCount 仍然用「请求条数」去 create 了一个没有任何块的切片（:2384-2389）
	WTSKlineSlice* slice = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute5, 5, 202401101500);
	ASSERT_TRUE(slice != NULL) << ":2384 判的是 hisCnt+rtCnt，而 hisCnt 没被清空";
	EXPECT_EQ(slice->size(), 5u)      << "切片自称 5 条";
	EXPECT_EQ(slice->get_block_counts(), 0u) << "却一个数据块都没挂";
	EXPECT_TRUE(slice->at(0) == NULL);
	EXPECT_TRUE(slice->at(-1) == NULL);
	slice->release();

	// 空文件的 min5（barcnt==0）同样落进这条「缓存建好但没有条目」的路径
	vector<WTSBarStruct> none;
	ASSERT_TRUE(write_bytes(his_kline_file("min5"),
		make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2, pack_bars(none))));
	WTSKlineSlice* slice2 = _reader.readKlineSliceByCount("SSE.STK.600000", KP_Minute5, 3, 202401101500);
	ASSERT_TRUE(slice2 != NULL);
	EXPECT_EQ(slice2->size(), 3u);
	EXPECT_EQ(slice2->get_block_counts(), 0u);
	EXPECT_TRUE(slice2->at(1) == NULL);
	slice2->release();
}

// ------------------------------------------------------------------ 崩溃类收口
namespace {

// 把 getCommodity(exchg,pid) 改成与真实 WTSBaseDataMgr 一致：查的是 "exchg.<product>" 表，
// 拿**合约代码**去查一定返回 NULL（WTSBaseDataMgr.cpp:81-89）
class StrictBDMgr : public BaseDataMgr
{
public:
	StrictBDMgr()
	{
		_stk = WTSCommodityInfo::create("STK", "股票", "SSE", "SSE_DAY", "CHN");
		_stk->setCategory(ContractCategory::CC_Stock);
	}

	virtual ~StrictBDMgr() { _stk->release(); }

	WTSCommodityInfo* getCommodity(const char*, const char* pid) override
	{
		if (pid != NULL && strcmp(pid, "STK") == 0)
			return _stk;
		return NULL;
	}

private:
	WTSCommodityInfo*	_stk = NULL;
};

} // namespace

TEST_F(R23RdmCountTest, DISABLED_CountByCodeShapedPidDereferencesNullCommodity)
{
	// 复现（不要启用）：readTickSliceByCount 的 :2401
	//   getSession(getCommodity(cInfo._exchg, cInfo._code)->getSession())
	// 两个问题：① 传的是**合约代码**而不是 product id，真实的按 "exchg.product" 建表的
	//   WTSBaseDataMgr 一定查不到；② 返回值没有 NULL 判定，紧接着还有
	//   `commInfo->isFuture()`（:2420）也无条件解引用 :2398 的结果。
	// → 段错误。两个文件的其余 20+ 处 getCommodity 传的 are _product，只有这一行用 _code。
	StrictBDMgr	bd;
	HotMgr		hot;
	RdmSink		sink(&bd, &hot);
	WtRdmDtReader	reader;
	reader.init(NULL, &sink);

	const vector<WTSTickStruct> rt = day_ticks(kCurTDate, 100);
	ASSERT_TRUE(write_bytes(rt_tick_file(), make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 10, rt)));

	WTSTickSlice* slice = reader.readTickSliceByCount("SSE.STK.600000", 3, 20240110150000000ull);
	if (slice != NULL)
		slice->release();
	reader.clearCache();
}

} // namespace r23
