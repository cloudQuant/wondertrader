// r132 / WtDataReader：逐笔历史成功装配、跨 action_date 比较、缓存块空指针守卫、
//       主力连续切段与热合约尾部重叠。
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readTickSlice his 分支比较器跨 action_date :437-442
//  - readOrdQueSlice his 成功路径（CMP_V2 解压 + 裁剪）:565-611
//  - readOrdDtlSlice his 成功路径与跨 action_date 比较 :722-768
//  - readTransSlice his 成功路径（CMP_V2 解压 + 裁剪）:879-925
//  - 历史块已缓存但 _block==NULL 的守卫 :585-586；tcnt<=0 :591-592/:749/:905
//  - cacheIntegratedBars 的 min1 重叠段 :1098-1108/:1163 与 day 重叠段 :1114-1122、
//    热合约尾部追加 :1262-1265
//
// 手法：
//  - 一律走公开 API（init/read*），不访问私有成员，没有访问权限宏；
//  - HFT 解压/裁剪用例以伪随机字段保持压缩载荷难压缩；独立 R132 回归另用高度重复的
//    记录断言解压块远大于压缩文件，覆盖实际扩容、缓存隔离、复读及损坏帧处理；
//  - 临时目录由 q03::Sandbox 管理，TearDown remove_all，不写仓库。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"
#include <cstdint>
#include <stdexcept>

#if defined(WT_RDM_DT_READER_TEST_HOOKS)
void wt_r132_fail_next_allocation_for_test(int stage);
#endif

namespace r132 {

using namespace q03;
using std::string;
using std::vector;

namespace {

uint32_t g_rng = 0x9e3779b9u;

uint32_t rnd()
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return g_rng;
}

void fill_bytes(char* p, std::size_t n)
{
	for (std::size_t i = 0; i < n; i++)
		p[i] = (char)(rnd() >> 24);
}

double rnd_double()
{
	const uint64_t bits = ((uint64_t)rnd() << 32) | rnd();
	double v = 0.0;
	memcpy(&v, &bits, sizeof(v));
	return v;
}

// 让一条委托队列记录几乎不可压缩（只保留 action/trading 时间用于排序断言）
void scramble_ordque(WTSOrdQueStruct& s)
{
	fill_bytes(s.exchg, sizeof(s.exchg));
	fill_bytes(s.code, sizeof(s.code));
	s.price = rnd_double();
	s.order_items = rnd();
	s.qsize = 3;
	for (uint32_t i = 0; i < 50; i++)
		s.volumes[i] = rnd();
}

void scramble_orddtl(WTSOrdDtlStruct& s)
{
	fill_bytes(s.exchg, sizeof(s.exchg));
	fill_bytes(s.code, sizeof(s.code));
	s.index = rnd();
	s.price = rnd_double();
	s.volume = rnd();
	s.orderid = rnd();
	s.side = (rnd() & 1) ? BDT_Buy : BDT_Sell;
	s.otype = (rnd() & 1) ? ODT_LimitPrice : ODT_AnyPrice;
}

void scramble_trans(WTSTransStruct& s)
{
	fill_bytes(s.exchg, sizeof(s.exchg));
	fill_bytes(s.code, sizeof(s.code));
	s.index = rnd();
	s.price = rnd_double();
	s.volume = rnd();
	s.askorder = rnd();
	s.bidorder = rnd();
	s.side = (rnd() & 1) ? BDT_Buy : BDT_Sell;
	s.ttype = (rnd() & 1) ? TT_Match : TT_Cancel;
}

uint64_t min_time(uint32_t date, uint32_t hhmm)
{
	return TimeUtils::timeToMinBar(date, hhmm);
}

} // namespace

class R132ReaderHisTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r132rd"));
		_root = _sandbox->root_abs();
		_bd.set_cur_tdate(20240110);
		WTSVariant* cfg = _cfg();
		_reader.init(cfg, &_sink, NULL);
		if (cfg != NULL)
			cfg->release();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	WTSVariant* _cfg()
	{
		const string file = _root + "reader.json";
		EXPECT_TRUE(write_bytes(file,
			fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root)));
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		EXPECT_TRUE(cfg != NULL);
		return cfg;
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string				_root;
	r52::BDMgr			_bd;
	r52::HotMgr			_hot;
	r52::ReaderSink			_sink{ &_bd, &_hot };
	WtDataReader			_reader;
};

