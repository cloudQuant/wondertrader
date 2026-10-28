// r70 / WtRdmDtReader：R52 之后仍缺的当日逐笔区间、他日文件校验、复权分钟线与主力 tick 尾窗
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange  :611-784   isToday 的 RT 分支（beginTDate==endTDate 左右裁剪 /
//                                      跨交易日取全部）+ getRTOrdQueBlock :2082-2120
//  - readOrdDtlSliceByRange  :786-958   isToday 的 RT 分支 + getRTOrdDtlBlock :2042-2080
//  - readTransSliceByRange   :960-1132  isToday 的 RT 分支 + getRTTransBlock :2122-2160
//  - 三条 his 分支的 CMP_V2 建块、`>=` 右边界回退、长度/尺寸校验失败、header-only 空块
//  - cacheHisBarsFromFile 的股票复权分钟分支 :1364-1567（lastQTime、minBarToDate、原始文件续接、
//    原始文件缺失 continue、原始文件过短 return false）
//  - readTickSliceByCount / readTickSliceByRange 的 HOT 代码探测、RT 尾窗与跨日回退 :536-608 / :2395-2604
//
// 手法沿用 q03/R52：init(NULL, sink)（不起 5 秒轮询线程）+ Sandbox 切 CWD；无访问权限宏。
#include "r52_storage_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r70 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

string his_hft_file(const char* kind, uint32_t date, const char* code = "600000")
{
	return fmtutil::format("his/{}/SSE/{}/{}.dsb", kind, date, code);
}

string rt_hft_file(const char* kind, const char* code = "600000")
{
	return fmtutil::format("rt/{}/SSE/{}.dmb", kind, code);
}

string his_tick_file(uint32_t date, const char* code = "rb_HOT")
{
	return fmtutil::format("his/ticks/SHFE/{}/{}.dsb", date, code);
}

// HOT 用例走 SHFE.rb.HOT：reader 按标准码的交易所（SHFE）拼路径，
// 不能沿用股票用例的 SSE 目录。
string rt_tick_file(const char* code = "rb2401")
{
	return fmtutil::format("rt/ticks/SHFE/{}.dmb", code);
}

vector<WTSOrdQueStruct> make_queues(uint32_t tdate, uint32_t base_seq)
{
	vector<WTSOrdQueStruct> ay;
	for (uint32_t i = 0; i < 4; i++)
		ay.push_back(make_ordque(tdate, tdate, 93000000u + i * 100000u, base_seq + i));
	return ay;
}

vector<WTSOrdDtlStruct> make_orddtls(uint32_t tdate, uint32_t base_seq)
{
	vector<WTSOrdDtlStruct> ay;
	for (uint32_t i = 0; i < 4; i++)
		ay.push_back(make_orddtl(tdate, tdate, 93000000u + i * 100000u, base_seq + i));
	return ay;
}

vector<WTSTransStruct> make_transes(uint32_t tdate, uint32_t base_seq)
{
	vector<WTSTransStruct> ay;
	for (uint32_t i = 0; i < 4; i++)
		ay.push_back(make_trans(tdate, tdate, 93000000u + i * 100000u, base_seq + i));
	return ay;
}

// WtRdmDtReader 的 CMP_V2 his 分支在 `_buffer.resize(sizeof(BlockHeader)); append(payload)`
// 之后仍通过解压前取得的 tBlockV2 指针写 `_version`；一旦 append 触发重分配，这次写入
// 就落在已释放内存上（bugs/R77.md R77-01）。zstd 对全零结构压缩率极高，普通 fixture
// 必然触发；这里把 reader 不读取的字节填成伪随机值，使压缩后尺寸不小于原始 payload，
// 保留完整 CMP_V2 解压/建块路径，同时避免触发该产品缺陷。
inline uint32_t next_noise(uint32_t& s)
{
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

template <typename T>
void scramble(T& item, uint32_t seed)
{
	unsigned char* p = (unsigned char*)&item;
	uint32_t s = seed * 2654435761u + 0x9e3779b9u;
	for (std::size_t i = 0; i < sizeof(T); i++)
		p[i] = (unsigned char)(next_noise(s) >> 24);
}

WTSOrdQueStruct noisy_ordque(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSOrdQueStruct item;
	scramble(item, seq);
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.price = 60.0 + seq;
	item.side = seq % 2 == 0 ? BDT_Buy : BDT_Sell;
	return item;
}

WTSOrdDtlStruct noisy_orddtl(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSOrdDtlStruct item;
	scramble(item, seq);
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.index = 1000 + seq;
	item.price = 61.0 + seq;
	item.volume = 100 + seq;
	return item;
}

WTSTransStruct noisy_trans(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSTransStruct item;
	scramble(item, seq);
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.index = 2000 + seq;
	item.price = 62.0 + seq;
	item.volume = 200 + seq;
	item.askorder = 300000 + seq;
	item.bidorder = 400000 + seq;
	return item;
}

} // namespace

