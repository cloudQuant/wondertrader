// r74 / WtRdmDtReader：逐笔历史文件装配、错误守卫与 HOT K 线切段
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange :611-784   his 首读/CMP_V2 解压重排头 :699-736、普通与同日裁剪 :748-782、
//                                       header-only tcnt==0 :744-746、短文件/尺寸不符日志 :710-725
//  - readOrdDtlSliceByRange :786-958   同型链路 + 跨交易日全量分支 :937-942
//  - readTransSliceByRange  :960-1132  同型链路 + 跨交易日全量分支 :1111-1116
//  - cacheHisBarsFromFile HOT 分支 :1162-1363：包裹文件长度守卫 :1180-1184、切段返回：:1202-1210、
//      bAllCovered :1239-1243 / :1353-1356、分段文件缺失 continue :1266-1270、早于左界 continue :1301-1305、
//      eIdx==0 break / pBar-- :1319-1327、HOT+ 的 OHLC 复权 :1334-1344、分段文件过短 :1276-1279、
//      eBar<=lastHotTime break :1262-1263
//
// 手法沿用 q03/R52：init(NULL, sink)，零线程；Sandbox 切 CWD；无访问权限宏。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r74 {

using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

string his_hft_file(const char* kind, uint32_t date, const char* code = "600000")
{
	return fmtutil::format("his/{}/SSE/{}/{}.dsb", kind, date, code);
}

vector<WTSOrdQueStruct> make_queues(uint32_t tdate, uint32_t base_seq)
{
	vector<WTSOrdQueStruct> ay;
	for (uint32_t i = 0; i < 3; i++)
		ay.push_back(q03::make_ordque(tdate, tdate, 93000000u + i * 100000u, base_seq + i));
	return ay;
}

vector<WTSOrdDtlStruct> make_orddtls(uint32_t tdate, uint32_t base_seq)
{
	vector<WTSOrdDtlStruct> ay;
	for (uint32_t i = 0; i < 3; i++)
		ay.push_back(q03::make_orddtl(tdate, tdate, 93000000u + i * 100000u, base_seq + i));
	return ay;
}

vector<WTSTransStruct> make_transes(uint32_t tdate, uint32_t base_seq)
{
	vector<WTSTransStruct> ay;
	for (uint32_t i = 0; i < 3; i++)
		ay.push_back(q03::make_trans(tdate, tdate, 93000000u + i * 100000u, base_seq + i));
	return ay;
}

} // namespace

class R74RdmHftHotTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("r74hft"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.reset(new WtRdmDtReader());
		_reader->init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader->clearCache();
		_reader.reset();
		_sandbox.reset();
	}

	std::unique_ptr<q03::Sandbox>	_sandbox;
	q03::BaseDataMgr		_bd;
	r74::HotMgr			_hot;
	q03::RdmSink			_sink{ &_bd, &_hot };
	std::unique_ptr<WtRdmDtReader>	_reader;
};

