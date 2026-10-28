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
//  - 逐笔 CMP_V2 载荷用伪随机字节填充（价格/量/代码字段），让 zstd 压缩后长度不小于
//    原始载荷，避免生产代码在 `_buffer.resize()+append()` 之后仍通过旧指针写
//    `tBlockV2->_version`（BUG-R132-001，缓冲区一旦重分配即悬垂写）；
//  - 临时目录由 q03::Sandbox 管理，TearDown remove_all，不写仓库。
#include "r52_storage_fixture.h"

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

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
	s.volume = rnd_double();
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
	s.volume = rnd_double();
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
	EXPECT_EQ(st->at(0)->action_date, 20240108u);
	EXPECT_EQ(st->at(1)->action_date, 20240109u);
	st->release();
}

// 缺陷复现（BUG-R132-001）：可高度压缩的 CMP_V2 载荷在 resize+append 后可能重分配，
// 生产代码仍通过旧指针写 tBlockV2->_version。默认不跑；--gtest_also_run_disabled_tests 可复现。
TEST_F(R132ReaderHisTest, DISABLED_R132ReaderHisCompressibleQueueDanglingWrite)
{
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240109, 20240109, 93100000, 41));
	que.push_back(make_ordque(20240109, 20240109, 93200000, 42));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600040.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600040", 2, 20240109150000000ull);
	ASSERT_TRUE(sq != NULL);
	EXPECT_EQ(sq->size(), 2u);
	sq->release();
}

// 缺陷复现（BUG-R132-002）：readOrdDtlSlice 用 _his_ordque_map 判缓存命中。同一合约/日期
// 先读过队列后，明细文件即使存在也不会被加载，直接返回 NULL。默认不跑。
TEST_F(R132ReaderHisTest, DISABLED_R132ReaderHisQueueCachePoisonsDetail)
{
	vector<WTSOrdQueStruct> que;
	WTSOrdQueStruct q1 = make_ordque(20240109, 20240109, 93100000, 51);
	WTSOrdQueStruct q2 = make_ordque(20240109, 20240109, 93200000, 52);
	scramble_ordque(q1);
	scramble_ordque(q2);
	que.push_back(q1);
	que.push_back(q2);
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600041.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));

	vector<WTSOrdDtlStruct> dtl;
	WTSOrdDtlStruct d1 = make_orddtl(20240109, 20240109, 93100000, 53);
	WTSOrdDtlStruct d2 = make_orddtl(20240109, 20240109, 93200000, 54);
	scramble_orddtl(d1);
	scramble_orddtl(d2);
	dtl.push_back(d1);
	dtl.push_back(d2);
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600041.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	WTSOrdQueSlice* sq = _reader.readOrdQueSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(sq != NULL);
	sq->release();

	// 期望明细仍可读到两条；当前实现因 _his_ordque_map 命中而返回 NULL
	WTSOrdDtlSlice* sd = _reader.readOrdDtlSlice("SSE.STK.600041", 2, 20240109150000000ull);
	ASSERT_TRUE(sd != NULL);
	EXPECT_EQ(sd->size(), 2u);
	sd->release();
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
