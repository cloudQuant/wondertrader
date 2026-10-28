// r100 / WtDataReader：逐笔 RT/历史的跨自然日边界、缺文件/空块回退、HOT 组装
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readTickSlice        RT 跨日期二分与回退 :371-385；短历史二次访问 :427-429；跨日期 :437-449
//  - readOrdQueSlice      RT :507-526；历史缺文件 :553-554、短文件二次访问 :584-586、空块 :590-592、
//                         跨日期 :594-606
//  - readOrdDtlSlice      RT :664-683；历史缺文件 :710-711、BUG-R74-001 空块 :741-743、空块 :747-749、
//                         跨日期 :751-762
//  - readTransSlice       RT :821-840；历史缺文件 :867-868、BUG-R74-001 空块 :898-900、空块 :904-906、
//                         跨日期 :908-919
//  - getRTTickBlock / getRTOrdDtlBlock / getRTOrdQueBlock / getRTTransBlock 的 !bHit :1936-1937 /
//    :1990-1991 / :2044-2045 / :2098-2099
//  - cacheIntegratedBars  KP_Minute5 周期名 :978-984、扩展 loader 分月读取 :1132-1140、
//    逐段装配与复权 :1181-1258；qfq/hfq 基础因子 :1074-1078
//  - cacheAdjustedStkBars 头-only 复权文件触发 std::out_of_range -> catch :1616-1624
//
// init 只赋值目录，不起线程；所有临时文件都在 q03::Sandbox 内。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtDataReader.h"

namespace r100 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

vector<WTSTickStruct> cross_ticks(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSTickStruct> ret;
	ret.push_back(make_tick(tdate, prevDate, 210000000, seq));
	ret.push_back(make_tick(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_tick(tdate, tdate, 103000000, seq + 2));
	return ret;
}

vector<WTSOrdQueStruct> cross_ques(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSOrdQueStruct> ret;
	ret.push_back(make_ordque(tdate, prevDate, 210000000, seq));
	ret.push_back(make_ordque(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_ordque(tdate, tdate, 103000000, seq + 2));
	return ret;
}

vector<WTSOrdDtlStruct> cross_dtls(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSOrdDtlStruct> ret;
	ret.push_back(make_orddtl(tdate, prevDate, 210000000, seq));
	ret.push_back(make_orddtl(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_orddtl(tdate, tdate, 103000000, seq + 2));
	return ret;
}

vector<WTSTransStruct> cross_trans(uint32_t tdate, uint32_t prevDate, uint32_t seq)
{
	vector<WTSTransStruct> ret;
	ret.push_back(make_trans(tdate, prevDate, 210000000, seq));
	ret.push_back(make_trans(tdate, tdate, 93000000, seq + 1));
	ret.push_back(make_trans(tdate, tdate, 103000000, seq + 2));
	return ret;
}

} // namespace

class R100ReaderCrossDayTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r100reader"));
		_bd.set_cur_tdate(kCurTDate);
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	void init_reader(IHisDataLoader* loader = NULL)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _sandbox->root_abs().c_str());
		_reader.init(cfg, &_sink, loader);
		cfg->release();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	r52::BDMgr			_bd;
	r74::HotMgr			_hot;
	r52::ReaderSink			_sink{ &_bd, &_hot };
	r52::Loader			_loader;
	WtDataReader			_reader;
};

// WtDataReader.cpp: L371-385 / L427-429 / L437-449 / L507-526 / L553-554 / L584-586 /
//                   L590-592 / L594-606 / L664-683 / L710-711 / L741-743 / L747-749 /
//                   L751-762 / L821-840 / L867-868 / L898-900 / L904-906 / L908-919
TEST_F(R100ReaderCrossDayTest, R100ReaderRtHftCrossDateAndMissing)
{
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600000.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 8, cross_ticks(kCurTDate, 20240109, 1))));
	ASSERT_TRUE(write_bytes("rt/queue/SSE/610000.dmb",
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kCurTDate, 3, 8, cross_ques(kCurTDate, 20240109, 4))));
	ASSERT_TRUE(write_bytes("rt/orders/SSE/620000.dmb",
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kCurTDate, 3, 8, cross_dtls(kCurTDate, 20240109, 7))));
	ASSERT_TRUE(write_bytes("rt/trans/SSE/630000.dmb",
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kCurTDate, 3, 8, cross_trans(kCurTDate, 20240109, 10))));

	// 截止 10:00 落在 09:30 与 10:30 之间：lower_bound 命中末条后回退一条
	WTSTickSlice* ts = _reader.readTickSlice("SSE.STK.600000", 2, 20240110100000000ull);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(r52::flatten_ticks(ts).size(), (std::size_t)2);
	ts->release();

	WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.610000", 2, 20240110100000000ull);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 2u);
	q->release();

	WTSOrdDtlSlice* d = _reader.readOrdDtlSlice("SSE.STK.620000", 2, 20240110100000000ull);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 2u);
	d->release();

	WTSTransSlice* tr = _reader.readTransSlice("SSE.STK.630000", 2, 20240110100000000ull);
	ASSERT_TRUE(tr != NULL);
	EXPECT_EQ(tr->size(), 2u);
	tr->release();

	// 文件缺失：getRT*Block 返回 NULL，调用方直接返回
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600001", 2, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.610001", 2, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.620001", 2, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.630001", 2, 20240110100000000ull) == NULL);
}

