// r52 / WtDataWriter：RT 块跨日/容量修复、K 线块扩容、日线与分钟线历史落盘的冷门分支
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - getOrdQueBlock/getOrdDtlBlock/getTransBlock/getTickBlock 的「文件已存在」分支
//      跨交易日重置 L717-755 / L811-850 / L906-945 / L1009-1049
//      文件大小与头部 capacity 不符时的容量修复（同上四段，tick 在 L1029-1047）
//  - pipeToKlines 的 min1/min5 块扩容 L1093-1098 / L1200-1205（容量按 getTradingMins 预分配）
//  - dump_day_data 的 altcode 改名 L1922-1927、旧文件续写与 >100 条压缩 L1950-2006
//  - dump_bars_to_file 的 min1/min5 altcode 改名 L2079-2084 / L2158-2163 与旧文件追加 L2097-2106
//
// 线程纪律沿用 r23：调过 init 的 writer 只 release 不 delete；走 transHisData 的
// 只等 marker.ini 并 abandon（reason 见 r23_writer_fixture.h 头注释）。
#include "r23_writer_fixture.h"
#include "../q03_storage3/r52_storage_fixture.h"

#include "WTSUtils/WTSCmpHelper.hpp"

namespace r52 {

using namespace r23;

namespace {

WTSTickStruct tick_struct(const TickSpec& spec)
{
	WTSTickStruct ts;
	std::memset(&ts, 0, sizeof(ts));
	snprintf(ts.exchg, sizeof(ts.exchg), "%s", spec.exchg.c_str());
	snprintf(ts.code, sizeof(ts.code), "%s", spec.code.c_str());
	ts.price = spec.price;
	ts.open = spec.open;
	ts.high = spec.high;
	ts.low = spec.low;
	ts.settle_price = spec.settle;
	ts.upper_limit = spec.upper_limit;
	ts.lower_limit = spec.lower_limit;
	ts.total_volume = spec.total_volume;
	ts.volume = spec.volume;
	ts.total_turnover = spec.total_turnover;
	ts.turn_over = spec.turn_over;
	ts.open_interest = spec.open_interest;
	ts.diff_interest = spec.diff_interest;
	ts.trading_date = spec.trading_date;
	ts.action_date = spec.action_date;
	ts.action_time = spec.action_time;
	ts.pre_close = spec.pre_close;
	ts.pre_settle = spec.pre_settle;
	ts.pre_interest = spec.pre_interest;
	for (uint32_t i = 0; i < 10; i++)
	{
		ts.bid_prices[i] = spec.price - 0.5 - i * 0.1;
		ts.ask_prices[i] = spec.price + 0.5 + i * 0.1;
		ts.bid_qty[i] = 100.0 + i;
		ts.ask_qty[i] = 200.0 + i;
	}
	return ts;
}

WTSOrdQueStruct que_struct(uint32_t tdate, uint32_t atime, uint32_t seq)
{
	WTSOrdQueStruct oq;
	std::memset(&oq, 0, sizeof(oq));
	snprintf(oq.exchg, sizeof(oq.exchg), "%s", "SHFE");
	snprintf(oq.code, sizeof(oq.code), "%s", "rb2210");
	oq.trading_date = tdate;
	oq.action_date = tdate;
	oq.action_time = atime;
	oq.price = 3600.0 + seq;
	oq.order_items = 3;
	oq.qsize = 5;
	for (uint32_t i = 0; i < 50; i++)
		oq.volumes[i] = 10 + i + seq;
	oq.side = BDT_Buy;
	return oq;
}

WTSOrdDtlStruct dtl_struct(uint32_t tdate, uint32_t atime, uint32_t seq)
{
	WTSOrdDtlStruct od;
	std::memset(&od, 0, sizeof(od));
	snprintf(od.exchg, sizeof(od.exchg), "%s", "SHFE");
	snprintf(od.code, sizeof(od.code), "%s", "rb2210");
	od.trading_date = tdate;
	od.action_date = tdate;
	od.action_time = atime;
	od.price = 3600.0 + seq;
	od.volume = 100.0;
	od.index = seq;
	od.side = BDT_Buy;
	od.otype = ODT_LimitPrice;
	od.orderid = 700000 + seq;
	return od;
}

WTSTransStruct trans_struct(uint32_t tdate, uint32_t atime, uint32_t seq)
{
	WTSTransStruct tr;
	std::memset(&tr, 0, sizeof(tr));
	snprintf(tr.exchg, sizeof(tr.exchg), "%s", "SHFE");
	snprintf(tr.code, sizeof(tr.code), "%s", "rb2210");
	tr.trading_date = tdate;
	tr.action_date = tdate;
	tr.action_time = atime;
	tr.price = 3600.0 + seq;
	tr.volume = 200.0;
	tr.index = seq;
	tr.ttype = TT_Match;
	tr.side = BDT_Sell;
	tr.askorder = 300000 + seq;
	tr.bidorder = 400000 + seq;
	return tr;
}

// 读取 CMP_V2 的历史 K 线文件（writer 侧压缩形态）
vector<WTSBarStruct> load_cmp_bars(const string& file, bool& ok)
{
	ok = false;
	const string raw = read_file_bytes(file);
	if (raw.size() < sizeof(BlockHeaderV2))
		return vector<WTSBarStruct>();

	BlockHeaderV2 head;
	std::memset(&head, 0, sizeof(head));
	std::memcpy(&head, raw.data(), sizeof(head));
	if (head._version != BLOCK_VERSION_CMP_V2 || raw.size() != sizeof(BlockHeaderV2) + head._size)
		return vector<WTSBarStruct>();

	const string payload = WTSCmpHelper::uncompress_data(raw.data() + sizeof(BlockHeaderV2),
		(std::size_t)head._size);
	if (payload.size() % sizeof(WTSBarStruct) != 0)
		return vector<WTSBarStruct>();

	vector<WTSBarStruct> bars(payload.size() / sizeof(WTSBarStruct));
	if (!bars.empty())
		std::memcpy(bars.data(), payload.data(), payload.size());
	ok = true;
	return bars;
}

} // namespace

class R52WriterTest : public R23WriterTest
{
};

// WtDataWriter.cpp: getTickBlock L1009-1015 / getOrdQueBlock L717-724 / getOrdDtlBlock L811-817
//                   / getTransBlock L906-912（跨交易日重置 size/date + memset）
TEST_F(R52WriterTest, R52RtBlockDateRolloverResetsAllFourBlocks)
{
	R23Writer* w1 = make_writer();
	ASSERT_TRUE(w1 != NULL);

	TickSpec t1;
	t1.trading_date = 20240108;
	t1.action_date = 20240108;
	write_tick(w1, t1, 0);
	OrdQueSpec q1;
	q1.trading_date = 20240108;
	q1.action_date = 20240108;
	ASSERT_TRUE(w1->writeOrderQueue(make_ordque_data(q1)));

	WTSContractInfo* ct = _bd->getContract("rb2210", "SHFE");
	ASSERT_TRUE(ct != NULL);
	WTSOrdDtlStruct od1 = dtl_struct(20240108, 93000000, 1);
	WTSOrdDtlData* d1 = WTSOrdDtlData::create(od1);
	d1->setContractInfo(ct);
	ASSERT_TRUE(w1->writeOrderDetail(d1));
	d1->release();
	WTSTransStruct tr1 = trans_struct(20240108, 93000000, 2);
	WTSTransData* r1 = WTSTransData::create(tr1);
	r1->setContractInfo(ct);
	ASSERT_TRUE(w1->writeTransaction(r1));
	r1->release();

	close_writer(w1);

	// 第二个 writer 在同一目录打开既有 RT 块；交易日 20240109 → 四条重置分支
	R23Writer* w2 = make_writer();
	ASSERT_TRUE(w2 != NULL);

	TickSpec t2;
	t2.trading_date = 20240109;
	t2.action_date = 20240109;
	t2.total_volume = 2000.0;
	write_tick(w2, t2, 0);

	OrdQueSpec q2;
	q2.trading_date = 20240109;
	q2.action_date = 20240109;
	ASSERT_TRUE(w2->writeOrderQueue(make_ordque_data(q2)));

	WTSOrdDtlStruct od2 = dtl_struct(20240109, 93000000, 3);
	WTSOrdDtlData* d2 = WTSOrdDtlData::create(od2);
	d2->setContractInfo(ct);
	ASSERT_TRUE(w2->writeOrderDetail(d2));
	d2->release();

	WTSTransStruct tr2 = trans_struct(20240109, 93000000, 4);
	WTSTransData* r2 = WTSTransData::create(tr2);
	r2->setContractInfo(ct);
	ASSERT_TRUE(w2->writeTransaction(r2));
	r2->release();

	const RTDayBlockHeader tick = read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"));
	EXPECT_EQ(tick._date, 20240109u);
	EXPECT_EQ(tick._size, 1u);
	const RTDayBlockHeader que = read_block_header<RTDayBlockHeader>(_base + "rt/queue/SHFE/rb2210.dmb");
	EXPECT_EQ(que._date, 20240109u);
	EXPECT_EQ(que._size, 1u);
	const RTDayBlockHeader dtl = read_block_header<RTDayBlockHeader>(_base + "rt/orders/SHFE/rb2210.dmb");
	EXPECT_EQ(dtl._date, 20240109u);
	EXPECT_EQ(dtl._size, 1u);
	const RTDayBlockHeader trns = read_block_header<RTDayBlockHeader>(_base + "rt/trans/SHFE/rb2210.dmb");
	EXPECT_EQ(trns._date, 20240109u);
	EXPECT_EQ(trns._size, 1u);

	EXPECT_TRUE(_sink->has_log("is different from current date", LL_INFO));
	EXPECT_TRUE(_sink->has_log("date[20240108] of tick cache block", LL_INFO));
	EXPECT_TRUE(_sink->has_log("date[20240108] of orderqueue cache block", LL_INFO));
	EXPECT_TRUE(_sink->has_log("date[20240108] of orderdetail cache block", LL_INFO));
	EXPECT_TRUE(_sink->has_log("date[20240108] of transaction cache block", LL_INFO));
}

// WtDataWriter.cpp: getTickBlock L1029-1047 / getOrdQueBlock L738-755 / getOrdDtlBlock L831-850
//                   / getTransBlock L926-945（头部 capacity 与实际文件大小不符 → 修复后首写再扩容）
TEST_F(R52WriterTest, R52RtBlockCapacityRepairFromFileSize)
{
	vector<WTSTickStruct> ticks;
	for (uint32_t i = 0; i < 3; i++)
	{
		TickSpec s;
		s.total_volume = 1000.0 + i;
		ticks.push_back(tick_struct(s));
	}
	ASSERT_TRUE(q03::write_bytes(rt_tick_file("rb2210"),
		make_short_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240108, 5, ticks)));