// WtDataReader.cpp: readTickSlice his 比较器 L437-449
TEST_F(R132ReaderHisTest, R132ReaderHisTickCrossActionDate)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240109, 20240108, 210000000, 1));
	ticks.push_back(make_tick(20240109, 20240108, 230000000, 2));
	ticks.push_back(make_tick(20240109, 20240109, 90000000, 3));
	ticks.push_back(make_tick(20240109, 20240109, 100000000, 4));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/600003.dsb",
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(ticks))));

	WTSTickSlice* s = _reader.readTickSlice("SSE.STK.600003", 2, 20240109103000000ull);
	ASSERT_TRUE(s != NULL);
	const vector<WTSTickStruct> got = r52::flatten_ticks(s);
	ASSERT_EQ(got.size(), (std::size_t)2);
	EXPECT_EQ(got[0].action_date, 20240109u);
	EXPECT_EQ(got[0].action_time, 90000000u);
	EXPECT_EQ(got[1].action_time, 100000000u);
	s->release();
}

// WtDataReader.cpp: readOrdQueSlice L565-611 / readOrdDtlSlice L722-768 / readTransSlice L879-925
TEST_F(R132ReaderHisTest, R132ReaderHisHftCompressedRoundTrip)
{
	// 队列：两条记录跨 action_date，lower_bound 首轮就命中日期分支
	vector<WTSOrdQueStruct> que;
	WTSOrdQueStruct q1 = make_ordque(20240109, 20240108, 210000000, 1);
	WTSOrdQueStruct q2 = make_ordque(20240109, 20240109, 93100000, 2);
	scramble_ordque(q1);
	scramble_ordque(q2);
	que.push_back(q1);
	que.push_back(q2);
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600000.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600000", 2, 20240109150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	EXPECT_EQ(reinterpret_cast<std::uintptr_t>(sq->at(0)) % alignof(WTSOrdQueStruct), 0u);
	EXPECT_EQ(sq->at(1), sq->at(0) + 1);
	EXPECT_EQ(sq->at(0)->action_date, 20240108u);
	EXPECT_EQ(sq->at(1)->action_date, 20240109u);
	EXPECT_EQ(sq->at(1)->action_time, 93100000u);
	sq->release();

	// 明细：另一个 stdCode，避免 readOrdDtlSlice 误用 _his_ordque_map 时被前一次队列缓存挡住
	vector<WTSOrdDtlStruct> dtl;
	WTSOrdDtlStruct d1 = make_orddtl(20240109, 20240108, 210000000, 11);
	WTSOrdDtlStruct d2 = make_orddtl(20240109, 20240109, 93100000, 12);
	scramble_orddtl(d1);
	scramble_orddtl(d2);
	dtl.push_back(d1);
	dtl.push_back(d2);
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600001.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600001", 2, 20240109150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 2u);
	EXPECT_EQ(reinterpret_cast<std::uintptr_t>(sd->at(0)) % alignof(WTSOrdDtlStruct), 0u);
	EXPECT_EQ(sd->at(1), sd->at(0) + 1);
	EXPECT_EQ(sd->at(0)->action_date, 20240108u);
	EXPECT_EQ(sd->at(1)->action_date, 20240109u);
	sd->release();

	// 成交：独立 stdCode
	vector<WTSTransStruct> trs;
	WTSTransStruct t1 = make_trans(20240109, 20240108, 210000000, 21);
	WTSTransStruct t2 = make_trans(20240109, 20240109, 93100000, 22);
	scramble_trans(t1);
	scramble_trans(t2);
	trs.push_back(t1);
	trs.push_back(t2);
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600002.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trs))));

	WTSTransSlice* st = _reader.readTransSlice("SSE.STK.600002", 2, 20240109150000000ull);
	ASSERT_TRUE(st != NULL);
	ASSERT_EQ(st->size(), 2u);
	EXPECT_EQ(reinterpret_cast<std::uintptr_t>(st->at(0)) % alignof(WTSTransStruct), 0u);
	EXPECT_EQ(st->at(1), st->at(0) + 1);
	EXPECT_EQ(st->at(0)->action_date, 20240108u);
	EXPECT_EQ(st->at(1)->action_date, 20240109u);
	st->release();
}

// BUG-R132-001：解压后的记录块远大于 CMP_V2 文件，覆盖缓存字符串扩容路径。
TEST_F(R132ReaderHisTest, R132ReaderHisCompressibleQueueExpansion)
{
	vector<WTSOrdQueStruct> que;
	for (uint32_t i = 0; i < 64; i++)
	{
		const uint32_t minuteOfDay = 570 + i;
		const uint32_t hhmm = minuteOfDay / 60 * 100 + minuteOfDay % 60;
		WTSOrdQueStruct item = make_ordque(20240109, 20240109, hhmm * 100000u, 41 + i);
		item.price = 50.0;
		item.order_items = 3;
		item.qsize = 5;
		for (uint32_t j = 0; j < 50; j++)
			item.volumes[j] = 10;
		que.push_back(item);
	}
	const string payload = pack_items(que);
	const string compressed = make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, payload);
	ASSERT_LT(compressed.size() * 4, sizeof(HisOrdQueBlock) + payload.size())
		<< "fixture must expand well beyond the compressed input allocation";
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600040.dsb", compressed));

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600040", 2, 20240109150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	EXPECT_EQ(sq->at(0)->action_time, 103200000u);
	EXPECT_EQ(sq->at(1)->action_time, 103300000u);
	sq->release();
}

