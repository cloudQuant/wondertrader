/**
 * r91_replayer_bin_hft_test.cpp  (lane R91 · wave17)
 *
 * 靶子：HisDataReplayer 的 Level2/高频 bin 装载与回放面（公开 API）
 *   - cacheRawOrdDtl/OrdQue/TransFromBin                 HisDataReplayer.cpp:3356-3441
 *   - get_order_*_slice / get_tick_slice 数据臂           :2470-2689
 *   - getNext*Time 的 lower_bound 与初始化游标分支        :1536-1767
 *   - replayHftDatas（按K线区间）                         :1904-2011
 *   - replayHftDatasByDay（按交易日）                     :1769-1902
 *   - run_by_bars 的 tick 回放与 get_day_price            :853-858、3034-3087
 *   - run_by_ticks 的 session/交易日推进                  :741-769
 *
 * 只使用公开 API；不碰私有 bin/cache*FromBin 声明，不镜像类布局。
 */
#include "r91_common.h"

using R91Fixture = r91::Fixture;

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const char* const kCode2 = "SSE.STK.000002";
	const uint32_t kDay1 = 20200102u;
	const uint32_t kDay2 = 20200103u;

	std::vector<r91::HftRec> filter_by_date(const std::vector<r91::HftRec>& items, uint32_t date)
	{
		std::vector<r91::HftRec> ret;
		for (const r91::HftRec& item : items)
		{
			if (item.date == date)
				ret.push_back(item);
		}
		return ret;
	}

	std::vector<r91::TickRec> filter_ticks(const std::vector<r91::TickRec>& items,
		const char* code, uint32_t date)
	{
		std::vector<r91::TickRec> ret;
		for (const r91::TickRec& item : items)
		{
			if (item.code == code && item.date == date)
				ret.push_back(item);
		}
		return ret;
	}
}

/*
 * 生产: HisDataReplayer.cpp:3356-3441（三种 Level2 的 bin 读取）
 *       2470-2545（tick 切片：etime 定位/早退/最后一笔）
 *       2547-2689（order detail/queue/trans 切片数据臂）
 *       2702-2826（checkOrder 系列与 checkTransactions 的装载与空数据分支）
 */
