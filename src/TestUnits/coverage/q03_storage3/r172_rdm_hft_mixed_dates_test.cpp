// r172 / 历史逐笔块内 action_date 跨日比较
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange 的 lower_bound 日期比较 :748-753（:750 为跨日臂）
//  - readOrdDtlSliceByRange 的 lower_bound 日期比较 :923-928（:925）
//  - readTransSliceByRange 的 lower_bound 日期比较 :1097-1102（:1099）
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readOrdQueSlice 的 lower_bound 日期比较 :594-599（:597-598 为同日臂）
//  - readTransSlice 的 lower_bound 日期比较 :908-913（:911-912 为同日臂）
//
// 所有块均由公开的 make_block/pack_items 拼装，读端只走 init + 公开读取接口。
#include "q03_storage3_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace r172 {

using namespace q03;
using std::string;
using std::vector;

namespace {

WTSOrdQueStruct make_que(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSOrdQueStruct item;
	std::memset(&item, 0, sizeof(item));
	snprintf(item.exchg, sizeof(item.exchg), "%s", "SSE");
	snprintf(item.code, sizeof(item.code), "%s", "600000");
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.price = 10.0 + seq;
	item.order_items = 2 + seq;
	item.qsize = 5;
	item.side = BDT_Buy;
	return item;
}

WTSOrdDtlStruct make_dtl(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSOrdDtlStruct item;
	std::memset(&item, 0, sizeof(item));
	snprintf(item.exchg, sizeof(item.exchg), "%s", "SSE");
	snprintf(item.code, sizeof(item.code), "%s", "600000");
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.price = 20.0 + seq;
	item.volume = 100.0 + seq;
	item.index = 1000 + seq;
	item.side = BDT_Sell;
	item.otype = ODT_LimitPrice;
	item.orderid = 500000 + seq;
	return item;
}

WTSTransStruct make_trs(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSTransStruct item;
	std::memset(&item, 0, sizeof(item));
	snprintf(item.exchg, sizeof(item.exchg), "%s", "SSE");
	snprintf(item.code, sizeof(item.code), "%s", "600000");
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.price = 30.0 + seq;
	item.volume = 200.0 + seq;
	item.index = 2000 + seq;
	item.ttype = TT_Match;
	item.side = BDT_Sell;
	item.askorder = 600000 + seq;
	item.bidorder = 700000 + seq;
	return item;
}

// RDM 读端：endTDate 用 20240109（当前交易日 20240110，走历史文件分支），
// 块内 action_date 全部早于查询日 20240109，使 lower_bound 比较器进入跨日臂
// （:750/:925/:1099，而不是同日的 action_time 臂）。
void write_rdm_blocks()
{
	vector<WTSOrdQueStruct> que = {
		make_que(20240107, 20240107, 93000000, 1),
		make_que(20240108, 20240108, 93100000, 2),
		make_que(20240108, 20240108, 93200000, 3) };
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	vector<WTSOrdDtlStruct> dtl = {
		make_dtl(20240107, 20240107, 93000000, 11),
		make_dtl(20240108, 20240108, 93100000, 12),
		make_dtl(20240108, 20240108, 93200000, 13) };
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	vector<WTSTransStruct> trs = {
		make_trs(20240107, 20240107, 93000000, 21),
		make_trs(20240108, 20240108, 93100000, 22),
		make_trs(20240108, 20240108, 93200000, 23) };
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600000.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trs))));
}

} // namespace

class R172RdmMixedDatesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r172rdm"));
		_bd.set_cur_tdate(20240110);
	}

	// 每种历史逐笔各用一个新 reader：WtRdmDtReader 的三个历史读取入口
	// 共用一个 `_his_ordque_map` 做装载判断（BUG-R74-001/R100-001），
	// 同一 reader 先读 queue 后，orders/trans 会跳过装载返回空块。
	// 本用例的目标是 lower_bound 的跨日比较，不是历史缓存复用，故按入口隔离。
	std::unique_ptr<WtRdmDtReader> make_reader()
	{
		std::unique_ptr<WtRdmDtReader> r(new WtRdmDtReader());
		r->init(NULL, &_sink);
		return r;
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	RdmSink				_sink{ &_bd, &_hot };
};