TEST_F(R132ReaderHisTest, R132AlignedHistoricalSlicesPreserveExclusiveEnd)
{
	const uint32_t date = 20240109;
	const uint32_t firstTime = 93000000;
	const uint32_t secondTime = 93100000;
	const vector<WTSOrdQueStruct> queues = {
		make_ordque(date, date, firstTime, 1), make_ordque(date, date, secondTime, 2)};
	const vector<WTSOrdDtlStruct> details = {
		make_orddtl(date, date, firstTime, 1), make_orddtl(date, date, secondTime, 2)};
	const vector<WTSTransStruct> transactions = {
		make_trans(date, date, firstTime, 1), make_trans(date, date, secondTime, 2)};
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600042.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(queues))));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600042.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(details))));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600042.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(transactions))));
	const char* code = "SSE.STK.600042";
	const uint64_t first = uint64_t(date) * 1000000000ULL + firstTime;
	const uint64_t second = uint64_t(date) * 1000000000ULL + secondTime;
	EXPECT_EQ(_reader.readOrdQueSlice(code, 2, first), nullptr);
	EXPECT_EQ(_reader.readOrdDtlSlice(code, 2, first), nullptr);
	EXPECT_EQ(_reader.readTransSlice(code, 2, first), nullptr);
	WTSOrdQueSlice* queue = _reader.readOrdQueSlice(code, 2, second);
	WTSOrdDtlSlice* detail = _reader.readOrdDtlSlice(code, 2, second);
	WTSTransSlice* trans = _reader.readTransSlice(code, 2, second);
	ASSERT_NE(queue, nullptr);
	ASSERT_NE(detail, nullptr);
	ASSERT_NE(trans, nullptr);
	EXPECT_EQ(queue->size(), 1u);
	EXPECT_EQ(detail->size(), 1u);
	EXPECT_EQ(trans->size(), 1u);
	EXPECT_EQ(queue->at(0)->action_time, firstTime);
	EXPECT_EQ(detail->at(0)->action_time, firstTime);
	EXPECT_EQ(trans->at(0)->action_time, firstTime);
	queue->release();
	detail->release();
	trans->release();
}

TEST_F(R132ReaderHisTest, R132AlignedSliceSurvivesOtherCacheInsertions)
{
	const uint32_t date = 20240109;
	const uint64_t endTime = 20240109150000000ULL;
	const vector<WTSOrdQueStruct> records = {make_ordque(date, date, 93000000, 1)};
	const string block = make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(records));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610000.dsb", block));
	WTSOrdQueSlice* held = _reader.readOrdQueSlice("SSE.STK.610000", 1, endTime);
	ASSERT_NE(held, nullptr);
	const WTSOrdQueStruct* heldItem = held->at(0);
	ASSERT_NE(heldItem, nullptr);
	for (unsigned i = 1; i <= 128; ++i)
	{
		const string rawCode = std::to_string(610000 + i);
		ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/" + rawCode + ".dsb", block));
		WTSOrdQueSlice* current = _reader.readOrdQueSlice(("SSE.STK." + rawCode).c_str(), 1, endTime);
		ASSERT_NE(current, nullptr);
		current->release();
	}
	EXPECT_EQ(held->at(0), heldItem);
	EXPECT_EQ(held->at(0)->action_time, 93000000u);
	held->release();
}