	vector<WTSOrdQueStruct> ques;
	for (uint32_t i = 0; i < 3; i++)
		ques.push_back(que_struct(20240108, 93000000 + i * 1000, i));
	ASSERT_TRUE(q03::write_bytes(_base + "rt/queue/SHFE/rb2210.dmb",
		make_short_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, 20240108, 5, ques)));

	vector<WTSOrdDtlStruct> dtls;
	for (uint32_t i = 0; i < 3; i++)
		dtls.push_back(dtl_struct(20240108, 93000000 + i * 1000, 10 + i));
	ASSERT_TRUE(q03::write_bytes(_base + "rt/orders/SHFE/rb2210.dmb",
		make_short_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, 20240108, 5, dtls)));

	vector<WTSTransStruct> trs;
	for (uint32_t i = 0; i < 3; i++)
		trs.push_back(trans_struct(20240108, 93000000 + i * 1000, 20 + i));
	ASSERT_TRUE(q03::write_bytes(_base + "rt/trans/SHFE/rb2210.dmb",
		make_short_rt_block<WTSTransStruct>(BT_RT_Trnsctn, 20240108, 5, trs)));

	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	WTSContractInfo* ct = _bd->getContract("rb2210", "SHFE");
	ASSERT_TRUE(ct != NULL);

	// 修复：capacity=实际条数 3 → 首写时 size==capacity → 扩容到 6，随后写入第 4 条
	TickSpec t;
	t.trading_date = 20240108;
	t.action_date = 20240108;
	t.total_volume = 5000.0;
	write_tick(w, t, 0);
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(OrdQueSpec())));
	WTSOrdDtlStruct od = dtl_struct(20240108, 94000000, 31);
	WTSOrdDtlData* d = WTSOrdDtlData::create(od);
	d->setContractInfo(ct);
	ASSERT_TRUE(w->writeOrderDetail(d));
	d->release();
	WTSTransStruct tr = trans_struct(20240108, 94000000, 32);
	WTSTransData* r = WTSTransData::create(tr);
	r->setContractInfo(ct);
	ASSERT_TRUE(w->writeTransaction(r));
	r->release();

	const RTDayBlockHeader tick = read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"));
	EXPECT_EQ(tick._capacity, 6u);
	EXPECT_EQ(tick._size, 4u);
	const RTDayBlockHeader que = read_block_header<RTDayBlockHeader>(_base + "rt/queue/SHFE/rb2210.dmb");
	EXPECT_EQ(que._capacity, 6u);
	EXPECT_EQ(que._size, 4u);
	const RTDayBlockHeader dtl = read_block_header<RTDayBlockHeader>(_base + "rt/orders/SHFE/rb2210.dmb");
	EXPECT_EQ(dtl._capacity, 6u);
	EXPECT_EQ(dtl._size, 4u);
	const RTDayBlockHeader trns = read_block_header<RTDayBlockHeader>(_base + "rt/trans/SHFE/rb2210.dmb");
	EXPECT_EQ(trns._capacity, 6u);
	EXPECT_EQ(trns._size, 4u);

	EXPECT_TRUE(_sink->has_log("Tick cache file of rb2210 on 20240108 repaired", LL_WARN));
	EXPECT_TRUE(_sink->has_log("Oderqueue cache file of rb2210 on date 20240108 repaired", LL_WARN));
	EXPECT_TRUE(_sink->has_log("Orderdetail cache file of rb2210 on date 20240108 repaired", LL_WARN));
	EXPECT_TRUE(_sink->has_log("Transaction cache file of rb2210 on date 20240108 repaired", LL_WARN));
}

