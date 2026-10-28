// r74 / WtDataReader：多目录配置、逐笔历史正常装配、HOT 代码、K 线 RT 回退与 onMinuteEnd
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - init 的 `his_path` 数组分支 :183-200；`path` 数组 + `_his_dir` 多目录查找 :161-181
//  - readOrdQueSlice :458-612 / readOrdDtlSlice :615-769 / readTransSlice :772-926 的 etime==0
//    （sink 时钟 :464-472 / :621-629 / :778-786）与 his 首读/CMP_V2 解压/`>=` 回退
//  - 三条逐笔链的 HOT 原始代码 `_ruletag` 分支 :486-492 / :643-649 / :800-806
//  - readTickSlice 的 HOT 原始代码 :350-356
//  - readKlineSlice 的 RT 右界回退 :1826-1830；onMinuteEnd 的 preCnt==0 :2232-2235、
//    非复权 on_bar :2249-2252、时间未到 break :2265-2269
//
// 手法：真实 init(cfg, sink)（WtDataReader 不起线程）+ Sandbox 临时目录；无访问权限宏。
#include "r74_rdm_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

namespace r74 {

using std::string;
using std::vector;

class R74ReaderResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("r74rd"));
		_bd.set_cur_tdate(20240110);
		_root = _sandbox->root_abs();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader(const string& json)
	{
		const string cfgfile = _root + "reader.json";
		ASSERT_TRUE(q03::write_bytes(cfgfile, json));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void init_reader_simple()
	{
		init_reader(fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root));
	}

	std::unique_ptr<q03::Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	r74::HotMgr			_hot;
	r52::ReaderSink			_sink{ &_bd, &_hot };
	WtDataReader			_reader;
};

// ------------------------------------------------------------------ 多目录配置
// WtDataReader.cpp: init :161-200（path 数组、his_path 数组）；getRTTickBlock :1927-1937（rt 目录循环）
//   + cacheHisBarsFromFile 之前的 his 目录循环 :1652-1662
TEST_F(R74ReaderResidualTest, R74ReaderInitPathAndHisPathArrays)
{
	const string rootA = _root + "rootA/";
	const string rootB = _root + "rootB/";
	const string hisA = _root + "hisA/";
	const string hisB = _root + "hisB/";
	init_reader(fmtutil::format(
		"{{\"path\":[\"{}\",\"{}\"],\"his_path\":[\"{}\",\"{}\"],\"adjust_flag\":0}}",
		rootA, rootB, hisA, hisB));

	// 历史 tick 只放在第二个 his_path 下 → 依次探测 rootA/his、rootB/his、hisA、hisB 才命中
	vector<WTSTickStruct> his;
	his.push_back(q03::make_tick(20240108, 20240108, 93000000, 1));
	his.push_back(q03::make_tick(20240108, 20240108, 103000000, 2));
	ASSERT_TRUE(q03::write_bytes(hisB + "ticks/SSE/20240108/600000.dsb",
		q03::make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, q03::pack_ticks(his))));

	WTSTickSlice* s1 = _reader.readTickSlice("SSE.STK.600000", 5, 20240108150000000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(r52::flatten_ticks(s1).size(), (std::size_t)2);
	s1->release();

	// 当日 RT 块只放在第二个 path/rt 下 → rt 目录循环命中 rootB
	vector<WTSTickStruct> rt;
	rt.push_back(q03::make_tick(20240110, 20240110, 93000000, 3));
	rt.push_back(q03::make_tick(20240110, 20240110, 93100000, 4));
	ASSERT_TRUE(q03::write_bytes(rootB + "rt/ticks/SSE/600000.dmb",
		q03::make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, 2, 8, rt)));

	WTSTickSlice* s2 = _reader.readTickSlice("SSE.STK.600000", 2, 20240110150000000ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSTickStruct> got = r52::flatten_ticks(s2);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].action_time, 93000000u);
	EXPECT_EQ(got[1].action_time, 93100000u);
	s2->release();
}

