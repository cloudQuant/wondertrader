// r74 / WtRdmDtReader：init(cfg) 复权因子链、复权文件尺寸守卫、tick 区间 RT 边界
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - init(cfg!=NULL) :79-91（_base_dir、adjfactor 分支、5 秒巡检线程启动）与析构 :63-66 的 join
//  - loadStkAdjFactorsFromFile :169-237（PID 键/裸代码键、非数组跳过、19900101 兜底、排序、日志）
//  - cacheHisBarsFromFile 股票复权分支 :1364-1567（复权后文件缺失/过短/CMP 短头、原始文件续接、
//    因子倒序回放 :1508-1555，含 baseFactor、pBar<bar 的 continue、lastIdx==0 break）
//  - getAdjFactorByDate :2607-2641（左边界回退、命中、越过尾部、`-` 后缀截断、非股票）
//  - readTickSliceByRange :363-609 的 eIdx==0 守卫、pTick-- 回退、endTDate>curTDate 的收盘价右界
//  - readTickSliceByRange 的 hotCode 探测 :426-456 与 readTickSliceByCount 的 RT 尾窗 :2435-2473、
//    hotCode 回退 :2509-2549
//
// 线程纪律：只有 R74RdmInitCfgLoadsAdjFactorsAndReplays 走 init(cfg)，析构最多等待 5 秒巡检
// 线程一次睡眠；其余用例一律 init(NULL)。新增用例不触碰 CMD_CLEAR_CACHE。
#include "r74_rdm_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace r74 {

using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

WTSVariant* load_cfg(const string& file)
{
	return WTSCfgLoader::load_from_file(file.c_str());
}

} // namespace

class R74RdmInitAdjTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("r74adj"));
		_bd.set_cur_tdate(kCurTDate);
	}

	void TearDown() override
	{
		// init(cfg) 起了 5 秒巡检线程：不能在它运行期间 clearCache（map 会被并发 clear），
		// 直接析构让 ~WtRdmDtReader 先置 _stopped 再 join，join 返回后 map 才随成员析构。
		_reader.reset();
		_sandbox.reset();
	}

	void init_reader_null()
	{
		_reader.reset(new WtRdmDtReader());
		_reader->init(NULL, &_sink);
	}

	void init_reader_cfg(const char* adjfile)
	{
		const string root = _sandbox->root_abs();
		string json = fmtutil::format("{{\"path\":\"{}\"", root);
		if (adjfile != NULL && adjfile[0] != '\0')
			json += fmtutil::format(",\"adjfactor\":\"{}\"", adjfile);
		json += "}";
		const string cfgfile = root + "rdm_cfg.json";
		ASSERT_TRUE(q03::write_bytes(cfgfile, json));
		WTSVariant* cfg = load_cfg(cfgfile);
		ASSERT_TRUE(cfg != NULL);
		_reader.reset(new WtRdmDtReader());
		_reader->init(cfg, &_sink);
		cfg->release();
	}

	string root() const { return _sandbox->root_abs(); }

	std::unique_ptr<q03::Sandbox>	_sandbox;
	q03::BaseDataMgr		_bd;
	r74::HotMgr			_hot;
	q03::RdmSink			_sink{ &_bd, &_hot };
	std::unique_ptr<WtRdmDtReader>	_reader;
};