class R70RdmResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r70rdm"));
		_bd.set_cur_tdate(kCurTDate);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	r52::HotMgr			_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// ------------------------------------------------------------------ 当日 RT 逐笔三条链
// WtRdmDtReader.cpp: readOrdQueSliceByRange L652-696 + getRTOrdQueBlock L2082-2120
TEST_F(R70RdmResidualTest, R70RdmTodayOrderQueueRangeClips)
{
	const vector<WTSOrdQueStruct> items = make_queues(kCurTDate, 1);
	ASSERT_TRUE(write_bytes(rt_hft_file("queue"),
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kCurTDate, 4, 8, items)));

	// beginTDate == endTDate：右边界 09:32、左边界 09:31 → 取 [1,2] 两条
	WTSOrdQueSlice* s = _reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240110093100000ull, 20240110093200000ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	ASSERT_TRUE(s->at(0) != NULL);
	ASSERT_TRUE(s->at(1) != NULL);
	EXPECT_EQ(s->at(0)->action_time, 93100000u);
	EXPECT_EQ(s->at(1)->action_time, 93200000u);
	EXPECT_DOUBLE_EQ(s->at(0)->price, items[1].price);
	s->release();

	// beginTDate != endTDate：从另一交易日起，返回 RT 块 0..eIdx（3 条）
	WTSOrdQueSlice* all = _reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240109090000000ull, 20240110093200000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(0)->action_time, 93000000u);
	EXPECT_EQ(all->at(2)->action_time, 93200000u);
	all->release();
}

// WtRdmDtReader.cpp: readOrdDtlSliceByRange L827-871 + getRTOrdDtlBlock L2042-2080
TEST_F(R70RdmResidualTest, R70RdmTodayOrderDetailRangeClips)
{
	const vector<WTSOrdDtlStruct> items = make_orddtls(kCurTDate, 11);
	ASSERT_TRUE(write_bytes(rt_hft_file("orders"),
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kCurTDate, 4, 8, items)));

	WTSOrdDtlSlice* s = _reader.readOrdDtlSliceByRange("SSE.STK.600000",
		20240110093100000ull, 20240110093200000ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	ASSERT_TRUE(s->at(0) != NULL);
	EXPECT_EQ(s->at(0)->action_time, 93100000u);
	EXPECT_EQ(s->at(1)->index, items[2].index);
	s->release();

	WTSOrdDtlSlice* all = _reader.readOrdDtlSliceByRange("SSE.STK.600000",
		20240109090000000ull, 20240110093200000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(0)->action_time, 93000000u);
	all->release();
}

// WtRdmDtReader.cpp: readTransSliceByRange L1001-1045 + getRTTransBlock L2122-2160
TEST_F(R70RdmResidualTest, R70RdmTodayTransRangeClips)
{
	const vector<WTSTransStruct> items = make_transes(kCurTDate, 21);
	ASSERT_TRUE(write_bytes(rt_hft_file("trans"),
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kCurTDate, 4, 8, items)));

	WTSTransSlice* s = _reader.readTransSliceByRange("SSE.STK.600000",
		20240110093100000ull, 20240110093200000ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	ASSERT_TRUE(s->at(0) != NULL);
	EXPECT_EQ(s->at(0)->action_time, 93100000u);
	EXPECT_EQ(s->at(1)->askorder, items[2].askorder);
	s->release();

	WTSTransSlice* all = _reader.readTransSliceByRange("SSE.STK.600000",
		20240109090000000ull, 20240110093200000ull);
	ASSERT_TRUE(all != NULL);
	ASSERT_EQ(all->size(), 3u);
	EXPECT_EQ(all->at(2)->action_time, 93200000u);
	all->release();
}