// WtDataReader.cpp: L437-449 / L553-554 / L584-586 / L590-592 / L594-606 / L710-711 /
//                   L741-743 / L747-749 / L751-762 / L867-868 / L898-900 / L904-906 / L908-919
TEST_F(R100ReaderCrossDayTest, R100ReaderHisHftCrossDateShortAndEmpty)
{
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/611100.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(cross_ques(20240109, 20240108, 20)))));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/621100.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(cross_dtls(20240109, 20240108, 23)))));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/631100.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(cross_trans(20240109, 20240108, 26)))));

	WTSOrdQueSlice* q = _reader.readOrdQueSlice("SSE.STK.611100", 2, 20240109100000000ull);
	ASSERT_TRUE(q != NULL);
	EXPECT_EQ(q->size(), 2u);
	q->release();

	WTSOrdDtlSlice* d = _reader.readOrdDtlSlice("SSE.STK.621100", 2, 20240109100000000ull);
	ASSERT_TRUE(d != NULL);
	EXPECT_EQ(d->size(), 2u);
	d->release();

	WTSTransSlice* tr = _reader.readTransSlice("SSE.STK.631100", 2, 20240109100000000ull);
	ASSERT_TRUE(tr != NULL);
	EXPECT_EQ(tr->size(), 2u);
	tr->release();

	// 历史文件缺失
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.611101", 2, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.621101", 2, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.631101", 2, 20240109100000000ull) == NULL);

	// 短文件先清空缓存，第二次命中「块为空」
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/611102.dsb", string(4, '\0')));
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.611102", 2, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.611102", 2, 20240109100000000ull) == NULL);

	// 空载荷：解压后只剩块头，tcnt==0
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240108/611105.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, string())));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240108/621105.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, string())));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240108/631105.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, string())));
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.611105", 2, 20240108100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.621105", 2, 20240108100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.631105", 2, 20240108100000000ull) == NULL);

	// 既有缺陷 BUG-R74-001：queue 占住同 key 后，dtl/trans 跳过装载并返回空块
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/611103.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(cross_ques(20240109, 20240108, 30)))));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/611104.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(cross_ques(20240109, 20240108, 33)))));
	WTSOrdQueSlice* q1 = _reader.readOrdQueSlice("SSE.STK.611103", 2, 20240109100000000ull);
	ASSERT_TRUE(q1 != NULL);
	q1->release();
	WTSOrdQueSlice* q2 = _reader.readOrdQueSlice("SSE.STK.611104", 2, 20240109100000000ull);
	ASSERT_TRUE(q2 != NULL);
	q2->release();
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.611103", 2, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.611104", 2, 20240109100000000ull) == NULL);

	// 历史 tick 跨日期
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/641100.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_CMP_V2, pack_ticks(cross_ticks(20240109, 20240108, 40)))));
	WTSTickSlice* hts = _reader.readTickSlice("SSE.STK.641100", 2, 20240109100000000ull);
	ASSERT_TRUE(hts != NULL);
	EXPECT_EQ(r52::flatten_ticks(hts).size(), (std::size_t)2);
	hts->release();

	// 短 tick 文件二次访问
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/641101.dsb", string(4, '\0')));
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.641101", 2, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.641101", 2, 20240109100000000ull) == NULL);
}

// WtDataReader.cpp: L978-984 / L1074-1078 / L1132-1140 / L1181-1258 / L1616-1624
TEST_F(R100ReaderCrossDayTest, R100ReaderIntegratedHotLoaderAndAdjustedCatch)
{
	_hot.set_enabled(true);
	_hot.set_raw("STK2401");
	_hot.add_section("STK2401", 20240101, 20240110, 2.0);

	vector<WTSBarStruct> raw;
	raw.push_back(make_bar(20240105, r52::min_bar_time(20240105, 930), 1));
	raw.push_back(make_bar(20240105, r52::min_bar_time(20240105, 935), 2));
	_loader.add_raw("SSE.STK.2401", raw);

	init_reader(&_loader);

	// 前复权：baseFactor 取最后一段因子（HOT-），loader 提供分月原始 bar
	WTSKlineSlice* qfq = _reader.readKlineSlice("SSE.STK.HOT-", KP_Minute5, 10, 202401101500ull);
	ASSERT_TRUE(qfq != NULL);
	EXPECT_EQ(qfq->size(), 2u);
	qfq->release();

	// 后复权：基础因子为 1，缓存条数一致
	WTSKlineSlice* hfq = _reader.readKlineSlice("SSE.STK.HOT+", KP_Minute5, 10, 202401101500ull);
	ASSERT_TRUE(hfq != NULL);
	EXPECT_EQ(hfq->size(), 2u);
	hfq->release();

	// 复权文件只有块头：cacheAdjustedStkBars 取 at(barcnt-1) 抛 out_of_range，
	// 被 cacheHisBarsFromFile 的 catch(std::exception&) 收口，最终是空切片。
	ASSERT_TRUE(write_bytes("his/day/SSE/600000-.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, string())));
	WTSKlineSlice* empty = _reader.readKlineSlice("SSE.STK.600000-", KP_DAY, 2, 202401101500ull);
	ASSERT_TRUE(empty != NULL);
	EXPECT_EQ(empty->size(), 0u);
	EXPECT_GE(_sink.count_of(LL_ERROR), 1u);
	empty->release();
}

} // namespace r100