// ------------------------------------------------------------------ init(cfg) + 复权因子回放
// WtRdmDtReader.cpp: init :79-91；loadStkAdjFactorsFromFile :169-237；
//   cacheHisBarsFromFile 股票复权分支 :1364-1567（因子回放 :1508-1555）；getAdjFactorByDate :2607-2641
TEST_F(R74RdmInitAdjTest, R74RdmInitCfgLoadsAdjFactorsAndReplaysQfq)
{
	const string adj = root() + "adj.json";
	const string adj_json =
		"{\"SSE\":{"
		"\"600000\":["
		"{\"date\":20240103,\"factor\":1.1},"
		"{\"date\":20240105,\"factor\":1.2},"
		"{\"date\":20240201,\"factor\":1.3}],"
		"\"STK.600001\":[{\"date\":20240104,\"factor\":1.5}],"
		"\"BAD\":123}}";
	ASSERT_TRUE(q03::write_bytes(adj, adj_json));

	vector<WTSBarStruct> raw;
	for (uint32_t i = 0; i < 7; i++)
		raw.push_back(q03::make_bar(20240102 + i, 20240102 + i, i + 1));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600000.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(raw))));

	init_reader_cfg(adj.c_str());

	WTSKlineSlice* slice = _reader->readKlineSliceByRange("SSE.STK.600000-", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(slice != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(slice);
	ASSERT_EQ(got.size(), (std::size_t)7);

	// baseFactor = 因子表末条 1.3；20240102 用 1/1.3，0103/0104 用 1.1/1.3，0105 起用 1.2/1.3，
	// 末条 20240201 的 1.3/1.3 因低于末根 bar 日期走 `pBar->date < bar.date` continue。
	EXPECT_DOUBLE_EQ(got[0].open, 11.0 / 1.3);
	EXPECT_DOUBLE_EQ(got[1].open, 12.0 * 1.1 / 1.3);
	EXPECT_DOUBLE_EQ(got[2].open, 13.0 * 1.1 / 1.3);
	EXPECT_DOUBLE_EQ(got[3].open, 14.0 * 1.2 / 1.3);
	EXPECT_DOUBLE_EQ(got[6].open, 17.0 * 1.2 / 1.3);
	EXPECT_DOUBLE_EQ(got[6].close, 17.5 * 1.2 / 1.3);
	// Rdm 的因子回放只调整 OHLC，量额保持原值
	EXPECT_DOUBLE_EQ(got[3].vol, 2004.0);
	slice->release();

	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600000", 20240102), 1.0);
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600000", 20240104), 1.1);
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600000", 20240105), 1.2);
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600000", 20240201), 1.3);
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600000", 20240202), 1.3);
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600000-", 20240104), 1.1);
	// PID 形式键（STK.600001）也必须被加载
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SSE.STK.600001", 20240104), 1.5);
	EXPECT_DOUBLE_EQ(_reader->getAdjFactorByDate("SHFE.rb.2401", 20240104), 1.0);
}

// ------------------------------------------------------------------ 复权文件的尺寸守卫
// WtRdmDtReader.cpp: cacheHisBarsFromFile :1376-1437（复权后文件过短 :1382、CMP 短头 :1394）、
//   :1465-1477（原始文件缺失 continue、原始文件过短 return false）
TEST_F(R74RdmInitAdjTest, R74RdmAdjustedFileSizeGuards)
{
	init_reader_null();

	// 1) 复权后文件 8 字节 < HisKlineBlock → 打错误日志后回落到原始文件
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600002-.dsb", string(8, 'X')));
	vector<WTSBarStruct> raw2;
	for (uint32_t i = 0; i < 3; i++)
		raw2.push_back(q03::make_bar(20240102 + i, 20240102 + i, i + 1));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600002.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(raw2))));
	uint32_t err0 = _sink.count_of(LL_ERROR);

	WTSKlineSlice* s1 = _reader->readKlineSliceByRange("SSE.STK.600002-", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(s1 != NULL);
	EXPECT_EQ(r52::flatten_bars(s1).size(), (std::size_t)3);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
	s1->release();

	// 2) 复权后文件是 CMP 版本但只有 12 字节块头（不足 HisKlineBlockV2）→ :1394 的短头守卫
	BlockHeader cmp_head;
	memset(&cmp_head, 0, sizeof(cmp_head));
	memcpy(cmp_head._blk_flag, BLK_FLAG, FLAG_SIZE);
	cmp_head._type = BT_HIS_Day;
	cmp_head._version = BLOCK_VERSION_CMP;
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600003-.dsb",
		string((const char*)&cmp_head, sizeof(cmp_head))));
	vector<WTSBarStruct> raw3;
	for (uint32_t i = 0; i < 2; i++)
		raw3.push_back(q03::make_bar(20240103 + i, 20240103 + i, 10 + i));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600003.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(raw3))));
	err0 = _sink.count_of(LL_ERROR);
	WTSKlineSlice* s2 = _reader->readKlineSliceByRange("SSE.STK.600003-", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(r52::flatten_bars(s2).size(), (std::size_t)2);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
	s2->release();

	// 3) 复权后文件正常、原始文件缺失 → :1468 continue，仅返回复权后的两根
	vector<WTSBarStruct> adj4;
	adj4.push_back(q03::make_bar(20240104, 20240104, 21));
	adj4.push_back(q03::make_bar(20240105, 20240105, 22));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600004-.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(adj4))));
	WTSKlineSlice* s3 = _reader->readKlineSliceByRange("SSE.STK.600004-", KP_DAY,
		202401010000ull, 202401090000ull);
	ASSERT_TRUE(s3 != NULL);
	EXPECT_EQ(r52::flatten_bars(s3).size(), (std::size_t)2);
	s3->release();

	// 4) 复权后文件正常、原始文件 8 字节 → :1474-1477 返回 false
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600005-.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(adj4))));
	ASSERT_TRUE(q03::write_bytes("his/day/SSE/600005.dsb", string(8, 'Y')));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readKlineSliceByRange("SSE.STK.600005-", KP_DAY,
		202401010000ull, 202401090000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
}