// WtDataWriter.cpp: pipeToKlines L1093-1098 / L1200-1205（min1/min5 实时块 size==capacity 扩容）
TEST_F(R52WriterTest, R52Min1Min5BlocksGrowWhenCapacityExhausted)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// R23_DAY 日盘 09:00-11:30 / 13:00-15:00 共 270 分钟 → min1 容量 270、min5 容量 54
	TickSpec s;
	s.trading_date = 20240108;
	s.action_date = 20240108;
	s.price = 3600.0;
	s.volume = 1.0;
	s.turn_over = 3600.0;
	s.total_turnover = 3600.0;

	uint32_t seq = 0;
	auto send_minute = [&](uint32_t hh, uint32_t mm) {
		s.action_time = (hh * 100 + mm) * 100000;
		s.total_volume = 1000.0 + seq;
		seq++;
		write_tick(w, s, 0);
	};

	for (uint32_t hh = 9; hh <= 11; hh++)
		for (uint32_t mm = 0; mm < 60; mm++)
		{
			if (hh == 11 && mm >= 30)
				break;
			send_minute(hh, mm);
		}
	for (uint32_t hh = 13; hh <= 14; hh++)
		for (uint32_t mm = 0; mm < 60; mm++)
			send_minute(hh, mm);
	ASSERT_EQ(read_block_header<RTDayBlockHeader>(rt_min1_file("rb2210"))._size, 270u);

	// 次日第一笔：两根都有新 bar，写前 size==capacity → 同时扩容（L1093-1098 / L1200-1205）
	s.trading_date = 20240109;
	s.action_date = 20240109;
	s.action_time = 90000000;
	s.total_volume = 4000.0;
	write_tick(w, s, 0);

	const RTDayBlockHeader min1 = read_block_header<RTDayBlockHeader>(rt_min1_file("rb2210"));
	EXPECT_EQ(min1._capacity, 540u);		// 270*2
	EXPECT_EQ(min1._size, 271u);
	const RTDayBlockHeader min5 = read_block_header<RTDayBlockHeader>(rt_min5_file("rb2210"));
	EXPECT_EQ(min5._capacity, 108u);		// 54*2
	EXPECT_EQ(min5._size, 55u);
}