// ------------------------------------------------------------------ 委托队列历史
// WtRdmDtReader.cpp: readOrdQueSliceByRange :699-736 建块/解压，:744-746 header-only，
//   :748-782 普通与 `>=` 回退；短文件 :710-717、尺寸不符 :721-725
TEST_F(R74RdmHftHotTest, R74RdmHisQueueLoadsClipsAndRejects)
{
	const vector<WTSOrdQueStruct> que = make_queues(20240108, 1);
	ASSERT_TRUE(q03::write_bytes(his_hft_file("queue", 20240108),
		q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, q03::pack_items(que))));

	WTSOrdQueSlice* all = _reader->readOrdQueSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(0)->action_time, 93000000u);
	EXPECT_EQ(all->at(2)->action_time, 93200000u);
	all->release();

	// etime=09:31 → lower_bound 命中 09:31 后 `>=` 回退，只剩 09:30
	WTSOrdQueSlice* one = _reader->readOrdQueSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240108093100000ull);
	ASSERT_TRUE(one != NULL);
	ASSERT_EQ(one->size(), 1u);
	EXPECT_EQ(one->at(0)->action_time, 93000000u);
	one->release();

	// header-only 压缩块：解压后 tcnt==0 → :744-746 返回 NULL
	ASSERT_TRUE(q03::write_bytes(his_hft_file("queue", 20240109),
		q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, "")));
	EXPECT_TRUE(_reader->readOrdQueSliceByRange("SSE.STK.600000",
		20240109090000000ull, 20240109150000000ull) == NULL);

	// 8 字节短文件 → :710-717
	ASSERT_TRUE(q03::write_bytes(his_hft_file("queue", 20240107), string(8, 'Q')));
	uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readOrdQueSliceByRange("SSE.STK.600000",
		20240107090000000ull, 20240107150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 合法块尾部多 1 字节 → :721-725 尺寸校验失败
	ASSERT_TRUE(q03::write_bytes(his_hft_file("queue", 20240106),
		q03::make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, q03::pack_items(que)) + "X"));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readOrdQueSliceByRange("SSE.STK.600000",
		20240106090000000ull, 20240106150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
}

// ------------------------------------------------------------------ 委托明细历史
// WtRdmDtReader.cpp: readOrdDtlSliceByRange :874-935 建块/解压，:937-942 跨交易日全量，
//   :946-957 同日裁剪；短文件 :888-892、尺寸不符 :897-900、header-only :919-921
TEST_F(R74RdmHftHotTest, R74RdmHisDetailCrossDayAndRejects)
{
	const vector<WTSOrdDtlStruct> dtl = make_orddtls(20240105, 11);
	ASSERT_TRUE(q03::write_bytes(his_hft_file("orders", 20240105),
		q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, q03::pack_items(dtl))));

	// beginTDate != endTDate → 返回 0..eIdx 全部三条（:940-942）
	WTSOrdDtlSlice* all = _reader->readOrdDtlSliceByRange("SSE.STK.600000",
		20240104090000000ull, 20240105093300000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(1)->index, dtl[1].index);
	all->release();

	// 同日窗口 09:00~09:31 → `>=` 回退后仅 09:30（:946-957）
	WTSOrdDtlSlice* one = _reader->readOrdDtlSliceByRange("SSE.STK.600000",
		20240105090000000ull, 20240105093100000ull);
	ASSERT_TRUE(one != NULL);
	ASSERT_EQ(one->size(), 1u);
	EXPECT_EQ(one->at(0)->index, dtl[0].index);
	one->release();

	// 8 字节短文件
	ASSERT_TRUE(q03::write_bytes(his_hft_file("orders", 20240103), string(8, 'D')));
	uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readOrdDtlSliceByRange("SSE.STK.600000",
		20240103090000000ull, 20240103150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 尺寸不符
	ASSERT_TRUE(q03::write_bytes(his_hft_file("orders", 20240102),
		q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, q03::pack_items(dtl)) + "X"));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readOrdDtlSliceByRange("SSE.STK.600000",
		20240102090000000ull, 20240102150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// header-only
	ASSERT_TRUE(q03::write_bytes(his_hft_file("orders", 20240101),
		q03::make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, "")));
	EXPECT_TRUE(_reader->readOrdDtlSliceByRange("SSE.STK.600000",
		20240101090000000ull, 20240101150000000ull) == NULL);
}