TEST_F(R132ReaderHisTest, R132HistoricalBadFilesCanBeReplacedAndRetried)
{
	const uint32_t date = 20240109;
	const uint64_t endTime = 20240109150000000ULL;
	const string code = "SSE.STK.610200";
	const string queueFile = "his/queue/SSE/20240109/610200.dsb";
	const string detailFile = "his/orders/SSE/20240109/610200.dsb";
	const string transFile = "his/trans/SSE/20240109/610200.dsb";
	ASSERT_TRUE(write_bytes(queueFile, "bad"));
	ASSERT_TRUE(write_bytes(detailFile, "bad"));
	ASSERT_TRUE(write_bytes(transFile, "bad"));
	EXPECT_EQ(_reader.readOrdQueSlice(code.c_str(), 1, endTime), nullptr);
	EXPECT_EQ(_reader.readOrdDtlSlice(code.c_str(), 1, endTime), nullptr);
	EXPECT_EQ(_reader.readTransSlice(code.c_str(), 1, endTime), nullptr);
	const vector<WTSOrdQueStruct> queues = {make_ordque(date, date, 93000000, 1)};
	const vector<WTSOrdDtlStruct> details = {make_orddtl(date, date, 93000000, 1)};
	const vector<WTSTransStruct> trans = {make_trans(date, date, 93000000, 1)};
	ASSERT_TRUE(write_bytes(queueFile, make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(queues))));
	ASSERT_TRUE(write_bytes(detailFile, make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(details))));
	ASSERT_TRUE(write_bytes(transFile, make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));
	WTSOrdQueSlice* q = _reader.readOrdQueSlice(code.c_str(), 1, endTime);
	WTSOrdDtlSlice* d = _reader.readOrdDtlSlice(code.c_str(), 1, endTime);
	WTSTransSlice* t = _reader.readTransSlice(code.c_str(), 1, endTime);
	EXPECT_NE(q, nullptr);
	EXPECT_NE(d, nullptr);
	EXPECT_NE(t, nullptr);
	if (q) { EXPECT_EQ(q->at(0)->action_time, 93000000u); q->release(); }
	if (d) { EXPECT_EQ(d->at(0)->action_time, 93000000u); d->release(); }
	if (t) { EXPECT_EQ(t->at(0)->action_time, 93000000u); t->release(); }
}

#if defined(WT_RDM_DT_READER_TEST_HOOKS)
TEST_F(R132ReaderHisTest, R132AllocationFailureDoesNotPublishOrInvalidateHeldSlices)
{
	struct ResetFailureInjection {
		~ResetFailureInjection() { wt_r132_fail_next_allocation_for_test(0); }
	} resetFailureInjection;
	const uint32_t date = 20240109;
	const uint64_t endTime = 20240109150000000ULL;
	const vector<WTSOrdQueStruct> queues = {make_ordque(date, date, 93000000, 1)};
	const vector<WTSOrdDtlStruct> details = {make_orddtl(date, date, 93000000, 1)};
	const vector<WTSTransStruct> trans = {make_trans(date, date, 93000000, 1)};
	const string qBlock = make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(queues));
	const string dBlock = make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(details));
	const string tBlock = make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans));
	const auto writeAll = [&](const string& raw) {
		return write_bytes("his/queue/SSE/20240109/" + raw + ".dsb", qBlock) &&
			write_bytes("his/orders/SSE/20240109/" + raw + ".dsb", dBlock) &&
			write_bytes("his/trans/SSE/20240109/" + raw + ".dsb", tBlock);
	};
	ASSERT_TRUE(writeAll("610210"));
	WTSOrdQueSlice* heldQ = _reader.readOrdQueSlice("SSE.STK.610210", 1, endTime);
	WTSOrdDtlSlice* heldD = _reader.readOrdDtlSlice("SSE.STK.610210", 1, endTime);
	WTSTransSlice* heldT = _reader.readTransSlice("SSE.STK.610210", 1, endTime);
	ASSERT_NE(heldQ, nullptr);
	ASSERT_NE(heldD, nullptr);
	ASSERT_NE(heldT, nullptr);
	const auto* qPtr = heldQ->at(0);
	const auto* dPtr = heldD->at(0);
	const auto* tPtr = heldT->at(0);
	for (int stage = 1; stage <= 2; ++stage)
	{
		const string raw = std::to_string(610210 + stage);
		const string code = "SSE.STK." + raw;
		ASSERT_TRUE(writeAll(raw));
		wt_r132_fail_next_allocation_for_test(stage);
		EXPECT_EQ(_reader.readOrdQueSlice(code.c_str(), 1, endTime), nullptr);
		wt_r132_fail_next_allocation_for_test(stage);
		EXPECT_EQ(_reader.readOrdDtlSlice(code.c_str(), 1, endTime), nullptr);
		wt_r132_fail_next_allocation_for_test(stage);
		EXPECT_EQ(_reader.readTransSlice(code.c_str(), 1, endTime), nullptr);
		EXPECT_EQ(heldQ->at(0), qPtr);
		EXPECT_EQ(heldD->at(0), dPtr);
		EXPECT_EQ(heldT->at(0), tPtr);
		EXPECT_EQ(qPtr->action_time, 93000000u);
		EXPECT_EQ(dPtr->action_time, 93000000u);
		EXPECT_EQ(tPtr->action_time, 93000000u);
		WTSOrdQueSlice* q = _reader.readOrdQueSlice(code.c_str(), 1, endTime);
		WTSOrdDtlSlice* d = _reader.readOrdDtlSlice(code.c_str(), 1, endTime);
		WTSTransSlice* t = _reader.readTransSlice(code.c_str(), 1, endTime);
		ASSERT_NE(q, nullptr);
		ASSERT_NE(d, nullptr);
		ASSERT_NE(t, nullptr);
		q->release();
		d->release();
		t->release();
	}
	heldQ->release();
	heldD->release();
	heldT->release();
}

