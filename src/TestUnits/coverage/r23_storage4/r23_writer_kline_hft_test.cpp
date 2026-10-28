// r23_storage4 / WtDataStorage：WtDataWriter 的分钟线合成（pipeToKlines）与逐笔落盘（procQueue/Order/Trans）
//
// 覆盖对象（src/WtDataStorage/WtDataWriter.cpp，全部经公开入口 writeTick/writeOrderQueue/
// writeOrderDetail/writeTransaction 驱动，async=false 的同步分支，不使用 #define private public）：
//  - pipeToKlines  :1056-1297  min1/min5 两段 switch、分钟桶标签、小节末归位、新建 bar vs 合并 bar、
//                              零价补 open/low、_min_price_mode==1 的 bid/ask 复用 hold/add 联合体、
//                              非法时间戳早退、_skip_notrade_bar/_skip_notrade_tick 两个开关
//  - procQueue     :429-468    canSessionReceive 过滤、rt/queue 块写入、broadcastOrdQue、分组计数日志
//  - procOrder     :483-522    rt/orders 块
//  - procTrans     :537-576    rt/trans 块
//  - writeOrderQueue/Detail/Transaction :416/:470/:524 的 NULL 与 disable* 拒绝面
//
// ⚠ 本文件有一条「行为固化」用例（DisableOrdDtlAlsoBlocksTransaction）记录的是
//    BUG-R23-002（writeTransaction 判的是 _disable_orddtl 而不是 _disable_trans）的**现状**，
//    源码修复后该用例会变红，需要同步更新期望值。
#include "r23_writer_fixture.h"

namespace r23 {

namespace {

// min1/min5 块的头部（RTKlineBlock = RTDayBlockHeader + bars[0]，取无柔性数组的同形父结构）
typedef RTDayBlockHeader KHead;

inline WTSBarStruct read_bar(const string& file, uint32_t idx)
{
	WTSBarStruct bar;
	std::memset(&bar, 0, sizeof(bar));
	const bool got = read_block_item<KHead, WTSBarStruct>(file, idx, bar);
	EXPECT_TRUE(got) << file << " 第 " << idx << " 条读不到";
	return bar;
}

} // namespace

// ------------------------------------------------------------------ min1 合成
TEST_F(R23WriterTest, Min1BarIsLabelledWithMinuteEnd)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec spec;			// 默认 09:30:00.000、price 3600、high 3610、low 3580
	spec.volume = 12.0;
	spec.turn_over = 36000.0;
	spec.diff_interest = 7.0;
	write_tick(w, spec, 0);

	const string min1 = rt_min1_file("rb2210");
	ASSERT_TRUE(file_exists(min1)) << "pipeToKlines 没有建 min1 实时块";
	const KHead head = read_block_header<KHead>(min1);
	EXPECT_EQ(head._size, 1u);
	EXPECT_EQ(head._capacity, 270u);      // getTradingMins()：150 + 120
	EXPECT_EQ(head._type, (uint32_t)BT_RT_Minute1);

	const WTSBarStruct bar = read_bar(min1, 0);
	// 09:30 这笔属于 09:30~09:31 这一根，条上打的是「分末」标签 931
	EXPECT_EQ(bar.time % 10000, 931u);
	EXPECT_EQ(bar.time / 10000, (uint64_t)(20240108 - 19900000));
	EXPECT_EQ(bar.date, 20240108u);
	EXPECT_DOUBLE_EQ(bar.open, 3600.0);
	EXPECT_DOUBLE_EQ(bar.close, 3600.0);
	// 当日首笔 tick 的新高新低标志为 1|2 → 直接取 tick 的 high/low
	EXPECT_DOUBLE_EQ(bar.high, 3610.0);
	EXPECT_DOUBLE_EQ(bar.low, 3580.0);
	EXPECT_DOUBLE_EQ(bar.vol, 12.0);
	EXPECT_DOUBLE_EQ(bar.money, 36000.0);
	EXPECT_DOUBLE_EQ(bar.hold, 90000.0);
	EXPECT_DOUBLE_EQ(bar.add, 7.0);
}