// ------------------------------------------------------------------ tick 区间 RT 边界
// WtRdmDtReader.cpp: readTickSliceByRange :389-609：eIdx==0 守卫 :499-501、pTick-- :503-505、
//   endTDate>curTDate 的收盘时间右界 :558-562、同日裁剪 :593-604
TEST_F(R74RdmInitAdjTest, R74RdmTickRangeRtBoundaries)
{
	init_reader_null();

	vector<WTSTickStruct> rt;
	rt.push_back(q03::make_tick(kCurTDate, kCurTDate, 93000000, 1));
	rt.push_back(q03::make_tick(kCurTDate, kCurTDate, 93100000, 2));
	rt.push_back(q03::make_tick(kCurTDate, kCurTDate, 93200000, 3));
	ASSERT_TRUE(q03::write_bytes("rt/ticks/SSE/600000.dmb",
		q03::make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 8, rt)));

	// etime 早于 RT 首条 → eIdx==0 直接 break，返回空切片对象
	WTSTickSlice* empty = _reader->readTickSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240110090000000ull);
	ASSERT_TRUE(empty != NULL);
	EXPECT_TRUE(r52::flatten_ticks(empty).empty());
	empty->release();

	// etime 夹在 09:30 与 09:31 之间 → lower_bound 命中 09:31 后回退一条，取 09:30/09:31
	WTSTickSlice* clipped = _reader->readTickSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240110093030000ull);
	ASSERT_TRUE(clipped != NULL);
	vector<WTSTickStruct> got = r52::flatten_ticks(clipped);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].action_time, 93000000u);
	EXPECT_EQ(got[1].action_time, 93100000u);
	clipped->release();

	// endTDate > curTDate：RT 右边界改用收盘时间 15:00:59.999 → 三条全取
	WTSTickSlice* future = _reader->readTickSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240111150000000ull);
	ASSERT_TRUE(future != NULL);
	vector<WTSTickStruct> fgot = r52::flatten_ticks(future);
	ASSERT_EQ(fgot.size(), (std::size_t)3);
	EXPECT_EQ(fgot[2].action_time, 93200000u);
	future->release();

	// 同日 15:00 右界同样三条
	WTSTickSlice* whole = _reader->readTickSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240110150000000ull);
	ASSERT_TRUE(whole != NULL);
	EXPECT_EQ(r52::flatten_ticks(whole).size(), (std::size_t)3);
	whole->release();
}