// ------------------------------------------------------------------ 他日逐笔文件校验
// WtRdmDtReader.cpp: readOrdQueSliceByRange L699-783 / readOrdDtlSliceByRange L874-957
//                   / readTransSliceByRange L1048-1131（CMP_V2 建块、>= 回退、短文件、尺寸不符）
TEST_F(R70RdmResidualTest, R70RdmHisHftFilesBuildAndReject)
{
	const vector<WTSOrdQueStruct> que = { noisy_ordque(20240108, 20240108, 93000000, 1),
		noisy_ordque(20240108, 20240108, 93100000, 2),
		noisy_ordque(20240108, 20240108, 93200000, 3) };
	ASSERT_TRUE(write_bytes(his_hft_file("queue", 20240108),
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(que))));
	ASSERT_TRUE(write_bytes(his_hft_file("queue", 20240109), string(8, 'X')));

	const vector<WTSOrdDtlStruct> dtl = { noisy_orddtl(20240107, 20240107, 93000000, 11),
		noisy_orddtl(20240107, 20240107, 93100000, 12),
		noisy_orddtl(20240107, 20240107, 93200000, 13) };
	ASSERT_TRUE(write_bytes(his_hft_file("orders", 20240107),
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtl))));

	const vector<WTSTransStruct> trs = { noisy_trans(20240106, 20240106, 93000000, 31),
		noisy_trans(20240106, 20240106, 93100000, 32) };
	ASSERT_TRUE(write_bytes(his_hft_file("trans", 20240106),
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trs)) + "X"));

	// 委托队列：15:00 右边界取全部 3 条；09:31 命中 `>=` 回退只剩 09:30
	WTSOrdQueSlice* sq = _reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240108150000000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 3u);
	EXPECT_EQ(sq->at(2)->action_time, 93200000u);
	sq->release();

	WTSOrdQueSlice* sq2 = _reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240108090000000ull, 20240108093100000ull);
	ASSERT_TRUE(sq2 != NULL);
	ASSERT_EQ(sq2->size(), 1u);
	EXPECT_EQ(sq2->at(0)->action_time, 93000000u);
	sq2->release();

	const uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240109090000000ull, 20240109150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 委托明细：15:00 三条、09:31 一条（右边界回退）
	WTSOrdDtlSlice* sd = _reader.readOrdDtlSliceByRange("SSE.STK.600000",
		20240107090000000ull, 20240107150000000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 3u);
	EXPECT_EQ(sd->at(1)->index, dtl[1].index);
	sd->release();

	WTSOrdDtlSlice* sd2 = _reader.readOrdDtlSliceByRange("SSE.STK.600000",
		20240107090000000ull, 20240107093100000ull);
	ASSERT_TRUE(sd2 != NULL);
	ASSERT_EQ(sd2->size(), 1u);
	EXPECT_EQ(sd2->at(0)->index, dtl[0].index);
	sd2->release();

	// 逐笔成交：文件尾部多 1 字节 → `_size` 与文件长度不符
	const uint32_t err1 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.600000",
		20240106090000000ull, 20240106150000000ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err1);
	EXPECT_NE(_sink.last_log().find("transaction"), string::npos);
}