TEST_F(R23WriterTest, SecondTickSameMinuteMergesOhlcAndAmounts)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;			// 09:30:00.000
	t1.price = 3600.0;
	t1.high = 3610.0;
	t1.low = 3580.0;
	t1.volume = 10.0;
	t1.turn_over = 36000.0;
	t1.diff_interest = 7.0;
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 93030000;      // 09:30:30.000 → 同一根 min1
	t2.price = 3620.0;
	t2.high = 3600.0;               // 不高于缓存 high(3610) → 不是新高
	t2.low = 3590.0;                // 不低于缓存 low(3580) → 不是新低
	t2.total_volume = 1100.0;       // 必须 >= 缓存总量才不会被丢弃
	t2.volume = 20.0;
	t2.turn_over = 72400.0;
	t2.diff_interest = 5.0;
	WTSTickData* d2 = write_tick(w, t2, 0);
	ASSERT_TRUE(d2 != NULL);
	EXPECT_EQ(d2->getLimitFlag(), 0u);

	const string min1 = rt_min1_file("rb2210");
	const KHead head = read_block_header<KHead>(min1);
	EXPECT_EQ(head._size, 1u) << "同一分钟的第二笔不该新开一根 bar";

	const WTSBarStruct bar = read_bar(min1, 0);
	EXPECT_DOUBLE_EQ(bar.open, 3600.0);
	EXPECT_DOUBLE_EQ(bar.close, 3620.0);
	// 非新高 → high = max(price, 原 high) = max(3620, 3610)
	EXPECT_DOUBLE_EQ(bar.high, 3620.0);
	// 非新低 → low = min(price, 原 low) = min(3620, 3580)
	EXPECT_DOUBLE_EQ(bar.low, 3580.0);
	EXPECT_DOUBLE_EQ(bar.vol, 30.0);
	EXPECT_DOUBLE_EQ(bar.money, 108400.0);
	// procFlag==0 分支：hold 取最新值、add 累加
	EXPECT_DOUBLE_EQ(bar.hold, 90000.0);
	EXPECT_DOUBLE_EQ(bar.add, 12.0);
}

TEST_F(R23WriterTest, Min5BucketsEveryFiveMinutes)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.action_time = 90300000;      // 09:03 → 第 3 分钟 → min5 桶 [09:00,09:05] 标签 905
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 90700000;      // 09:07 → 第 7 分钟 → min5 桶标签 910
	t2.total_volume = 1100.0;
	write_tick(w, t2, 0);

	const string min5 = rt_min5_file("rb2210");
	ASSERT_TRUE(file_exists(min5));
	const KHead head = read_block_header<KHead>(min5);
	EXPECT_EQ(head._size, 2u) << "09:03 与 09:07 应落在相邻两根 5 分钟线里";
	EXPECT_EQ(head._capacity, 54u);       // 270/5
	EXPECT_EQ(head._type, (uint32_t)BT_RT_Minute5);

	const WTSBarStruct b0 = read_bar(min5, 0);
	const WTSBarStruct b1 = read_bar(min5, 1);
	EXPECT_EQ(b0.time % 10000, 905u);
	EXPECT_EQ(b1.time % 10000, 910u);
	EXPECT_DOUBLE_EQ(b0.open, 3600.0);
	EXPECT_DOUBLE_EQ(b1.open, 3600.0);

	// 同一测试顺手核对 min1：09:03→904、09:07→908
	const WTSBarStruct m0 = read_bar(rt_min1_file("rb2210"), 0);
	const WTSBarStruct m1 = read_bar(rt_min1_file("rb2210"), 1);
	EXPECT_EQ(m0.time % 10000, 904u);
	EXPECT_EQ(m1.time % 10000, 908u);
}