// ------------------------------------------------------------------ HOT tick 候选文件 + 按条数尾窗
// WtRdmDtReader.cpp: readTickSliceByRange :426-456（hotCode 命中 + 短文件）:536-566（HOT 代码）；
//   readTickSliceByCount :2435-2473（RT 尾窗/回退）:2509-2549（hotCode 回退）
TEST_F(R74RdmInitAdjTest, R74RdmHotTickCandidatesAndCountWindows)
{
	_hot.set_enabled(true);
	_hot.set_raw("rb2401");
	init_reader_null();

	// 0108 的 hot 候选文件只有 8 字节 → bHitHot=true 后走长度校验失败
	ASSERT_TRUE(q03::write_bytes("his/ticks/SHFE/20240108/rb_HOT.dsb", string(8, 'Z')));
	vector<WTSTickStruct> d09;
	d09.push_back(q03::make_tick(20240109, 20240109, 93000000, 11));
	d09.push_back(q03::make_tick(20240109, 20240109, 93100000, 12));
	ASSERT_TRUE(q03::write_bytes("his/ticks/SHFE/20240109/rb_HOT.dsb",
		q03::make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, q03::pack_ticks(d09))));

	uint32_t err0 = _sink.count_of(LL_ERROR);
	WTSTickSlice* range = _reader->readTickSliceByRange("SHFE.rb.HOT",
		20240108090000000ull, 20240110090000000ull);
	ASSERT_TRUE(range != NULL);
	vector<WTSTickStruct> rgot = r52::flatten_ticks(range);
	ASSERT_EQ(rgot.size(), (std::size_t)2);
	EXPECT_EQ(rgot[0].action_date, 20240109u);
	EXPECT_GE(_sink.count_of(LL_ERROR), err0 + 1);
	range->release();

	// ByCount：当前日无 RT，回退到 0109 的 hot 候选文件（:2517 bHitHot）
	WTSTickSlice* count = _reader->readTickSliceByCount("SHFE.rb.HOT", 2,
		20240110150000000ull);
	ASSERT_TRUE(count != NULL);
	vector<WTSTickStruct> cgot = r52::flatten_ticks(count);
	ASSERT_EQ(cgot.size(), (std::size_t)2);
	EXPECT_EQ(cgot[0].action_time, 93000000u);
	EXPECT_EQ(cgot[1].action_time, 93100000u);
	count->release();

	// RT 块：09:30/09:31/09:32，容量 8
	vector<WTSTickStruct> rt;
	for (uint32_t i = 0; i < 3; i++)
		rt.push_back(q03::make_tick(kCurTDate, kCurTDate, 93000000u + i * 100000u, 31 + i));
	ASSERT_TRUE(q03::write_bytes("rt/ticks/SHFE/rb2401.dmb",
		q03::make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 8, rt)));

	// etime 早于首条 → eIdx==0 break（:2460-2462）；RT 段为空后仍会回补 0109 的两条历史
	WTSTickSlice* early = _reader->readTickSliceByCount("SHFE.rb.HOT", 3,
		20240110090000000ull);
	ASSERT_TRUE(early != NULL);
	vector<WTSTickStruct> egot = r52::flatten_ticks(early);
	ASSERT_EQ(egot.size(), (std::size_t)2);
	EXPECT_EQ(egot[0].action_date, 20240109u);
	EXPECT_EQ(egot[1].action_time, 93100000u);
	early->release();

	// etime 09:30:30 → pTick-- 回退到 09:30（:2464-2466），再前补 1 条历史
	WTSTickSlice* mid = _reader->readTickSliceByCount("SHFE.rb.HOT", 2,
		20240110093030000ull);
	ASSERT_TRUE(mid != NULL);
	vector<WTSTickStruct> mgot = r52::flatten_ticks(mid);
	ASSERT_EQ(mgot.size(), (std::size_t)2);
	EXPECT_EQ(mgot[0].action_date, 20240109u);
	EXPECT_EQ(mgot[1].action_time, 93000000u);
	mid->release();

	// endTDate > curTDate 的 RT 尾窗走收盘时间（:2443-2447），再回补 2 条 hot 历史
	WTSTickSlice* future = _reader->readTickSliceByCount("SHFE.rb.HOT", 4,
		20240111150000000ull);
	ASSERT_TRUE(future != NULL);
	vector<WTSTickStruct> fgot = r52::flatten_ticks(future);
	ASSERT_EQ(fgot.size(), (std::size_t)4);
	// left=4-3=1：历史段只补 0109 的尾条，RT 三条全部保留
	EXPECT_EQ(fgot[0].action_date, 20240109u);
	EXPECT_EQ(fgot[0].action_time, 93100000u);
	EXPECT_EQ(fgot[1].action_date, kCurTDate);
	EXPECT_EQ(fgot[1].action_time, 93000000u);
	EXPECT_EQ(fgot[3].action_time, 93200000u);
	future->release();
}

} // namespace r74