// ------------------------------------------------------------------ 逐笔历史正常装配（sink 时钟）
// WtDataReader.cpp: readOrdQueSlice :464-612、readOrdDtlSlice :621-769、readTransSlice :778-926
TEST_F(R74ReaderResidualTest, R74ReaderHftHisPathsUseSinkClock)
{
	init_reader_simple();

	vector<WTSOrdQueStruct> que;
	que.push_back(q03::make_ordque(20240108, 20240108, 93000000, 1));
	que.push_back(q03::make_ordque(20240108, 20240108, 93100000, 2));
	que.push_back(q03::make_ordque(20240108, 20240108, 93200000, 3));
	ASSERT_TRUE(q03::write_bytes("his/queue/SSE/20240108/600000.dsb",
		q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, q03::pack_items(que))));

	vector<WTSOrdDtlStruct> dtl;
	for (uint32_t i = 0; i < 3; i++)
		dtl.push_back(q03::make_orddtl(20240107, 20240107, 93000000u + i * 100000u, 11 + i));
	ASSERT_TRUE(q03::write_bytes("his/orders/SSE/20240107/600000.dsb",
		q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, q03::pack_items(dtl))));

	vector<WTSTransStruct> trs;
	for (uint32_t i = 0; i < 3; i++)
		trs.push_back(q03::make_trans(20240106, 20240106, 93000000u + i * 100000u, 21 + i));
	ASSERT_TRUE(q03::write_bytes("his/trans/SSE/20240106/600000.dsb",
		q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, q03::pack_items(trs))));

	_sink.set_clock(20240108, 1500, 0);
	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600000", 2, 0);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	EXPECT_EQ(sq->at(0)->action_time, 93100000u);
	EXPECT_EQ(sq->at(1)->action_time, 93200000u);
	sq->release();

	_sink.set_clock(20240107, 1500, 0);
	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600000", 10, 0);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 3u);
	EXPECT_EQ(sd->at(1)->index, dtl[1].index);
	sd->release();

	_sink.set_clock(20240106, 1500, 0);
	WTSTransSlice* st = _reader.readTransSlice("SSE.STK.600000", 2, 0);
	ASSERT_TRUE(st != NULL);
	ASSERT_EQ(st->size(), 2u);
	EXPECT_EQ(st->at(0)->index, trs[1].index);
	EXPECT_EQ(st->at(1)->index, trs[2].index);
	st->release();
}

// ------------------------------------------------------------------ HOT 原始代码
// WtDataReader.cpp: readTickSlice :350-356、readOrdQueSlice :486-492、readOrdDtlSlice :643-649、
//   readTransSlice :800-806（future + ruleTag → getCustomRawCode 换成 rb2401）
TEST_F(R74ReaderResidualTest, R74ReaderHotRuleTagSelectsRawCode)
{
	_hot.set_enabled(true);
	_hot.set_raw("rb2401");
	init_reader_simple();

	vector<WTSTickStruct> ticks;
	ticks.push_back(q03::make_tick(20240108, 20240108, 93000000, 1));
	ticks.push_back(q03::make_tick(20240108, 20240108, 93100000, 2));
	ASSERT_TRUE(q03::write_bytes("his/ticks/SHFE/20240108/rb2401.dsb",
		q03::make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, q03::pack_ticks(ticks))));

	vector<WTSOrdQueStruct> que;
	que.push_back(q03::make_ordque(20240108, 20240108, 93000000, 3));
	ASSERT_TRUE(q03::write_bytes("his/queue/SHFE/20240108/rb2401.dsb",
		q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, q03::pack_items(que))));

	vector<WTSOrdDtlStruct> dtl;
	dtl.push_back(q03::make_orddtl(20240107, 20240107, 93000000, 4));
	ASSERT_TRUE(q03::write_bytes("his/orders/SHFE/20240107/rb2401.dsb",
		q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, q03::pack_items(dtl))));

	vector<WTSTransStruct> trs;
	trs.push_back(q03::make_trans(20240106, 20240106, 93000000, 5));
	ASSERT_TRUE(q03::write_bytes("his/trans/SHFE/20240106/rb2401.dsb",
		q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, q03::pack_items(trs))));

	WTSTickSlice* st = _reader.readTickSlice("SHFE.rb.HOT", 2, 20240108150000000ull);
	ASSERT_TRUE(st != NULL);
	EXPECT_EQ(r52::flatten_ticks(st).size(), (std::size_t)2);
	st->release();

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SHFE.rb.HOT", 2, 20240108150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 1u);
	EXPECT_DOUBLE_EQ(sq->at(0)->price, que[0].price);
	sq->release();

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SHFE.rb.HOT", 2, 20240107150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 1u);
	EXPECT_EQ(sd->at(0)->index, dtl[0].index);
	sd->release();

	WTSTransSlice* sr = _reader.readTransSlice("SHFE.rb.HOT", 2, 20240106150000000ull);
	ASSERT_TRUE(sr != NULL);
	ASSERT_EQ(sr->size(), 1u);
	EXPECT_EQ(sr->at(0)->index, trs[0].index);
	sr->release();
}