TEST_F(R172RdmMixedDatesTest, R172RdmMixedActionDatesHisHftBlocks)
{
	write_rdm_blocks();

	const uint64_t stime = 20240108090000000ull;
	const uint64_t etime = 20240109150000000ull;

	std::unique_ptr<WtRdmDtReader> queReader = make_reader();
	WTSOrdQueSlice* que = queReader->readOrdQueSliceByRange("SSE.STK.600000", stime, etime);
	ASSERT_TRUE(que != NULL);
	EXPECT_EQ(que->size(), 3u);
	EXPECT_EQ(que->at(0)->action_date, 20240107u);
	EXPECT_EQ(que->at(2)->action_time, 93200000u);
	que->release();

	std::unique_ptr<WtRdmDtReader> dtlReader = make_reader();
	WTSOrdDtlSlice* dtl = dtlReader->readOrdDtlSliceByRange("SSE.STK.600000", stime, etime);
	ASSERT_TRUE(dtl != NULL);
	EXPECT_EQ(dtl->size(), 3u);
	EXPECT_EQ(dtl->at(0)->action_date, 20240107u);
	EXPECT_EQ(dtl->at(2)->index, 1013u);
	dtl->release();

	std::unique_ptr<WtRdmDtReader> trsReader = make_reader();
	WTSTransSlice* trs = trsReader->readTransSliceByRange("SSE.STK.600000", stime, etime);
	ASSERT_TRUE(trs != NULL);
	EXPECT_EQ(trs->size(), 3u);
	EXPECT_EQ(trs->at(0)->action_date, 20240107u);
	EXPECT_EQ(trs->at(2)->askorder, 600023u);
	trs->release();
}

class R172ReaderMixedDatesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r172rd"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);

		const string cfgfile = _root + "reader.json";
		ASSERT_TRUE(write_bytes(cfgfile, fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root)));
	}

	// 与 Rdm 用例同理：WtDataReader 的 queue/orders/trans 也共用
	// `_his_ordque_map` 做装载判断（BUG-R74-001/R100-001），
	// 同一 reader 先读 queue 会让 orders/trans 跳过装载返回空块。
	std::unique_ptr<WtDataReader> make_reader()
	{
		WTSVariant* cfg = WTSCfgLoader::load_from_file((_root + "reader.json").c_str());
		EXPECT_TRUE(cfg != NULL);
		if (cfg == NULL)
			return std::unique_ptr<WtDataReader>();
		std::unique_ptr<WtDataReader> r(new WtDataReader());
		r->init(cfg, &_sink, NULL);
		cfg->release();
		return r;
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	ReaderSink			_sink{ &_bd, &_hot };
};

// WtDataReader.cpp:594-599 / 908-913（最后一条与查询日同日，比较器走 action_time 臂）
TEST_F(R172ReaderMixedDatesTest, R172ReaderMixedActionDatesHisHftSlices)
{
	vector<WTSOrdQueStruct> que = {
		make_que(20240108, 20240108, 93000000, 1),
		make_que(20240109, 20240109, 93100000, 2),
		make_que(20240109, 20240109, 93200000, 3) };
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	vector<WTSOrdDtlStruct> dtl = {
		make_dtl(20240108, 20240108, 93000000, 11),
		make_dtl(20240109, 20240109, 93100000, 12),
		make_dtl(20240109, 20240109, 93200000, 13) };
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	vector<WTSTransStruct> trs = {
		make_trs(20240108, 20240108, 93000000, 21),
		make_trs(20240109, 20240109, 93100000, 22),
		make_trs(20240109, 20240109, 93200000, 23) };
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600000.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trs))));

	const uint64_t etime = 20240109150000000ull;

	std::unique_ptr<WtDataReader> queReader = make_reader();
	ASSERT_TRUE(queReader != NULL);
	WTSOrdQueSlice* qs = queReader->readOrdQueSlice("SSE.STK.600000", 10, etime);
	ASSERT_TRUE(qs != NULL);
	EXPECT_EQ(qs->size(), 3u);
	EXPECT_EQ(qs->at(0)->action_date, 20240108u);
	EXPECT_EQ(qs->at(2)->action_time, 93200000u);
	qs->release();

	std::unique_ptr<WtDataReader> dtlReader = make_reader();
	ASSERT_TRUE(dtlReader != NULL);
	WTSOrdDtlSlice* ds = dtlReader->readOrdDtlSlice("SSE.STK.600000", 10, etime);
	ASSERT_TRUE(ds != NULL);
	EXPECT_EQ(ds->size(), 3u);
	EXPECT_EQ(ds->at(2)->index, 1013u);
	ds->release();

	std::unique_ptr<WtDataReader> trsReader = make_reader();
	ASSERT_TRUE(trsReader != NULL);
	WTSTransSlice* ts = trsReader->readTransSlice("SSE.STK.600000", 10, etime);
	ASSERT_TRUE(ts != NULL);
	EXPECT_EQ(ts->size(), 3u);
	EXPECT_EQ(ts->at(2)->askorder, 600023u);
	ts->release();
}

} // namespace r172