TEST_F(R23WriterTest, LastMinuteOfSectionFallsIntoSectionCloseLabel)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.action_time = 112900000;     // 11:29:00.000 → 第 149 分钟 → 标签 1130
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 113000000;     // 11:30:00.000 正好是小节末界
	t2.price = 3621.0;
	t2.high = 3600.0;
	t2.low = 3590.0;
	t2.total_volume = 1100.0;
	t2.volume = 20.0;
	t2.turn_over = 72420.0;
	write_tick(w, t2, 0);

	// isLastOfSection(1130) 为真 → minutes-- → 仍然归到 11:29~11:30 这一根（:1080-1083）
	const string min1 = rt_min1_file("rb2210");
	const KHead head = read_block_header<KHead>(min1);
	EXPECT_EQ(head._size, 1u) << "小节末的 tick 被另算成了一根新 bar";

	const WTSBarStruct bar = read_bar(min1, 0);
	EXPECT_EQ(bar.time % 10000, 1130u);
	EXPECT_DOUBLE_EQ(bar.close, 3621.0);
	EXPECT_DOUBLE_EQ(bar.vol, 30.0);
}

TEST_F(R23WriterTest, ZeroPriceOpeningTickIsBackfilledByNextTick)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.price = 0.0;
	t1.open = 0.0;
	t1.high = 0.0;
	t1.low = 0.0;
	t1.settle = 0.0;
	t1.total_volume = 0.0;
	t1.volume = 0.0;
	t1.total_turnover = 0.0;
	t1.turn_over = 0.0;
	t1.open_interest = 0.0;
	t1.pre_interest = 0.0;
	t1.diff_interest = 0.0;
	write_tick(w, t1, 0);

	const string min1 = rt_min1_file("rb2210");
	{
		const KHead head = read_block_header<KHead>(min1);
		ASSERT_EQ(head._size, 1u);
		const WTSBarStruct zero = read_bar(min1, 0);
		EXPECT_DOUBLE_EQ(zero.open, 0.0);
		EXPECT_DOUBLE_EQ(zero.low, 0.0);
	}

	TickSpec t2;
	t2.action_time = 93000500;      // 同一分钟，价格正常
	t2.price = 3600.0;
	t2.high = 3610.0;
	t2.low = 3580.0;
	t2.total_volume = 100.0;
	t2.volume = 100.0;
	t2.turn_over = 360000.0;
	WTSTickData* d2 = write_tick(w, t2, 0);
	ASSERT_TRUE(d2 != NULL);
	// 缓存里 high=0/low=0 → 3610>0 算新高，3580<0 不成立 → flag 只有 1
	EXPECT_EQ(d2->getLimitFlag(), 1u);

	const KHead head = read_block_header<KHead>(min1);
	EXPECT_EQ(head._size, 1u);
	const WTSBarStruct bar = read_bar(min1, 0);
	// open/low 为 0 的补写分支（:1161-1167）
	EXPECT_DOUBLE_EQ(bar.open, 3600.0);
	EXPECT_DOUBLE_EQ(bar.low, 3600.0);
	// 新高 → high 直接取 tick 的 high
	EXPECT_DOUBLE_EQ(bar.high, 3610.0);
	EXPECT_DOUBLE_EQ(bar.close, 3600.0);
	EXPECT_DOUBLE_EQ(bar.vol, 100.0);
}

TEST_F(R23WriterTest, InvalidActionTimeOnlyWarnsAndKeepsBars)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	write_tick(w, t1, 0);
	const string min1 = rt_min1_file("rb2210");
	ASSERT_EQ(read_block_header<KHead>(min1)._size, 1u);

	TickSpec t2;
	t2.action_time = 0;             // 0900 之外的时间 → timeToMinutes 返回 INVALID
	t2.total_volume = 1100.0;
	WTSTickData* d2 = write_tick(w, t2, 0);
	ASSERT_TRUE(d2 != NULL);
	// 顺带把「同一时间戳进位」也算了一遍：0 <= 缓存 93000000 且总量更大 → 0 + min(950,200) = 200
	EXPECT_EQ(d2->actiontime(), 200u);

	EXPECT_TRUE(_sink->has_log("is invalid timestamp, skip this tick", LL_WARN));
	EXPECT_EQ(read_block_header<KHead>(min1)._size, 1u) << "非法时间戳仍然合了 bar";
	EXPECT_EQ(_sink->ticks(), 2u) << "早退只该挡住 K 线，不该挡掉 broadcastTick";
}