// ------------------------------------------------------------------ 股票复权分钟线
// WtRdmDtReader.cpp: cacheHisBarsFromFile 的复权分支 L1364-1567
//   复权后文件 lastQTime（:1435）、minBarToDate（:1454-1458）、原始文件 lower_bound 续接、
//   原始文件缺失 continue（:1468）、原始文件过短 return false（:1474-1477）
//   + getAdjFactorByDate L2607-2641
TEST_F(R70RdmResidualTest, R70RdmAdjustedStockMin1Continuation)
{
	// 前复权文件（已复权）两根：0103 14:58 / 0104 14:58
	vector<WTSBarStruct> adj;
	adj.push_back(make_bar(20240103, TimeUtils::timeToMinBar(20240103, 1458), 1));
	adj.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 1458), 2));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(adj))));

	// 原始文件：0104 14:58 与最后一条复权 bar 时间相同（sBar.time = lastQTime+1 会跳过它）
	vector<WTSBarStruct> raw;
	raw.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 1458), 11));
	raw.push_back(make_bar(20240105, TimeUtils::timeToMinBar(20240105, 930), 12));
	raw.push_back(make_bar(20240105, TimeUtils::timeToMinBar(20240105, 931), 13));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600000.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(raw))));

	WTSKlineSlice* s = _reader.readKlineSliceByRange("SSE.STK.600000-", KP_Minute1,
		202401010000ull, 202401081500ull);
	ASSERT_TRUE(s != NULL);
	vector<WTSBarStruct> got = r52::flatten_bars(s);
	ASSERT_EQ(got.size(), (std::size_t)4);
	EXPECT_EQ(got[0].date, 20240103u);
	EXPECT_EQ(got[1].date, 20240104u);
	EXPECT_EQ(got[2].date, 20240105u);
	EXPECT_EQ(got[3].date, 20240105u);
	EXPECT_DOUBLE_EQ(got[0].open, 11.0);
	EXPECT_DOUBLE_EQ(got[1].open, 12.0);
	EXPECT_DOUBLE_EQ(got[2].open, 22.0);
	EXPECT_DOUBLE_EQ(got[3].open, 23.0);
	s->release();

	// 只有复权文件、没有原始文件 → :1468 continue，仍返回复权文件里的 2 根
	vector<WTSBarStruct> adj2;
	adj2.push_back(make_bar(20240104, TimeUtils::timeToMinBar(20240104, 1459), 41));
	adj2.push_back(make_bar(20240105, TimeUtils::timeToMinBar(20240105, 1459), 42));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600001-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(adj2))));

	WTSKlineSlice* s2 = _reader.readKlineSliceByRange("SSE.STK.600001-", KP_Minute1,
		202401010000ull, 202401081500ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 2);
	s2->release();

	// 原始文件存在但小于 HisKlineBlock(12) → :1474-1477 返回 false
	ASSERT_TRUE(write_bytes("his/min1/SSE/600002-.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(adj2))));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600002.dsb", string(8, 'X')));
	const uint32_t err0 = _sink.count_of(LL_ERROR);
	EXPECT_TRUE(_reader.readKlineSliceByRange("SSE.STK.600002-", KP_Minute1,
		202401010000ull, 202401081500ull) == NULL);
	EXPECT_GT(_sink.count_of(LL_ERROR), err0);

	// 未配置因子文件（init(NULL)）：股票走空表返回 1.0；带后缀先截断；非股票直接 1.0
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000-", 20240105), 1.0);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SSE.STK.600000", 20240105), 1.0);
	EXPECT_DOUBLE_EQ(_reader.getAdjFactorByDate("SHFE.rb.2401", 20240105), 1.0);
}