TEST_F(R132ReaderHisTest, R132UnexpectedSliceFailureLogsAndLoggerFailureDoesNotEscape)
{
	struct ResetFailureInjection {
		~ResetFailureInjection() { wt_r132_fail_next_allocation_for_test(0); }
	} resetFailureInjection;
	const uint32_t date = 20240109;
	const uint64_t endTime = 20240109150000000ULL;
	const char* code = "SSE.STK.610220";
	const vector<WTSOrdQueStruct> queues = {make_ordque(date, date, 93000000, 1)};
	const vector<WTSOrdDtlStruct> details = {make_orddtl(date, date, 93000000, 1)};
	const vector<WTSTransStruct> trans = {make_trans(date, date, 93000000, 1)};
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610220.dsb", make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(queues))));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/610220.dsb", make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(details))));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/610220.dsb", make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));
	wt_r132_fail_next_allocation_for_test(3);
	EXPECT_EQ(_reader.readOrdQueSlice(code, 1, endTime), nullptr);
	wt_r132_fail_next_allocation_for_test(3);
	EXPECT_EQ(_reader.readOrdDtlSlice(code, 1, endTime), nullptr);
	wt_r132_fail_next_allocation_for_test(3);
	EXPECT_EQ(_reader.readTransSlice(code, 1, endTime), nullptr);
	EXPECT_TRUE(_sink.has_log_containing("Unexpected failure reading historical queue slice for SSE.STK.610220"));
	EXPECT_TRUE(_sink.has_log_containing("Unexpected failure reading historical detail slice for SSE.STK.610220"));
	EXPECT_TRUE(_sink.has_log_containing("Unexpected failure reading historical transaction slice for SSE.STK.610220"));

	class ThrowingSink : public r52::ReaderSink {
	public:
		ThrowingSink(IBaseDataMgr* bd, IHotMgr* hot) : r52::ReaderSink(bd, hot) {}
		void reader_log(WTSLogLevel ll, const char* message) override
		{
			if (throwOnLog) { ++attempts; throw std::runtime_error("injected log sink failure"); }
			r52::ReaderSink::reader_log(ll, message);
		}
		bool throwOnLog = false;
		int attempts = 0;
	} sink(&_bd, &_hot);
	WtDataReader reader;
	WTSVariant* cfg = _cfg();
	ASSERT_NE(cfg, nullptr);
	reader.init(cfg, &sink, NULL);
	cfg->release();
	WTSOrdQueSlice* q = reader.readOrdQueSlice(code, 1, endTime);
	WTSOrdDtlSlice* d = reader.readOrdDtlSlice(code, 1, endTime);
	WTSTransSlice* t = reader.readTransSlice(code, 1, endTime);
	ASSERT_NE(q, nullptr);
	ASSERT_NE(d, nullptr);
	ASSERT_NE(t, nullptr);
	q->release(); d->release(); t->release();
	sink.throwOnLog = true;
	wt_r132_fail_next_allocation_for_test(3);
	EXPECT_NO_THROW(EXPECT_EQ(reader.readOrdQueSlice(code, 1, endTime), nullptr));
	wt_r132_fail_next_allocation_for_test(3);
	EXPECT_NO_THROW(EXPECT_EQ(reader.readOrdDtlSlice(code, 1, endTime), nullptr));
	wt_r132_fail_next_allocation_for_test(3);
	EXPECT_NO_THROW(EXPECT_EQ(reader.readTransSlice(code, 1, endTime), nullptr));
	EXPECT_EQ(sink.attempts, 3);
	sink.throwOnLog = false;
	q = reader.readOrdQueSlice(code, 1, endTime);
	d = reader.readOrdDtlSlice(code, 1, endTime);
	t = reader.readTransSlice(code, 1, endTime);
	ASSERT_NE(q, nullptr);
	ASSERT_NE(d, nullptr);
	ASSERT_NE(t, nullptr);
	EXPECT_EQ(q->at(0)->action_time, 93000000u);
	EXPECT_EQ(d->at(0)->action_time, 93000000u);
	EXPECT_EQ(t->at(0)->action_time, 93000000u);
	q->release(); d->release(); t->release();
}
#endif