// ------------------------------------------------------------------ K 线 RT 回退 + onMinuteEnd
// WtDataReader.cpp: readKlineSlice :1820-1830（RT 右界回退）；onMinuteEnd :2232-2269
TEST_F(R74ReaderResidualTest, R74ReaderKlineRtBackoffAndMinuteEnd)
{
	init_reader_simple();

	// 600002：09:30 / 09:40 两根，etime 09:35 → lower_bound 命中 09:40 后 pBar-- 到 09:30
	vector<WTSBarStruct> rt2;
	rt2.push_back(q03::make_bar(20240110, TimeUtils::timeToMinBar(20240110, 930), 1));
	rt2.push_back(q03::make_bar(20240110, TimeUtils::timeToMinBar(20240110, 940), 2));
	ASSERT_TRUE(q03::write_bytes("rt/min1/SSE/600002.dmb",
		q03::make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, 2, 8, rt2)));

	WTSKlineSlice* s1 = _reader.readKlineSlice("SSE.STK.600002", KP_Minute1, 3, 202401100935ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSBarStruct> g1 = r52::flatten_bars(s1);
	ASSERT_EQ(g1.size(), (std::size_t)1);
	EXPECT_EQ(g1[0].time % 10000, 930u);
	s1->release();

	// 600001：先读一次（无 RT）建立 barsList/_raw_code 且游标保持 UINT_MAX，
	// 再补 RT 块 → onMinuteEnd 的 preCnt==0 分支，非后复权直接回调 on_bar。
	WTSKlineSlice* empty = _reader.readKlineSlice("SSE.STK.600001", KP_Minute1, 3, 202401101500ull);
	ASSERT_TRUE(empty != NULL);
	EXPECT_TRUE(r52::flatten_bars(empty).empty());
	empty->release();

	vector<WTSBarStruct> rt1;
	rt1.push_back(q03::make_bar(20240110, TimeUtils::timeToMinBar(20240110, 930), 3));
	rt1.push_back(q03::make_bar(20240110, TimeUtils::timeToMinBar(20240110, 931), 4));
	ASSERT_TRUE(q03::write_bytes("rt/min1/SSE/600001.dmb",
		q03::make_rt_block<WTSBarStruct>(BT_RT_Minute1, 20240110, 2, 8, rt1)));

	// now=09:31：600001 两根都到点；600002 的 09:40 还没到 → break
	_reader.onMinuteEnd(20240110, 931);
	EXPECT_EQ(_sink.bar_count(), 2u);
	EXPECT_EQ(_sink.last_code(), string("SSE.STK.600001"));
	EXPECT_EQ(_sink.last_period(), KP_Minute1);
	EXPECT_EQ(_sink.last_bar().time % 10000, 931u);

	// nowTime 回退 → _last_time 早退
	_reader.onMinuteEnd(20240110, 930);
	EXPECT_EQ(_sink.bar_count(), 2u);
}

// ------------------------------------------------------------------ 缺陷复现（不进启用集）
// BUG-R74-001：readOrdDtlSlice :690 与 readTransSlice :847 查的是 `_his_ordque_map`，
//   但分别装载进 `_his_orddtl_map` / `_his_trans_map`。同一 stdCode+交易日只要先读过 queue，
//   后续 dtl/trans 就会跳过装载并落到空 block → 文件存在也返回 NULL（顺序相关假空）。
TEST_F(R74ReaderResidualTest, DISABLED_R74ReaderQueueCacheHidesSameDayDetailAndTrans)
{
	init_reader_simple();

	vector<WTSOrdQueStruct> que;
	que.push_back(q03::make_ordque(20240108, 20240108, 93000000, 1));
	ASSERT_TRUE(q03::write_bytes("his/queue/SSE/20240108/600000.dsb",
		q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, q03::pack_items(que))));

	vector<WTSOrdDtlStruct> dtl;
	dtl.push_back(q03::make_orddtl(20240108, 20240108, 93100000, 2));
	ASSERT_TRUE(q03::write_bytes("his/orders/SSE/20240108/600000.dsb",
		q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, q03::pack_items(dtl))));

	vector<WTSTransStruct> trs;
	trs.push_back(q03::make_trans(20240108, 20240108, 93200000, 3));
	ASSERT_TRUE(q03::write_bytes("his/trans/SSE/20240108/600000.dsb",
		q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, q03::pack_items(trs))));

	WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.600000", 5, 20240108150000000ull);
	ASSERT_TRUE(q != NULL);
	q->release();

	// 期望：同一天的三类逐笔互不影响；实际：这两条会返回 NULL
	WTSOrdDtlSlice* d = _reader.readOrdDtlSlice("SSE.STK.600000", 5, 20240108150000000ull);
	EXPECT_TRUE(d != NULL);
	if (d != NULL)
		d->release();

	WTSTransSlice* t = _reader.readTransSlice("SSE.STK.600000", 5, 20240108150000000ull);
	EXPECT_TRUE(t != NULL);
	if (t != NULL)
		t->release();
}

} // namespace r74