// ------------------------------------------------------------------ HOT tick 尾窗
// WtRdmDtReader.cpp: readTickSliceByCount L2417-2604（HOT 代码、RT 尾窗、跨日回退、交易日不同时
//   用收盘时间作右边界）+ readTickSliceByRange L536-608（HOT 代码、同日裁剪/跨日全量）
TEST_F(R70RdmResidualTest, R70RdmHotTickCountAndRangeTail)
{
	_hot.set_tag("HOT");
	_hot.set_raw_code("rb2401");

	vector<WTSTickStruct> rt = { make_tick(kCurTDate, kCurTDate, 143000000, 500),
		make_tick(kCurTDate, kCurTDate, 143100000, 501),
		make_tick(kCurTDate, kCurTDate, 143200000, 502) };
	ASSERT_TRUE(write_bytes(rt_tick_file("rb2401"),
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, kCurTDate, 3, 8, rt)));

	vector<WTSTickStruct> his = { make_tick(20240109, 20240109, 143000000, 100),
		make_tick(20240109, 20240109, 143100000, 101) };
	ASSERT_TRUE(write_bytes(his_tick_file(20240109, "rb_HOT"),
		make_block(BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, pack_ticks(his))));

	// etime 落在未来交易日（endTDate > curTDate）：RT 右边界用收盘时间，历史段同样用收盘时间
	WTSTickSlice* s = _reader.readTickSliceByCount("SHFE.rb.HOT", 4, 20240111150000000ull);
	ASSERT_TRUE(s != NULL);
	vector<WTSTickStruct> got = r52::flatten_ticks(s);
	ASSERT_EQ(got.size(), (std::size_t)4);
	EXPECT_EQ(got[0].action_date, 20240109u);
	EXPECT_EQ(got[0].action_time, 143100000u);
	EXPECT_EQ(got[1].action_date, kCurTDate);
	EXPECT_EQ(got[1].action_time, 143000000u);
	s->release();

	// etime 夹在 14:31 与 14:32 之间：RT 分支 `>` 回退一条，取 14:30/14:31
	WTSTickSlice* s2 = _reader.readTickSliceByCount("SHFE.rb.HOT", 2, 20240110143150000ull);
	ASSERT_TRUE(s2 != NULL);
	vector<WTSTickStruct> got2 = r52::flatten_ticks(s2);
	ASSERT_EQ(got2.size(), (std::size_t)2);
	EXPECT_EQ(got2[0].action_time, 143000000u);
	EXPECT_EQ(got2[1].action_time, 143100000u);
	s2->release();

	// ByRange 同日：左边界 14:31 → 取 14:31/14:32
	WTSTickSlice* r1 = _reader.readTickSliceByRange("SHFE.rb.HOT",
		20240110143100000ull, 20240110150000000ull);
	ASSERT_TRUE(r1 != NULL);
	vector<WTSTickStruct> g1 = r52::flatten_ticks(r1);
	ASSERT_EQ(g1.size(), (std::size_t)2);
	EXPECT_EQ(g1[0].action_time, 143100000u);
	EXPECT_EQ(g1[1].action_time, 143200000u);
	r1->release();

	// ByRange 跨交易日：历史段 0109 两条全取，RT 段返回 0..eIdx 两条
	WTSTickSlice* r2 = _reader.readTickSliceByRange("SHFE.rb.HOT",
		20240109090000000ull, 20240110143100000ull);
	ASSERT_TRUE(r2 != NULL);
	vector<WTSTickStruct> g2 = r52::flatten_ticks(r2);
	ASSERT_EQ(g2.size(), (std::size_t)4);
	EXPECT_EQ(g2[0].action_date, 20240109u);
	EXPECT_EQ(g2[0].action_time, 143000000u);
	EXPECT_EQ(g2[1].action_time, 143100000u);
	EXPECT_EQ(g2[2].action_date, kCurTDate);
	EXPECT_EQ(g2[2].action_time, 143000000u);
	EXPECT_EQ(g2[3].action_time, 143100000u);
	r2->release();
}

// ------------------------------------------------------------------ 缺陷复现（不进启用集）
// BUG-R70-001：readOrdQueSliceByRange / readOrdDtlSliceByRange / readTransSliceByRange 的
//   isToday 分支（:670-674 / :845-849 / :1019-1023）在 etime 早于 RT 块首条时，
//   lower_bound 返回 begin，随后无 eIdx==0 守卫直接 `pItem--; eIdx--;`（size_t 回绕）；
//   同交易日分支再用回绕后的 eIdx 调 lower_bound，区间 first > last → 未定义行为/崩溃。
TEST_F(R70RdmResidualTest, DISABLED_R70RdmTodayHftRangeBeforeFirstItemUnderflows)
{
	const vector<WTSOrdQueStruct> items = make_queues(kCurTDate, 1);
	ASSERT_TRUE(write_bytes(rt_hft_file("queue"),
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kCurTDate, 4, 8, items)));

	// 块内首条 09:30，etime 09:00 → pItem==begin，`pItem-- / eIdx--` 回绕
	WTSOrdQueSlice* s = _reader.readOrdQueSliceByRange("SSE.STK.600000",
		20240110090000000ull, 20240110090000000ull);
	if (s != NULL)
		s->release();
}

} // namespace r70