// BUG-R132-002：逐笔队列、委托明细、成交必须使用各自独立的历史块缓存。
// 同一合约/交易日依次访问不同数据类型，并在不同顺序重复读取，确认不会误命中别的类型缓存。
TEST_F(R132ReaderHisTest, R132ReaderHisOrderQueueDetailAndTransCachesAreIndependent)
{
	const auto action_time = [](uint32_t minuteOfDay) {
		const uint32_t hhmm = minuteOfDay / 60 * 100 + minuteOfDay % 60;
		return hhmm * 100000u;
	};

	vector<WTSOrdQueStruct> que;
	for (uint32_t i = 0; i < 64; i++)
	{
		WTSOrdQueStruct item = make_ordque(20240109, 20240109, action_time(570 + i), 51 + i);
		item.price = 50.5;
		item.order_items = 333;
		item.qsize = 45;
		for (uint32_t j = 0; j < 50; j++)
			item.volumes[j] = 100 + j;
		que.push_back(item);
	}
	const string quePayload = pack_items(que);
	const string queBlock = make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, quePayload);
	ASSERT_LT(queBlock.size() * 4, sizeof(HisOrdQueBlock) + quePayload.size());
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600041.dsb", queBlock));

	vector<WTSOrdDtlStruct> dtl;
	for (uint32_t i = 0; i < 64; i++)
	{
		WTSOrdDtlStruct item = make_orddtl(20240109, 20240109, action_time(600 + i), 115 + i);
		item.index = 1000 + i;
		item.price = 51.25;
		item.volume = 1234;
		item.orderid = 700123;
		item.side = BDT_Sell;
		item.otype = ODT_AnyPrice;
		dtl.push_back(item);
	}
	const string dtlPayload = pack_items(dtl);
	const string dtlBlock = make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, dtlPayload);
	ASSERT_LT(dtlBlock.size() * 4, sizeof(HisOrdDtlBlock) + dtlPayload.size());
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600041.dsb", dtlBlock));

	vector<WTSTransStruct> trans;
	for (uint32_t i = 0; i < 64; i++)
	{
		WTSTransStruct item = make_trans(20240109, 20240109, action_time(630 + i), 179 + i);
		item.index = 2000 + i;
		item.ttype = TT_Cancel;
		item.side = BDT_Sell;
		item.price = 52.75;
		item.volume = 2345;
		item.askorder = 300123;
		item.bidorder = 400456;
		trans.push_back(item);
	}
	const string transPayload = pack_items(trans);
	const string transBlock = make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, transPayload);
	ASSERT_LT(transBlock.size() * 4, sizeof(HisTransBlock) + transPayload.size());
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600041.dsb", transBlock));

	const auto expectQueFields = [&](const WTSOrdQueStruct* actual, uint32_t record) {
		if (actual == NULL)
		{
			ADD_FAILURE() << "missing queue record " << record;
			return;
		}
		const uint32_t seq = 51 + record;
		const string expectedCode = std::to_string(600000 + seq);
		EXPECT_STREQ(actual->exchg, "SSE");
		EXPECT_STREQ(actual->code, expectedCode.c_str());
		EXPECT_EQ(actual->trading_date, 20240109u);
		EXPECT_EQ(actual->action_date, 20240109u);
		EXPECT_EQ(actual->action_time, action_time(570 + record));
		EXPECT_EQ(actual->side, seq % 2 == 0 ? BDT_Buy : BDT_Sell);
		EXPECT_DOUBLE_EQ(actual->price, 50.5);
		EXPECT_EQ(actual->order_items, 333u);
		EXPECT_EQ(actual->qsize, 45u);
		for (uint32_t j = 0; j < 50; j++)
			EXPECT_EQ(actual->volumes[j], 100u + j) << "volume index " << j;
	};
	const auto expectDtlFields = [&](const WTSOrdDtlStruct* actual, uint32_t record) {
		if (actual == NULL)
		{
			ADD_FAILURE() << "missing detail record " << record;
			return;
		}
		const uint32_t seq = 115 + record;
		const string expectedCode = std::to_string(600000 + seq);
		EXPECT_STREQ(actual->exchg, "SSE");
		EXPECT_STREQ(actual->code, expectedCode.c_str());
		EXPECT_EQ(actual->trading_date, 20240109u);
		EXPECT_EQ(actual->action_date, 20240109u);
		EXPECT_EQ(actual->action_time, action_time(600 + record));
		EXPECT_EQ(actual->index, 1000u + record);
		EXPECT_DOUBLE_EQ(actual->price, 51.25);
		EXPECT_EQ(actual->volume, 1234u);
		EXPECT_EQ(actual->side, BDT_Sell);
		EXPECT_EQ(actual->otype, ODT_AnyPrice);
		EXPECT_EQ(actual->orderid, 700123);
	};
	const auto expectTransFields = [&](const WTSTransStruct* actual, uint32_t record) {
		if (actual == NULL)
		{
			ADD_FAILURE() << "missing transaction record " << record;
			return;
		}
		const uint32_t seq = 179 + record;
		const string expectedCode = std::to_string(600000 + seq);
		EXPECT_STREQ(actual->exchg, "SSE");
		EXPECT_STREQ(actual->code, expectedCode.c_str());
		EXPECT_EQ(actual->trading_date, 20240109u);
		EXPECT_EQ(actual->action_date, 20240109u);
		EXPECT_EQ(actual->action_time, action_time(630 + record));
		EXPECT_EQ(actual->index, 2000u + record);
		EXPECT_EQ(actual->ttype, TT_Cancel);
		EXPECT_EQ(actual->side, BDT_Sell);
		EXPECT_DOUBLE_EQ(actual->price, 52.75);
		EXPECT_EQ(actual->volume, 2345u);
		EXPECT_EQ(actual->askorder, 300123);
		EXPECT_EQ(actual->bidorder, 400456);
	};

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 2u);
	expectDtlFields(sd->at(0), 62);
	expectDtlFields(sd->at(1), 63);
	sd->release();

	WTSTransSlice* st = _reader.readTransSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(st != NULL);
	ASSERT_EQ(st->size(), 2u);
	expectTransFields(st->at(0), 62);
	expectTransFields(st->at(1), 63);
	st->release();
	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	expectQueFields(sq->at(0), 62);
	expectQueFields(sq->at(1), 63);
	sq->release();

	// 独立 reader 对同一文件执行反向首次访问，证明先有队列缓存不会屏蔽后续类型。
	WtDataReader queueFirstReader;
	WTSVariant* cfg = _cfg();
	ASSERT_TRUE(cfg != NULL);
	queueFirstReader.init(cfg, &_sink, NULL);
	cfg->release();

	WTSOrdQueSlice* qFirst = queueFirstReader.readOrdQueSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(qFirst != NULL);
	ASSERT_EQ(qFirst->size(), 2u);
	expectQueFields(qFirst->at(0), 62);
	expectQueFields(qFirst->at(1), 63);
	qFirst->release();
	WTSOrdDtlSlice* dAfterQueue = queueFirstReader.readOrdDtlSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(dAfterQueue != NULL);
	ASSERT_EQ(dAfterQueue->size(), 2u);
	expectDtlFields(dAfterQueue->at(0), 62);
	expectDtlFields(dAfterQueue->at(1), 63);
	dAfterQueue->release();
	WTSTransSlice* tAfterQueue = queueFirstReader.readTransSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(tAfterQueue != NULL);
	ASSERT_EQ(tAfterQueue->size(), 2u);
	expectTransFields(tAfterQueue->at(0), 62);
	expectTransFields(tAfterQueue->at(1), 63);
	tAfterQueue->release();

	// 重复读取改按 trans/detail/queue 顺序，核对三个缓存仍各自返回原始记录。
	WTSTransSlice* tAgain = queueFirstReader.readTransSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(tAgain != NULL);
	ASSERT_EQ(tAgain->size(), 2u);
	expectTransFields(tAgain->at(0), 62);
	expectTransFields(tAgain->at(1), 63);
	tAgain->release();
	WTSOrdDtlSlice* dAgain = queueFirstReader.readOrdDtlSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(dAgain != NULL);
	ASSERT_EQ(dAgain->size(), 2u);
	expectDtlFields(dAgain->at(0), 62);
	expectDtlFields(dAgain->at(1), 63);
	dAgain->release();
	WTSOrdQueSlice* qAgain = queueFirstReader.readOrdQueSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(qAgain != NULL);
	ASSERT_EQ(qAgain->size(), 2u);
	expectQueFields(qAgain->at(0), 62);
	expectQueFields(qAgain->at(1), 63);
	qAgain->release();
}

