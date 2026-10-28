/**
 * r161_btcore_edges_test.cpp  (lane R161 · wave24)
 *
 * 靶子：WtBtCore 四个 Mocker/回放器的残余未覆盖行（只走公开 API）。
 * 每个用例注释标明生产文件:行段，独立、确定性、无长线程/墙钟。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/UftMocker.h>

#include "r161_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk1 = "SSE.STK.000001";
	const char* const kFut1 = "DCE.i.2409";
	const char* const kFut2 = "DCE.j.2409";
}

using R161ReplayFixture = r161::ReplayFixture;

// 生产: HisDataReplayer.cpp:2554/2602/2650（csv 模式拒绝 level2 查询）
//       HisDataReplayer.cpp:2475-2476（无 tick 文件时 get_tick_slice 返回空）
TEST_F(R161ReplayFixture, R161ReplayEdges_Level2QueriesRejectedInCsvMode)
{
	// tick=true 下 rb 有 dsb，正常路径非空
	WTSTickSlice* ticks = _rp.get_tick_slice(kRb, 10);
	ASSERT_NE(nullptr, ticks);
	EXPECT_EQ(1u, ticks->size());
	ticks->release();

	// cu 没有 tick dsb，checkTicks 失败后返回空
	EXPECT_EQ(nullptr, _rp.get_tick_slice(kCu, 10));
	EXPECT_EQ(nullptr, _rp.get_last_tick(kCu));

	// csv 模式没有逐笔委托/委托队列/逐笔成交，三条查询都在 check 处返回空
	EXPECT_EQ(nullptr, _rp.get_order_detail_slice(kCu, 10));
	EXPECT_EQ(nullptr, _rp.get_order_queue_slice(kCu, 10));
	EXPECT_EQ(nullptr, _rp.get_transaction_slice(kCu, 10));
}

// 生产: HisDataReplayer.cpp:64-72（压缩块长度与块头 _size 不一致时拒绝）
TEST_F(R161ReplayFixture, R161ReplayEdges_MalformedTickBlockSizeMismatch)
{
	// 长度恰好是 1 个 WTSTickStruct，避免失败路径之后的 memcpy 越过 vector 尾
	std::string content(sizeof(WTSTickStruct), '\0');
	BlockHeaderV2* hdr = (BlockHeaderV2*)&content[0];
	memset(hdr, 0, sizeof(*hdr));
	memcpy(hdr->_blk_flag, BLK_FLAG, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Ticks;
	hdr->_version = BLOCK_VERSION_CMP_V2;
	hdr->_size = 1;	// 与 content.size()-sizeof(BlockHeaderV2) 不符

	ASSERT_TRUE(r141::write_text(
		_base / "bin" / "ticks" / (std::string(kCu) + "_tick_20200102.dsb"),
		content));

	// proc_block_data 报长度校验失败；缓存里只有一条零值 tick，游标定位后返回空
	EXPECT_EQ(nullptr, _rp.get_tick_slice(kCu, 10));
}

// 生产: HisDataReplayer.cpp:64-72（bar 分支 isBar=true）
//       HisDataReplayer.cpp:2410-2412（解压失败留下的脏 bar 无法定位时返回空）
TEST_F(R161ReplayFixture, R161ReplayEdges_MalformedBarBlockSizeMismatch)
{
	// 长度取 2 根 WTSBarStruct，保证失败路径 memcpy 不越界
	std::string content(sizeof(WTSBarStruct) * 2, '\0');
	BlockHeaderV2* hdr = (BlockHeaderV2*)&content[0];
	memset(hdr, 0, sizeof(*hdr));
	memcpy(hdr->_blk_flag, BLK_FLAG, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Day;
	hdr->_version = BLOCK_VERSION_CMP_V2;
	hdr->_size = 3;	// 故意与 content.size()-sizeof(BlockHeaderV2) 不符

	ASSERT_TRUE(r141::write_text(
		_base / "his" / "day" / "SHFE" / "2406.dsb", content));

	// 块头被当成第一根 bar 的 date（远大于当前交易日），日线定位落到 end
	EXPECT_EQ(nullptr, _rp.get_kline_slice(kCu, "d", 10));
}

// 生产: UftMocker.cpp:709-711（非平今模式下平多时释放今仓可用）
//       UftMocker.cpp:770-772（非平今模式下平空时释放今仓可用）
TEST_F(R161ReplayFixture, R161UftCover_PartialExitReleasesNewAvail)
{
	UftMocker mocker(&_rp, "r161_uft");

	// 大商所期货 covermode=CM_OpenCover 且非 T+1：当日开多后 newavail=10
	OrderIDs buys = mocker.stra_buy(kFut1, 0.0, 10);
	ASSERT_EQ(1u, buys.size());
	WTSTickData* t1 = r141::make_tick(kFut1, 20200102u, 93130000u, 801.0, 800.0, 802.0, 50, 50);
	mocker.handle_tick(kFut1, t1, 0);
	t1->release();
	ASSERT_DOUBLE_EQ(10.0, mocker.stra_get_position(kFut1));

	// 平 4 手：preavail=0，走 newavail -= qty 分支
	OrderIDs sells = mocker.stra_sell(kFut1, 0.0, 4);
	ASSERT_EQ(1u, sells.size());
	WTSTickData* t2 = r141::make_tick(kFut1, 20200102u, 93130000u, 801.0, 800.0, 802.0, 50, 50);
	mocker.handle_tick(kFut1, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(6.0, mocker.stra_get_position(kFut1));

	// 另一只期货做空后再平空，覆盖 stra_exit_short 的同一分支
	OrderIDs shorts = mocker.stra_sell(kFut2, 0.0, 10);
	ASSERT_EQ(1u, shorts.size());
	WTSTickData* t3 = r141::make_tick(kFut2, 20200102u, 93130000u, 901.0, 900.0, 902.0, 50, 50);
	mocker.handle_tick(kFut2, t3, 0);
	t3->release();
	ASSERT_DOUBLE_EQ(-10.0, mocker.stra_get_position(kFut2));

	OrderIDs covers = mocker.stra_buy(kFut2, 0.0, 4);
	ASSERT_EQ(1u, covers.size());
	WTSTickData* t4 = r141::make_tick(kFut2, 20200102u, 93130000u, 901.0, 900.0, 902.0, 50, 50);
	mocker.handle_tick(kFut2, t4, 0);
	t4->release();
	EXPECT_DOUBLE_EQ(-6.0, mocker.stra_get_position(kFut2));
}

// 生产: CtaMocker.cpp:1636-1651（同向加仓的比率滑点分支）
TEST_F(R161ReplayFixture, R161CtaSlippage_RatioSlippageOnSameDirectionAdd)
{
	CtaMocker cta(&_rp, "r161_cta_ratio", 10, true, NULL, true);

	WTSTickData* t1 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	cta.handle_tick(kStk1, t1, 0);
	t1->release();

	// 0 -> 1：平/反手分支的滑点 10bp = 0.01
	cta.stra_set_position(kStk1, 1);
	WTSTickData* t2 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	cta.handle_tick(kStk1, t2, 0);
	t2->release();
	ASSERT_DOUBLE_EQ(1.0, cta.stra_get_position(kStk1));

	// 1 -> 2：持仓与变化同向，进入 1636 的同向加仓分支
	cta.stra_set_position(kStk1, 2);
	WTSTickData* t3 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	cta.handle_tick(kStk1, t3, 0);
	t3->release();
	EXPECT_DOUBLE_EQ(2.0, cta.stra_get_position(kStk1));

	cta.handle_replay_done();
	const std::string trades = r161::read_text(_out / "r161_cta_ratio" / "trades.csv");
	EXPECT_NE(std::string::npos, trades.find("10.01"));
}

// 生产: CtaMocker.cpp:1649-1651（同向加仓的绝对滑点 else 分支）
TEST_F(R161ReplayFixture, R161CtaSlippage_AbsoluteSlippageOnSameDirectionAdd)
{
	CtaMocker cta(&_rp, "r161_cta_abs", 5, true, NULL, false);

	WTSTickData* t1 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	cta.handle_tick(kStk1, t1, 0);
	t1->release();

	cta.stra_set_position(kStk1, 1);
	WTSTickData* t2 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	cta.handle_tick(kStk1, t2, 0);
	t2->release();
	ASSERT_DOUBLE_EQ(1.0, cta.stra_get_position(kStk1));

	cta.stra_set_position(kStk1, 2);
	WTSTickData* t3 = r141::make_tick(kStk1, 20200102u, 93200000u, 10.0, 9.99, 10.01);
	cta.handle_tick(kStk1, t3, 0);
	t3->release();
	EXPECT_DOUBLE_EQ(2.0, cta.stra_get_position(kStk1));

	cta.handle_replay_done();
	const std::string trades = r161::read_text(_out / "r161_cta_abs" / "trades.csv");
	EXPECT_NE(std::string::npos, trades.find("10.05"));
}

namespace
{
	// 用公开虚函数 on_calculate 在调度窗口内回调图表接口，不触碰私有成员
	class ChartCta : public CtaMocker
	{
	public:
		using CtaMocker::CtaMocker;

		int calc_calls { 0 };
		bool idx_ok { false };
		bool bad_index { true };
		bool bad_line { true };

		void on_calculate(uint32_t, uint32_t) override
		{
			calc_calls++;
			register_index("idx", 1);
			register_index_line("idx", "line", 2);
			add_chart_mark(3810.0, "buy", "tag");
			idx_ok = set_index_value("idx", "line", 42.0);
			bad_index = set_index_value("nope", "line", 1.0);
			bad_line = set_index_value("idx", "nope", 1.0);
		}
	};
}

// 生产: CtaMocker.cpp:2157-2161（add_chart_mark 成功写 marks）
//       CtaMocker.cpp:2208-2227（set_index_value 的成功/未知 index/未知 line）
TEST_F(R161ReplayFixture, R161CtaChart_MarkAndIndexInsideSchedule)
{
	ChartCta cta(&_rp, "r161_chart", 0, true, NULL, false);

	// 先建立主 K 线 tag，并把 tag 标记为已收盘，on_schedule 才会触发 on_calculate
	WTSKlineSlice* kline = cta.stra_get_bars(kRb, "d", 10, true);
	ASSERT_NE(nullptr, kline);
	kline->release();

	WTSBarStruct bar = r141::make_bar(20200102u, 1000u, 3800.0);
	cta.handle_bar_close(kRb, "d", 1, &bar);

	ASSERT_TRUE(cta.on_schedule(20200102u, 1000u));
	EXPECT_EQ(1, cta.calc_calls);
	EXPECT_TRUE(cta.idx_ok);
	EXPECT_FALSE(cta.bad_index);
	EXPECT_FALSE(cta.bad_line);

	cta.handle_replay_done();

	const std::string chart = r161::read_text(_out / "r161_chart" / "btchart.json");
	EXPECT_NE(std::string::npos, chart.find("\"idx\""));
	const std::string marks = r161::read_text(_out / "r161_chart" / "marks.csv");
	EXPECT_NE(std::string::npos, marks.find("3810,buy,tag"));
	const std::string indice = r161::read_text(_out / "r161_chart" / "indice.csv");
	EXPECT_NE(std::string::npos, indice.find(",idx,line,42"));
}

// 生产: CtaMocker.cpp:1813-1816（复权代码取 K 线后剥掉后缀再订阅 tick）
TEST_F(R161ReplayFixture, R161CtaExright_StripsSuffixAfterBarsLoaded)
{
	// 复权代码在 csv 模式下走 <exchg>.<code><suffix>.dsb 文件名
	ASSERT_TRUE(r141::write_text(
		_base / "his" / "day" / "SSE" / "SSE.000001+.dsb",
		r141::make_cmp_bar_block(5, 10.0)));

	CtaMocker cta(&_rp, "r161_exright", 0, false, NULL, false);
	WTSKlineSlice* kline = cta.stra_get_bars("SSE.STK.000001+", "d", 10, true);
	ASSERT_NE(nullptr, kline);
	EXPECT_EQ(1, kline->size());
	EXPECT_DOUBLE_EQ(10.25, kline->at(0)->close);
	kline->release();
}

// BUG-R161-001（待审核）：cacheRawTicksFromCSV 忽略 proc_block_data 的 false 返回值，
// 随后按 content.size()/sizeof(WTSTickStruct) 扩容、再按 content.size() memcpy
// （HisDataReplayer.cpp:3495-3499）。content 长度不是结构体整数倍时会越界写。
// 触发即 heap corruption，故禁用；保留为缺陷复现占位，不放宽断言。
TEST_F(R161ReplayFixture, DISABLED_MalformedTickBlockTailBeyondLastItem)
{
	std::string content(sizeof(WTSTickStruct) + 3, '\0');
	BlockHeaderV2* hdr = (BlockHeaderV2*)&content[0];
	memset(hdr, 0, sizeof(*hdr));
	memcpy(hdr->_blk_flag, BLK_FLAG, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Ticks;
	hdr->_version = BLOCK_VERSION_CMP_V2;
	hdr->_size = 1;

	ASSERT_TRUE(r141::write_text(
		_base / "bin" / "ticks" / (std::string(kCu) + "_tick_20200102.dsb"),
		content));

	EXPECT_EQ(nullptr, _rp.get_tick_slice(kCu, 10));
}