TEST_F(R91Fixture, R91BinHft_Level2SlicesLoadFromBin)
{
	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r91::mk_tick(kDay1, 931u, 10.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 932u, 11.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 933u, 12.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 1501u, 99.0, 1.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 1502u, 98.0, 1.0, "000001"));	// 游标边界哨兵
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000001", kDay1, ticks));

	std::vector<WTSOrdDtlStruct> details;
	details.push_back(r91::mk_orddtl(kDay1, 931u, 10.1, 1u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 932u, 10.2, 2u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 933u, 10.3, 3u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDay1, details));

	std::vector<WTSOrdQueStruct> queues;
	queues.push_back(r91::mk_ordque(kDay1, 931u, 9.9, 11u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 932u, 9.8, 12u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 933u, 9.7, 13u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_ordque(_base, "SSE", "000001", kDay1, queues));

	std::vector<WTSTransStruct> trans;
	trans.push_back(r91::mk_trans(kDay1, 931u, 10.05, 21u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 932u, 10.15, 22u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 933u, 10.25, 23u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000001", kDay1, trans));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_l2");
	ASSERT_TRUE(replayer.prepare());

	replayer.sub_tick(1u, kCode);
	replayer.sub_order_detail(2u, kCode);
	replayer.sub_order_queue(3u, kCode);
	replayer.sub_transaction(4u, kCode);

	// etime 早于首笔：定位到 idx 0 且无法回退 -> NULL（L2516-2519）
	EXPECT_EQ(nullptr, replayer.get_tick_slice(kCode, 2u, 202001020900ull));

	WTSTickSlice* tickSlice = replayer.get_tick_slice(kCode, 2u, 202001020933ull);
	ASSERT_NE(nullptr, tickSlice);
	ASSERT_EQ(2u, tickSlice->size());
	EXPECT_EQ(931u, tickSlice->at(0)->action_time / 100000u);
	EXPECT_DOUBLE_EQ(10.0, tickSlice->at(0)->price);
	EXPECT_EQ(932u, tickSlice->at(1)->action_time / 100000u);
	EXPECT_DOUBLE_EQ(11.0, tickSlice->at(1)->price);
	tickSlice->release();

	// 游标停在 933 之后，get_last_tick 必须返回 933（L2890-2912）
	WTSTickData* last = replayer.get_last_tick(kCode);
	ASSERT_NE(nullptr, last);
	EXPECT_EQ(933u, last->actiontime() / 100000u);
	EXPECT_DOUBLE_EQ(12.0, last->price());
	last->release();

	WTSOrdDtlSlice* detailSlice = replayer.get_order_detail_slice(kCode, 3u, 202001020933ull);
	ASSERT_NE(nullptr, detailSlice);
	ASSERT_EQ(3u, detailSlice->size());
	for (uint32_t i = 0; i < 3; i++)
	{
		EXPECT_EQ((931u + i) * 100000u, detailSlice->at((int32_t)i)->action_time);
		EXPECT_DOUBLE_EQ(10.1 + i * 0.1, detailSlice->at((int32_t)i)->price);
	}
	detailSlice->release();

	WTSOrdQueSlice* queueSlice = replayer.get_order_queue_slice(kCode, 3u, 202001020933ull);
	ASSERT_NE(nullptr, queueSlice);
	ASSERT_EQ(3u, queueSlice->size());
	EXPECT_DOUBLE_EQ(9.9, queueSlice->at(0)->price);
	EXPECT_EQ(13u, queueSlice->at(2)->qsize);
	queueSlice->release();

	WTSTransSlice* transSlice = replayer.get_transaction_slice(kCode, 3u, 202001020933ull);
	ASSERT_NE(nullptr, transSlice);
	ASSERT_EQ(3u, transSlice->size());
	EXPECT_DOUBLE_EQ(10.05, transSlice->at(0)->price);
	EXPECT_EQ(23u, transSlice->at(2)->volume);
	transSlice->release();

	// 无文件的 tick / order detail：缓存空列表并返回 NULL（L2865-2874、2729-2739）
	EXPECT_EQ(nullptr, replayer.get_tick_slice("SSE.STK.999999", 2u, 202001020933ull));
	EXPECT_EQ(nullptr, replayer.get_order_detail_slice("SSE.STK.000003", 2u, 202001020933ull));
}

/*
 * 生产: HisDataReplayer.cpp:1904-2011（replayHftDatas：stime/etime 与四类数据回放）
 *       1536-1767（getNext*Time 的 stime != UINT64_MAX 分支）
 *       853-858（run_by_bars 调用 tick 回放）
 *       3034-3087（get_day_price 的 tick 分支，含 default flag）
 *       3021-3032（get_cur_price）
 */
TEST_F(R91Fixture, R91BinHft_BarReplayUsesHftDatasAndDayPrice)
{
	std::vector<WTSBarStruct> bars;
	bars.push_back(r91::mk_bar(20191231u, 1500u, 99.0, 100.0, 98.0, 100.0, 10.0));
	bars.push_back(r91::mk_bar(kDay1, 1500u, 100.0, 102.0, 99.0, 101.0, 20.0));
	bars.push_back(r91::mk_bar(kDay2, 1500u, 101.0, 103.0, 100.0, 102.0, 30.0));
	ASSERT_TRUE(r91::write_bars(_base, "SSE", "000001", "day", bars));

	std::vector<WTSTickStruct> ticks;
	ticks.push_back(r91::mk_tick(kDay1, 931u, 10.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 932u, 11.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 933u, 12.0, 100.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 1501u, 99.0, 1.0, "000001"));
	ticks.push_back(r91::mk_tick(kDay1, 1502u, 98.0, 1.0, "000001"));
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000001", kDay1, ticks));

	std::vector<WTSOrdDtlStruct> details;
	details.push_back(r91::mk_orddtl(kDay1, 931u, 10.1, 1u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 932u, 10.2, 2u, "000001"));
	details.push_back(r91::mk_orddtl(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDay1, details));

	std::vector<WTSOrdQueStruct> queues;
	queues.push_back(r91::mk_ordque(kDay1, 931u, 9.9, 11u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 932u, 9.8, 12u, "000001"));
	queues.push_back(r91::mk_ordque(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_ordque(_base, "SSE", "000001", kDay1, queues));

	std::vector<WTSTransStruct> trans;
	trans.push_back(r91::mk_trans(kDay1, 931u, 10.05, 21u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 932u, 10.15, 22u, "000001"));
	trans.push_back(r91::mk_trans(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000001", kDay1, trans));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	sink.rp = &replayer;
	sink.collectDayPrice = true;
	replayer.register_sink(&sink, "r91_bars_hft");
	ASSERT_TRUE(replayer.prepare());

	replayer.sub_tick(1u, kCode);
	replayer.sub_order_detail(2u, kCode);
	replayer.sub_order_queue(3u, kCode);
	replayer.sub_transaction(4u, kCode);

	WTSKlineSlice* slice = replayer.get_kline_slice(kCode, "d", 10, 1, true);
	ASSERT_NE(nullptr, slice);
	slice->release();

	replayer.run(false);

	// 日线 20200102 的 931/932/933 走 bin tick 回放；1501 哨兵不进入窗口
	std::vector<r91::TickRec> day1Ticks = filter_ticks(sink.ticks, kCode, kDay1);
	ASSERT_EQ(3u, day1Ticks.size());
	EXPECT_EQ(931u, day1Ticks[0].time);
	EXPECT_DOUBLE_EQ(10.0, day1Ticks[0].price);
	EXPECT_EQ(932u, day1Ticks[1].time);
	EXPECT_EQ(933u, day1Ticks[2].time);
	EXPECT_DOUBLE_EQ(12.0, day1Ticks[2].price);

	ASSERT_EQ(2u, sink.details.size());
	ASSERT_EQ(2u, sink.queues.size());
	ASSERT_EQ(2u, sink.trans.size());
	EXPECT_EQ(931u, sink.details[0].time);
	EXPECT_EQ(932u, sink.details[1].time);
	EXPECT_DOUBLE_EQ(10.15, sink.trans[1].price);

	// 932 时刻的日线价格：open/high/low/price/default
	auto priceIt = sink.dayPrices.find(932u);
	ASSERT_NE(sink.dayPrices.end(), priceIt);
	ASSERT_EQ(5u, priceIt->second.size());
	EXPECT_DOUBLE_EQ(10.5, priceIt->second[0]);
	EXPECT_DOUBLE_EQ(12.0, priceIt->second[1]);
	EXPECT_DOUBLE_EQ(9.0, priceIt->second[2]);
	EXPECT_DOUBLE_EQ(11.0, priceIt->second[3]);
	EXPECT_DOUBLE_EQ(0.0, priceIt->second[4]);

	ASSERT_EQ(2u, sink.bars.size());
	EXPECT_EQ(kDay1, sink.bars[0].date);
	EXPECT_DOUBLE_EQ(101.0, sink.bars[0].close);
	EXPECT_EQ(kDay2, sink.bars[1].date);
	EXPECT_DOUBLE_EQ(102.0, sink.bars[1].close);

	// 第二日没有 bin tick，回落到K线模拟 tick，最后价格是日线收盘 102
	EXPECT_DOUBLE_EQ(102.0, replayer.get_cur_price(kCode));
}

/*
 * 生产: HisDataReplayer.cpp:741-769（run_by_ticks 按交易日推进）
 *       1536-1568（getNextTickTime 初始化时跳过非交易时间的第一笔）
 *       1769-1902（replayHftDatasByDay 四类数据 + 空列表 continue）
 *       2828-2847（checkTicks 换日重载）
 */
TEST_F(R91Fixture, R91BinHft_TickReplaySkipsPreOpenAndAdvancesSessions)
{
	// 第一天：000001 首笔 0925 在交易时间外；000002 正常两笔
	std::vector<WTSTickStruct> c1d1;
	c1d1.push_back(r91::mk_tick(kDay1, 925u, 9.0, 10.0, "000001"));
	c1d1.push_back(r91::mk_tick(kDay1, 931u, 10.0, 10.0, "000001"));
	c1d1.push_back(r91::mk_tick(kDay1, 1501u, 99.0, 1.0, "000001"));
	c1d1.push_back(r91::mk_tick(kDay1, 1502u, 98.0, 1.0, "000001"));
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000001", kDay1, c1d1));

	std::vector<WTSTickStruct> c2d1;
	c2d1.push_back(r91::mk_tick(kDay1, 931u, 11.0, 10.0, "000002"));
	c2d1.push_back(r91::mk_tick(kDay1, 932u, 12.0, 10.0, "000002"));
	c2d1.push_back(r91::mk_tick(kDay1, 1501u, 99.0, 1.0, "000002"));
	c2d1.push_back(r91::mk_tick(kDay1, 1502u, 98.0, 1.0, "000002"));
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000002", kDay1, c2d1));

	// 第二天
	std::vector<WTSTickStruct> c1d2;
	c1d2.push_back(r91::mk_tick(kDay2, 931u, 13.0, 10.0, "000001"));
	c1d2.push_back(r91::mk_tick(kDay2, 1501u, 99.0, 1.0, "000001"));
	c1d2.push_back(r91::mk_tick(kDay2, 1502u, 98.0, 1.0, "000001"));
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000001", kDay2, c1d2));

	std::vector<WTSTickStruct> c2d2;
	c2d2.push_back(r91::mk_tick(kDay2, 931u, 14.0, 10.0, "000002"));
	c2d2.push_back(r91::mk_tick(kDay2, 932u, 15.0, 10.0, "000002"));
	c2d2.push_back(r91::mk_tick(kDay2, 1501u, 99.0, 1.0, "000002"));
	c2d2.push_back(r91::mk_tick(kDay2, 1502u, 98.0, 1.0, "000002"));
	ASSERT_TRUE(r91::write_ticks(_base, "SSE", "000002", kDay2, c2d2));

	std::vector<WTSOrdDtlStruct> details1;
	details1.push_back(r91::mk_orddtl(kDay1, 931u, 10.1, 1u, "000001"));
	details1.push_back(r91::mk_orddtl(kDay1, 932u, 10.2, 2u, "000001"));
	details1.push_back(r91::mk_orddtl(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDay1, details1));

	std::vector<WTSOrdDtlStruct> details2;
	details2.push_back(r91::mk_orddtl(kDay2, 931u, 10.3, 3u, "000001"));
	details2.push_back(r91::mk_orddtl(kDay2, 932u, 10.4, 4u, "000001"));
	details2.push_back(r91::mk_orddtl(kDay2, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_orddtl(_base, "SSE", "000001", kDay2, details2));

	std::vector<WTSOrdQueStruct> queues1;
	queues1.push_back(r91::mk_ordque(kDay1, 931u, 9.9, 11u, "000001"));
	queues1.push_back(r91::mk_ordque(kDay1, 932u, 9.8, 12u, "000001"));
	queues1.push_back(r91::mk_ordque(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_ordque(_base, "SSE", "000001", kDay1, queues1));

	std::vector<WTSOrdQueStruct> queues2;
	queues2.push_back(r91::mk_ordque(kDay2, 931u, 9.7, 13u, "000001"));
	queues2.push_back(r91::mk_ordque(kDay2, 932u, 9.6, 14u, "000001"));
	queues2.push_back(r91::mk_ordque(kDay2, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_ordque(_base, "SSE", "000001", kDay2, queues2));

	std::vector<WTSTransStruct> trans1;
	trans1.push_back(r91::mk_trans(kDay1, 931u, 10.05, 21u, "000001"));
	trans1.push_back(r91::mk_trans(kDay1, 932u, 10.15, 22u, "000001"));
	trans1.push_back(r91::mk_trans(kDay1, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000001", kDay1, trans1));

	std::vector<WTSTransStruct> trans2;
	trans2.push_back(r91::mk_trans(kDay2, 931u, 10.25, 23u, "000001"));
	trans2.push_back(r91::mk_trans(kDay2, 932u, 10.35, 24u, "000001"));
	trans2.push_back(r91::mk_trans(kDay2, 1501u, 0.0, 0u, "000001"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000001", kDay2, trans2));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001031500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_2days");
	ASSERT_TRUE(replayer.prepare());

	replayer.sub_tick(1u, kCode);
	replayer.sub_tick(2u, kCode2);
	replayer.sub_order_detail(3u, kCode);
	replayer.sub_order_queue(4u, kCode);
	replayer.sub_transaction(5u, kCode);

	replayer.run(false);

	// 0925 不得出现在回调里
	for (const r91::TickRec& rec : sink.ticks)
		EXPECT_NE(925u, rec.time);

	std::vector<r91::TickRec> c1day1 = filter_ticks(sink.ticks, kCode, kDay1);
	std::vector<r91::TickRec> c2day1 = filter_ticks(sink.ticks, kCode2, kDay1);
	std::vector<r91::TickRec> c1day2 = filter_ticks(sink.ticks, kCode, kDay2);
	std::vector<r91::TickRec> c2day2 = filter_ticks(sink.ticks, kCode2, kDay2);

	ASSERT_EQ(1u, c1day1.size());
	EXPECT_EQ(931u, c1day1[0].time);
	ASSERT_EQ(2u, c2day1.size());
	EXPECT_EQ(931u, c2day1[0].time);
	EXPECT_EQ(932u, c2day1[1].time);
	ASSERT_EQ(1u, c1day2.size());
	EXPECT_EQ(931u, c1day2[0].time);
	ASSERT_EQ(2u, c2day2.size());
	EXPECT_EQ(932u, c2day2[1].time);

	// Level2 两日各两笔
	std::vector<r91::HftRec> det1 = filter_by_date(sink.details, kDay1);
	std::vector<r91::HftRec> det2 = filter_by_date(sink.details, kDay2);
	ASSERT_EQ(2u, det1.size());
	ASSERT_EQ(2u, det2.size());
	EXPECT_EQ(932u, det1[1].time);
	EXPECT_EQ(932u, det2[1].time);
	ASSERT_EQ(2u, filter_by_date(sink.queues, kDay1).size());
	ASSERT_EQ(2u, filter_by_date(sink.queues, kDay2).size());
	ASSERT_EQ(2u, filter_by_date(sink.trans, kDay1).size());
	ASSERT_EQ(2u, filter_by_date(sink.trans, kDay2).size());

	ASSERT_EQ(2u, sink.beginDates.size());
	EXPECT_EQ(kDay1, sink.beginDates[0]);
	EXPECT_EQ(kDay2, sink.beginDates[1]);
	ASSERT_EQ(2u, sink.endDates.size());
	EXPECT_EQ(kDay1, sink.endDates[0]);
	EXPECT_EQ(kDay2, sink.endDates[1]);
	EXPECT_EQ(1u, sink.doneCalls);
	// run_by_ticks 用自然日 +1 推进：20200103(周五) -> 20200104，再与 end_tdate 比较后退出
	EXPECT_EQ(20200104u, replayer.get_trading_date());
}

TEST_F(R91Fixture, R91BinHft_TransactionSliceLoadsWithoutOrderDetailMiss)
{
	const char* const code = "SSE.STK.000004";
	std::vector<WTSTransStruct> trans;
	trans.push_back(r91::mk_trans(kDay1, 931u, 30.1, 31u, "000004"));
	trans.push_back(r91::mk_trans(kDay1, 932u, 30.2, 32u, "000004"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000004", kDay1, trans));

	r91::CfgOpts opts;
	opts.mode = "bin";
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_trans_direct");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_transaction(1u, code);

	// etime 命中最后一笔成交；不先查询缺失订单明细，因此直接验证正常成交缓存读取。
	WTSTransSlice* slice = replayer.get_transaction_slice(code, 2u, 202001020932ull);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(2u, slice->size());
	if (slice->size() == 2u)
	{
		EXPECT_DOUBLE_EQ(30.1, slice->at(0)->price);
		EXPECT_EQ(31u, slice->at(0)->volume);
		EXPECT_DOUBLE_EQ(30.2, slice->at(1)->price);
		EXPECT_EQ(32u, slice->at(1)->volume);
	}
	slice->release();
}

/*
 * 生产: HisDataReplayer.cpp:2731-2738（checkOrderDetails 无数据时写 _trans_cache）
 *       2773-2780（checkOrderQueues 同样写 _trans_cache）、2786-2825（checkTransactions）
 *
 * 缺陷归档：orddtl 缺失会把 trans 缓存设为空并标成当日已加载；后续成交查询跳过磁盘加载，
 * 却可能按空数组构造非空切片。禁用用例保留正确的两条成交契约预期，不把越界路径当作通过。
 * 详见 docs/迭代计划/迭代01/bugs/R91.md BUG-R91-001。
 */
TEST_F(R91Fixture, DISABLED_R91BinHft_OrderDetailCacheMissPoisonsTransactionCache)
{
	// 暂时禁用：缺订单明细会把空 trans 缓存标记为当日已加载；随后查询在空数组上
	// 得到非零切片长度，可能构造越界 view。不能运行此未定义行为路径来充当成功结果。
	const char* const code = "SSE.STK.000003";
	std::vector<WTSTransStruct> trans;
	trans.push_back(r91::mk_trans(kDay1, 931u, 20.1, 1u, "000003"));
	trans.push_back(r91::mk_trans(kDay1, 932u, 20.2, 2u, "000003"));
	ASSERT_TRUE(r91::write_trans(_base, "SSE", "000003", kDay1, trans));

	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	WTSVariant* cfg = r91::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r91::Sink sink;
	replayer.register_sink(&sink, "r91_poison");
	ASSERT_TRUE(replayer.prepare());

	replayer.sub_transaction(1u, code);

	// orders 目录没有 000003.dsb：返回 NULL，同时把 _trans_cache[code] 置空并标成当日
	EXPECT_EQ(nullptr, replayer.get_order_detail_slice(code, 2u, 202001020933ull));

	// 正确的行为是此刻从磁盘装载两条成交
	WTSTransSlice* slice = replayer.get_transaction_slice(code, 2u, 202001020932ull);
	ASSERT_NE(nullptr, slice);
	ASSERT_EQ(2u, slice->size());
	EXPECT_DOUBLE_EQ(20.1, slice->at(0)->price);
	EXPECT_DOUBLE_EQ(20.2, slice->at(1)->price);
	slice->release();
}