// WtDataReader.cpp: 空块/空数据守卫 L585-586、L591-592、L749、L905
TEST_F(R132ReaderHisTest, R132ReaderHisEmptyBlockAndEmptyRecordSet)
{
	// A) 过短文件：首次 size 校验失败并在 map 里留下空 block；第二次直接命中空 block 守卫
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600010.dsb", string(8, '\0')));
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600010", 2, 20240109150000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600010", 2, 20240109150000000ull) == NULL);

	// B) 合法 CMP_V2 但载荷为空：tcnt==0
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240108/600011.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, string())));
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600011", 2, 20240108150000000ull) == NULL);

	ASSERT_TRUE(write_bytes("his/orders/SSE/20240107/600012.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, string())));
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.600012", 2, 20240107150000000ull) == NULL);

	ASSERT_TRUE(write_bytes("his/trans/SSE/20240106/600013.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, string())));
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.600013", 2, 20240106150000000ull) == NULL);
}

TEST_F(R132ReaderHisTest, R132ReaderHisMalformedCompressedRecordsAreRejected)
{
	const auto truncate_frame = [](string block) {
		if (block.size() > sizeof(BlockHeaderV2) + 1)
		{
			block.pop_back();
			BlockHeaderV2* header = (BlockHeaderV2*)block.data();
			header->_size = block.size() - sizeof(BlockHeaderV2);
		}
		return block;
	};

	vector<WTSOrdQueStruct> queues(1, make_ordque(20240109, 20240109, 93100000, 61));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600014.dsb",
		truncate_frame(make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(queues)))));
	WTSOrdQueSlice* queueSlice = NULL;
	EXPECT_NO_THROW(queueSlice = _reader.readOrdQueSlice("SSE.STK.600014", 2, 20240109150000000ull));
	EXPECT_EQ(queueSlice, nullptr);
	if (queueSlice != NULL)
		queueSlice->release();

	vector<WTSOrdDtlStruct> details(1, make_orddtl(20240109, 20240109, 93200000, 62));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600015.dsb",
		truncate_frame(make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(details)))));
	WTSOrdDtlSlice* detailSlice = NULL;
	EXPECT_NO_THROW(detailSlice = _reader.readOrdDtlSlice("SSE.STK.600015", 2, 20240109150000000ull));
	EXPECT_EQ(detailSlice, nullptr);
	if (detailSlice != NULL)
		detailSlice->release();

	vector<WTSTransStruct> trans(1, make_trans(20240109, 20240109, 93300000, 63));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600016.dsb",
		truncate_frame(make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans)))));
	WTSTransSlice* transSlice = NULL;
	EXPECT_NO_THROW(transSlice = _reader.readTransSlice("SSE.STK.600016", 2, 20240109150000000ull));
	EXPECT_EQ(transSlice, nullptr);
	if (transSlice != NULL)
		transSlice->release();
}