// WtDataWriter.cpp: dump_day_data L1922-2006（altcode 改名 + 旧文件续写 + >100 条压缩）
//                   + dump_bars_to_file L2079-2106 / L2158-2163（min1/min5 同名改名与追加）
TEST_F(R52WriterTest, R52DayBarAltCodeRenameAndCompressionAfterHundredBars)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	WTSContractInfo* ct = _bd->getContract("rb2210", "SHFE");
	ASSERT_TRUE(ct != NULL);
	ct->setAltCode("rb210");

	// 旧日线 100 条（RAW_V2），最后一条 20231199 < 新 tick 的 20240108
	vector<WTSBarStruct> oldday;
	for (uint32_t i = 0; i < 100; i++)
		oldday.push_back(q03::make_bar(20231101 + i, 0, i));
	ASSERT_TRUE(q03::write_bytes(his_day_file("rb210"),
		q03::make_block(BT_HIS_Day, BLOCK_VERSION_RAW_V2, q03::pack_bars(oldday))));

	ASSERT_TRUE(q03::write_bytes(his_min1_file("rb210"),
		q03::make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2,
			q03::pack_bars(vector<WTSBarStruct>{
				q03::make_bar(20240105, min_bar_time(20240105, 1459), 1)}))));
	ASSERT_TRUE(q03::write_bytes(his_min5_file("rb210"),
		q03::make_block(BT_HIS_Minute5, BLOCK_VERSION_RAW_V2,
			q03::pack_bars(vector<WTSBarStruct>{
				q03::make_bar(20240105, min_bar_time(20240105, 1455), 2)}))));

	TickSpec s;
	s.trading_date = 20240108;
	s.action_date = 20240108;
	s.action_time = 93000000;
	s.total_volume = 1000.0;
	s.total_turnover = 3600000.0;
	write_tick(w, s, 0);

	abandon_writer(w);
	w->transHisData("R52_ALT");
	ASSERT_TRUE(wait_marker("R52_ALT")) << "收盘作业没在 10s 内排空队列";

	// 日线：rb210.dsb → rb2210.dsb，101 条触发 CMP_V2
	EXPECT_FALSE(file_exists(his_day_file("rb210")));
	ASSERT_TRUE(file_exists(his_day_file("rb2210")));
	bool ok = false;
	vector<WTSBarStruct> day = load_cmp_bars(his_day_file("rb2210"), ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(day.size(), (std::size_t)101);
	EXPECT_EQ(day.front().date, 20231101u);
	EXPECT_EQ(day.back().date, 20240108u);
	EXPECT_DOUBLE_EQ(day.back().close, 3600.0);
	EXPECT_DOUBLE_EQ(day.back().vol, 1000.0);

	// 分钟线：旧文件 1 条 + 本次 1 条 → 2 条
	EXPECT_FALSE(file_exists(his_min1_file("rb210")));
	ASSERT_TRUE(file_exists(his_min1_file("rb2210")));
	vector<WTSBarStruct> m1 = load_cmp_bars(his_min1_file("rb2210"), ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(m1.size(), (std::size_t)2);
	EXPECT_EQ(m1[0].time % 10000, 1459u);
	EXPECT_EQ(m1[1].time % 10000, 931u);

	EXPECT_FALSE(file_exists(his_min5_file("rb210")));
	ASSERT_TRUE(file_exists(his_min5_file("rb2210")));
	vector<WTSBarStruct> m5 = load_cmp_bars(his_min5_file("rb2210"), ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(m5.size(), (std::size_t)2);
	EXPECT_EQ(m5[0].time % 10000, 1455u);
	EXPECT_EQ(m5[1].time % 10000, 935u);
}

} // namespace r52