// ------------------------------------------------------------------ 两个 skip 开关
TEST_F(R23WriterTest, SkipNotradeBarSuppressesBarGeneration)
{
	R23Writer* w = make_writer("\"skip_notrade_bar\":true");
	ASSERT_TRUE(w != NULL);
	EXPECT_TRUE(_sink->has_log("disable_history: false", LL_INFO));

	TickSpec no_trade;
	no_trade.turn_over = 0.0;       // 无成交
	no_trade.total_volume = 0.0;
	no_trade.volume = 0.0;
	no_trade.total_turnover = 0.0;
	write_tick(w, no_trade, 0);

	// pipeToKlines 第一行就返回（:1061-1064），但 tick 块照常
	EXPECT_FALSE(file_exists(rt_min1_file("rb2210")));
	EXPECT_TRUE(file_exists(rt_tick_file("rb2210")));
	EXPECT_EQ(_sink->ticks(), 1u);

	TickSpec traded;
	traded.action_time = 93100000;
	traded.total_volume = 100.0;
	traded.volume = 100.0;
	traded.turn_over = 360000.0;
	write_tick(w, traded, 0);

	// 有成交的 tick 仍然建 bar
	const string min1 = rt_min1_file("rb2210");
	ASSERT_TRUE(file_exists(min1));
	const KHead head = read_block_header<KHead>(min1);
	ASSERT_EQ(head._size, 1u);
	const WTSBarStruct bar = read_bar(min1, 0);
	EXPECT_EQ(bar.time % 10000, 932u);
	EXPECT_DOUBLE_EQ(bar.vol, 100.0);
}

TEST_F(R23WriterTest, SkipNotradeTickMergesOnlyTradingTicks)
{
	R23Writer* w = make_writer("\"skip_notrade_tick\":true");
	ASSERT_TRUE(w != NULL);

	TickSpec t1;
	t1.price = 3600.0;
	t1.volume = 10.0;
	t1.turn_over = 36000.0;
	write_tick(w, t1, 0);

	TickSpec t2;
	t2.action_time = 93030000;      // 同一分钟
	t2.price = 3650.0;
	t2.high = 3660.0;               // 比缓存 high(3610) 高 → 新高
	t2.low = 3570.0;                // 比缓存 low(3580) 低 → 新低
	t2.total_volume = 1100.0;       // 不被总量回退判掉
	t2.volume = 20.0;
	t2.turn_over = 0.0;             // 无成交
	write_tick(w, t2, 0);

	const string min1 = rt_min1_file("rb2210");
	const KHead head = read_block_header<KHead>(min1);
	EXPECT_EQ(head._size, 1u);
	const WTSBarStruct bar = read_bar(min1, 0);
	// 合并分支被 `_skip_notrade_tick && tickNoTrade` 挡掉（:1152）→ 全部维持 t1 的值
	EXPECT_DOUBLE_EQ(bar.close, 3600.0);
	EXPECT_DOUBLE_EQ(bar.high, 3610.0);
	EXPECT_DOUBLE_EQ(bar.low, 3580.0);
	EXPECT_DOUBLE_EQ(bar.vol, 10.0);
	EXPECT_DOUBLE_EQ(bar.money, 36000.0);

	// 但缓存仍然被这笔无成交 tick 更新了（开关只作用于 K 线合成）
	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->price(), 3650.0);
	cached->release();
}

TEST_F(R23WriterTest, MinPriceModeOneWritesBidAskIntoHoldAddUnion)
{
	R23Writer* w = make_writer("\"minbar_price_mode\":1");
	ASSERT_TRUE(w != NULL);
	EXPECT_TRUE(_sink->has_log("min_price_mode: 1", LL_INFO));

	TickSpec spec;
	spec.price = 3600.0;
	write_tick(w, spec, 0);

	const WTSBarStruct bar = read_bar(rt_min1_file("rb2210"), 0);
	// 夹具里 bid_prices[0] = price-0.5、ask_prices[0] = price+0.5
	EXPECT_DOUBLE_EQ(bar.bid, 3599.5);
	EXPECT_DOUBLE_EQ(bar.ask, 3600.5);
	// hold/bid、add/ask 是同一个联合体（WTSStruct.h:114-124）
	EXPECT_DOUBLE_EQ(bar.hold, 3599.5);
	EXPECT_DOUBLE_EQ(bar.add, 3600.5);
	EXPECT_NE(bar.hold, 90000.0) << "mode==1 时不该再写持仓";
}