// ------------------------------------------------------------------ 逐笔成交历史
// WtRdmDtReader.cpp: readTransSliceByRange :1048-1085 建块/解压，:1111-1116 跨交易日全量，
//   :1120-1131 同日裁剪；短文件 :1061-1066、尺寸不符 :1071-1074
TEST_F(R74RdmHftHotTest, R74RdmHisTransCrossDayAndRejects)
{
	const vector<WTSTransStruct> trs = make_transes(20231229, 21);
	ASSERT_TRUE(q03::write_bytes(his_hft_file("trans", 20231229),
		q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, q03::pack_items(trs))));

	WTSTransSlice* all = _reader->readTransSliceByRange("SSE.STK.600000",
		20231228090000000ull, 20231229093300000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(2)->index, trs[2].index);
	all->release();

	WTSTransSlice* one = _reader->readTransSliceByRange("SSE.STK.600000",
		20231229090000000ull, 20231229093100000ull);
	ASSERT_TRUE(one != NULL);
	ASSERT_EQ(one->size(), 1u);
	EXPECT_EQ(one->at(0)->index, trs[0].index);
	one->release();

	ASSERT_TRUE(q03::write_bytes(his_hft_file("trans", 20231228),
		q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, q03::pack_items(trs)) + "X"));
	uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readTransSliceByRange("SSE.STK.600000",
		20231228090000000ull, 20231228150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	ASSERT_TRUE(q03::write_bytes(his_hft_file("trans", 20231227), string(8, 'T')));
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readTransSliceByRange("SSE.STK.600000",
		20231227090000000ull, 20231227150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	ASSERT_TRUE(q03::write_bytes(his_hft_file("trans", 20231226),
		q03::make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, "")));
	EXPECT_TRUE(_reader->readTransSliceByRange("SSE.STK.600000",
		20231226090000000ull, 20231226150000000ull) == NULL);
}