// WtDataReader.cpp: cacheIntegratedBars L1098-1108/L1114-1122/L1163/L1262-1265
TEST_F(R132ReaderHisTest, R132ReaderIntegratedSectionsOverlapHotTail)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2402");
	_hot.add_section("rb2401", 20240102, 20240103, 1.0);
	_hot.add_section("rb2402", 20240104, 20240105, 1.0);

	// 日线热合约：0104..0108，最后一条 0108；最新切段 0104-0105 的右界早于热尾部
	vector<WTSBarStruct> hotDay;
	for (uint32_t d = 20240104; d <= 20240108; d++)
		hotDay.push_back(make_bar(d, d, d - 20240104));
	ASSERT_TRUE(write_bytes("his/day/SHFE.rb_HOT.dsb",
		make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, pack_bars(hotDay))));

	// 分钟热合约：最后一条 0105 09:30；左界 0104 09:00 早于热尾部、右界 0105 15:00 晚于热尾部
	vector<WTSBarStruct> hotMin;
	hotMin.push_back(make_bar(20240104, min_time(20240104, 930), 1));
	hotMin.push_back(make_bar(20240105, min_time(20240105, 930), 2));
	ASSERT_TRUE(write_bytes("his/min1/SHFE.rb_HOT.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(hotMin))));

	WTSKlineSlice* day = _reader.readKlineSlice("SHFE.rb.HOT", KP_DAY, 10, 202401101500ull);
	ASSERT_TRUE(day != NULL);
	const vector<WTSBarStruct> dgot = r52::flatten_bars(day);
	ASSERT_EQ(dgot.size(), hotDay.size());
	EXPECT_EQ(dgot.back().date, 20240108u);
	day->release();

	WTSKlineSlice* m1 = _reader.readKlineSlice("SHFE.rb.HOT", KP_Minute1, 10, 202401101500ull);
	ASSERT_TRUE(m1 != NULL);
	const vector<WTSBarStruct> mgot = r52::flatten_bars(m1);
	ASSERT_EQ(mgot.size(), hotMin.size());
	EXPECT_EQ(mgot.back().time, hotMin.back().time);
	m1->release();
}

} // namespace r132