// ------------------------------------------------------------------ 逐笔三类
TEST_F(R23WriterTest, OrderQueueWritesRtBlockAndCountsReceipts)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	OrdQueSpec q1;
	q1.price = 3600.0;
	q1.order_items = 3;
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q1)));

	OrdQueSpec q2;
	q2.price = 3601.0;
	q2.order_items = 4;
	q2.action_time = 93000100;
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q2)));

	const string que = _base + "rt/queue/SHFE/rb2210.dmb";
	ASSERT_TRUE(file_exists(que)) << "procQueue 没有建委托队列块";
	const RTDayBlockHeader head = read_block_header<RTDayBlockHeader>(que);
	EXPECT_EQ(head._size, 2u);
	EXPECT_EQ(head._capacity, 2500u);
	EXPECT_EQ(head._type, (uint32_t)BT_RT_OrdQueue);
	EXPECT_EQ(head._date, 20240108u);

	WTSOrdQueStruct item;
	const bool got0 = read_block_item<RTDayBlockHeader, WTSOrdQueStruct>(que, 0, item);
	ASSERT_TRUE(got0);
	EXPECT_DOUBLE_EQ(item.price, 3600.0);
	EXPECT_EQ(item.order_items, 3u);
	EXPECT_EQ(item.qsize, 5u);
	EXPECT_DOUBLE_EQ(item.volumes[0], 10.0);
	EXPECT_DOUBLE_EQ(item.volumes[49], 59.0);
	EXPECT_EQ(item.side, BDT_Sell);      // order_items 为奇数

	EXPECT_EQ(_sink->queues(), 2u);
	EXPECT_TRUE(_sink->has_log("2 queues received from exchange SHFE", LL_INFO));
}

TEST_F(R23WriterTest, OrderDetailAndTransactionWriteTheirOwnRtBlocks)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	ASSERT_TRUE(w->writeOrderDetail(make_orddtl_data(93000000, 3600.0, 1)));
	ASSERT_TRUE(w->writeTransaction(make_trans_data(93000000, 3601.0, 2)));

	const string orders = _base + "rt/orders/SHFE/rb2210.dmb";
	const string trans = _base + "rt/trans/SHFE/rb2210.dmb";
	ASSERT_TRUE(file_exists(orders));
	ASSERT_TRUE(file_exists(trans));

	const RTDayBlockHeader ohead = read_block_header<RTDayBlockHeader>(orders);
	EXPECT_EQ(ohead._size, 1u);
	EXPECT_EQ(ohead._type, (uint32_t)BT_RT_OrdDetail);
	const RTDayBlockHeader thead = read_block_header<RTDayBlockHeader>(trans);
	EXPECT_EQ(thead._size, 1u);
	EXPECT_EQ(thead._type, (uint32_t)BT_RT_Trnsctn);

	WTSOrdDtlStruct od;
	const bool got_od = read_block_item<RTDayBlockHeader, WTSOrdDtlStruct>(orders, 0, od);
	ASSERT_TRUE(got_od);
	EXPECT_DOUBLE_EQ(od.price, 3600.0);
	EXPECT_DOUBLE_EQ(od.volume, 100.0);
	EXPECT_EQ(od.index, 1u);
	EXPECT_EQ(od.orderid, 700001u);
	EXPECT_EQ(od.side, BDT_Buy);
	EXPECT_EQ(od.otype, ODT_LimitPrice);

	WTSTransStruct tr;
	const bool got_tr = read_block_item<RTDayBlockHeader, WTSTransStruct>(trans, 0, tr);
	ASSERT_TRUE(got_tr);
	EXPECT_DOUBLE_EQ(tr.price, 3601.0);
	EXPECT_DOUBLE_EQ(tr.volume, 200.0);
	EXPECT_EQ(tr.index, 2u);
	EXPECT_EQ(tr.askorder, 300002u);
	EXPECT_EQ(tr.bidorder, 400002u);
	EXPECT_EQ(tr.ttype, TT_Match);

	EXPECT_EQ(_sink->dtls(), 1u);
	EXPECT_EQ(_sink->trans(), 1u);
	EXPECT_TRUE(_sink->has_log("1 orders received from exchange SHFE", LL_INFO));
	EXPECT_TRUE(_sink->has_log("1 transactions received from exchange SHFE", LL_INFO));
}