// ------------------------------------------------------------------ HOT K 线切段
// WtRdmDtReader.cpp: cacheHisBarsFromFile HOT 分支 :1162-1363（见文件头行段）
TEST_F(R74RdmHftHotTest, R74RdmHotDaySectionsEdgeCases)
{
	_hot.set_enabled(true);
	_hot.set_raw("rb2402");

	// ---- A) HOT+：包裹文件到 0106，切段从 0107 续接，bAllCovered 提前结束，OHLC 乘 1.2
	vector<WTSBarStruct> hot;
	hot.push_back(q03::make_bar(20240105, 20240105, 1));
	hot.push_back(q03::make_bar(20240106, 20240106, 2));
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/SHFE.rb_HOT+.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(hot))));

	vector<WTSBarStruct> seg;
	for (uint32_t i = 0; i < 6; i++)
		seg.push_back(q03::make_bar(20240105 + i, 20240105 + i, 11 + i));
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/rb2402.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(seg))));

	_hot.clear_sections();
	_hot.add_section("rb2401", 20240102, 20240104, 1.1);
	_hot.add_section("rb2402", 20240105, 20240110, 1.2);

	WTSKlineSlice* s1 = _reader->readKlineSliceByRange("SHFE.rb.HOT+", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_TRUE(s1 != NULL);
	vector<WTSBarStruct> g1 = r52::flatten_bars(s1);
	ASSERT_EQ(g1.size(), (std::size_t)6);
	EXPECT_EQ(g1[0].date, 20240105u);
	EXPECT_EQ(g1[1].date, 20240106u);
	EXPECT_DOUBLE_EQ(g1[0].open, 11.0);
	EXPECT_DOUBLE_EQ(g1[1].open, 12.0);
	EXPECT_EQ(g1[2].date, 20240107u);
	EXPECT_DOUBLE_EQ(g1[2].open, 23.0 * 1.2);
	EXPECT_DOUBLE_EQ(g1[5].open, 26.0 * 1.2);
	s1->release();

	// ---- B) 分段文件早于左界 → pBar->date < sBar.date 的 continue（:1301-1305）
	_reader->clearCache();
	std::error_code ec;
	std::filesystem::remove("his/day/SHFE/SHFE.rb_HOT+.dsb", ec);
	vector<WTSBarStruct> early;
	early.push_back(q03::make_bar(20240102, 20240102, 31));
	early.push_back(q03::make_bar(20240103, 20240103, 32));
	early.push_back(q03::make_bar(20240104, 20240104, 33));
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/rb2403.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(early))));
	_hot.clear_sections();
	_hot.add_section("rb2403", 20240106, 20240110, 1.0);
	EXPECT_TRUE(_reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull) == NULL);

	// ---- C) eBar 落在首根之前 → eIdx==0 break（:1321-1323）
	_reader->clearCache();
	vector<WTSBarStruct> late;
	late.push_back(q03::make_bar(20240102, 20240102, 41));
	late.push_back(q03::make_bar(20240105, 20240105, 42));
	late.push_back(q03::make_bar(20240106, 20240106, 43));
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/rb2404.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(late))));
	_hot.clear_sections();
	_hot.add_section("rb2404", 20231201, 20231210, 1.0);
	EXPECT_TRUE(_reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202312010000ull, 202312101500ull) == NULL);

	// ---- D) eBar 夹在 0102 与 0105 之间 → pBar--（:1325-1327），只留 0102
	_reader->clearCache();
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/rb2405.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(late))));
	_hot.clear_sections();
	_hot.add_section("rb2405", 20240101, 20240104, 1.0);
	WTSKlineSlice* s4 = _reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401041500ull);
	ASSERT_TRUE(s4 != NULL);
	vector<WTSBarStruct> g4 = r52::flatten_bars(s4);
	ASSERT_EQ(g4.size(), (std::size_t)1);
	EXPECT_EQ(g4[0].date, 20240102u);
	s4->release();

	// ---- E) HOT 包裹文件过短（:1180-1184），分段仍按正常文件装配
	_reader->clearCache();
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/SHFE.rb_HOT.dsb", string(8, 'H')));
	vector<WTSBarStruct> seg6;
	for (uint32_t i = 0; i < 3; i++)
		seg6.push_back(q03::make_bar(20240102 + i, 20240102 + i, 51 + i));
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/rb2406.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(seg6))));
	_hot.clear_sections();
	_hot.add_section("rb2406", 20240102, 20240105, 1.0);
	uint32_t err0 = _sink.count_of(LL_ERROR);
	WTSKlineSlice* s5 = _reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401051500ull);
	ASSERT_TRUE(s5 != NULL);
	EXPECT_EQ(r52::flatten_bars(s5).size(), (std::size_t)3);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);
	s5->release();

	// ---- F) 分段文件过短 → :1276-1279 return false
	_reader->clearCache();
	std::filesystem::remove("his/day/SHFE/SHFE.rb_HOT.dsb", ec);
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/rb2407.dsb", string(8, 'S')));
	_hot.clear_sections();
	_hot.add_section("rb2407", 20240102, 20240105, 1.0);
	err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401051500ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// ---- G) splitCustomSections 返回 true 但切段为空 → :1209-1210 return false
	_reader->clearCache();
	_hot.clear_sections();
	_hot.set_split_empty(true);
	EXPECT_TRUE(_reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401051500ull) == NULL);
	_hot.set_split_empty(false);

	// ---- H) 切段右界 <= 主力末条时间 → :1262-1263 break，只返回包裹文件内容
	_reader->clearCache();
	vector<WTSBarStruct> hot8;
	hot8.push_back(q03::make_bar(20240110, 20240110, 61));
	ASSERT_TRUE(q03::write_bytes("his/day/SHFE/SHFE.rb_HOT.dsb",
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(hot8))));
	_hot.clear_sections();
	_hot.add_section("rb2408", 20240102, 20240109, 1.0);
	WTSKlineSlice* s8 = _reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull);
	ASSERT_TRUE(s8 != NULL);
	EXPECT_EQ(r52::flatten_bars(s8).size(), (std::size_t)1);
	s8->release();

	// ---- I) 分段文件缺失 → :1266-1270 continue，整体空结果
	_reader->clearCache();
	std::filesystem::remove("his/day/SHFE/SHFE.rb_HOT.dsb", ec);
	_hot.clear_sections();
	_hot.add_section("rb2409", 20240102, 20240110, 1.0);
	EXPECT_TRUE(_reader->readKlineSliceByRange("SHFE.rb.HOT", KP_DAY,
		202401010000ull, 202401101500ull) == NULL);
}

} // namespace r74