TEST_F(R23WriterTest, NullAndDisabledHftWritersAreRejected)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	// NULL 入参拒绝面
	EXPECT_FALSE(w->writeOrderQueue(NULL));
	EXPECT_FALSE(w->writeOrderDetail(NULL));
	EXPECT_FALSE(w->writeTransaction(NULL));

	// 队列未禁用 → 仍然可写
	{
		OrdQueSpec q;
		q.price = 3610.0;
		ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q)));
	}
	EXPECT_TRUE(file_exists(_base + "rt/queue/SHFE/rb2210.dmb"));

	R23Writer* w2 = make_writer("\"disableordque\":true");
	ASSERT_TRUE(w2 != NULL);
	OrdQueSpec q;
	q.price = 3611.0;
	EXPECT_FALSE(w2->writeOrderQueue(make_ordque_data(q)));
	EXPECT_EQ(_sink->queues(), 1u) << "被禁用的队列写入仍然走了 broadcastOrdQue";
}

// ------------------------------------------------------------------ BUG-R23-002 现状固化
TEST_F(R23WriterTest, DisableOrdDtlAlsoBlocksTransaction)
{
	// 现状：writeTransaction 判的是 _disable_orddtl（WtDataWriter.cpp:526），
	// _disable_trans 只在 proc_loop 的收盘作业里被读（:2492）。
	// 本用例固化的是**当前行为**，BUG-R23-002 修好后需要把期望改成 true。
	R23Writer* w = make_writer("\"disableorddtl\":true");
	ASSERT_TRUE(w != NULL);

	WTSOrdDtlData* dtl = make_orddtl_data(93000000, 3600.0, 1);
	WTSTransData* trns = make_trans_data(93000000, 3601.0, 2);
	EXPECT_FALSE(w->writeOrderDetail(dtl));
	EXPECT_FALSE(w->writeTransaction(trns));      // ← 这里本该放行（disabletrans=false）
	EXPECT_FALSE(file_exists(_base + "rt/trans/SHFE/rb2210.dmb"));
	EXPECT_FALSE(file_exists(_base + "rt/orders/SHFE/rb2210.dmb"));

	// 对照组：只关 trans 开关时，逐笔成交照样写（证明 _disable_trans 对写入路径无影响）
	R23Writer* w2 = make_writer("\"disabletrans\":true");
	ASSERT_TRUE(w2 != NULL);
	EXPECT_TRUE(w2->writeTransaction(make_trans_data(93100000, 3602.0, 3)));
	EXPECT_TRUE(file_exists(_base + "rt/trans/SHFE/rb2210.dmb"));
	EXPECT_EQ(_sink->trans(), 1u);
}

TEST_F(R23WriterTest, SessionNotReceivingStopsAllHftWriters)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	_sink->set_can_receive(false);

	OrdQueSpec q;
	q.price = 3600.0;
	// 入口不返回 false（只有 NULL/disable 才返回 false），但 proc* 会在 canSessionReceive 处 break
	EXPECT_TRUE(w->writeOrderQueue(make_ordque_data(q)));
	EXPECT_TRUE(w->writeOrderDetail(make_orddtl_data(93000000, 3600.0, 1)));
	EXPECT_TRUE(w->writeTransaction(make_trans_data(93000000, 3601.0, 2)));

	EXPECT_EQ(_sink->queues(), 0u);
	EXPECT_EQ(_sink->dtls(), 0u);
	EXPECT_EQ(_sink->trans(), 0u);
	EXPECT_FALSE(file_exists(_base + "rt/queue/SHFE/rb2210.dmb"));
	EXPECT_FALSE(file_exists(_base + "rt/orders/SHFE/rb2210.dmb"));
	EXPECT_FALSE(file_exists(_base + "rt/trans/SHFE/rb2210.dmb"));
}

} // namespace r23
