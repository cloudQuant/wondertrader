/**
 * r210_btcore_residual_test.cpp  (lane R210 · wave29)
 *
 * 靶子：WtBtCore 四个文件的公开面残余行
 *   - HisDataReplayer.cpp 1181-1186 / 1221：正偏移会话（夜盘）分钟任务的
 *     换交易日分支与日内跨零点 `_cur_date` 步进；
 *   - HisDataReplayer.cpp 1930 / 1994：bar 回放中 `replayHftDatas` 遍历
 *     空委托明细/委托队列订阅时的 cursor 越界 continue；
 *   - HisDataReplayer.cpp 2255-2256：csv 行情块解压抛 std::exception 的 catch；
 *   - HisDataReplayer.cpp 80-85：未压缩 V2 块剥头分支（bKeepHead=false）；
 *   - HftMocker.cpp 561：`stra_cancel` 任务把未成交委托的 `_left` 清零；
 *   - CtaMocker 公开面：id/空仓/带 hook 的 replay_done 落盘（固化行为）。
 *
 * 安全口径：
 *   - 不启动线程、不 sleep、不读挂钟；不调用任何会 wait 的 hook 路径；
 *   - 只走公开 API；临时文件只落 r72 的系统临时目录，由夹具 TearDown 清理；
 *   - ExecMocker 的 94 行成功路径仍被 BUG-R200-001 阻塞（init 即 SIGSEGV），
 *     本轮不新增会崩溃的用例。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtDataStorage/DataDefine.h>

#include "r72_common.h"

namespace
{
	namespace fs = std::filesystem;

	const char* const kCu = "SHFE.cu.2406";
	const char* const kRb = "SHFE.rb.2405";

	using R210Fixture = r72::Fixture;

	// BlockHeader + 原始 WTSBarStruct，version=BLOCK_VERSION_RAW_V2。
	// proc_block_data 走 bCmped=false / bOldVer=false 分支：按 bKeepHead=false 剥头。
	bool write_uncompressed_bar_block(const fs::path& file, const std::vector<WTSBarStruct>& bars)
	{
		std::string content(sizeof(BlockHeader), 0);
		BlockHeader* head = (BlockHeader*)content.data();
		strcpy(head->_blk_flag, BLK_FLAG);
		head->_type = BT_HIS_Day;
		head->_version = BLOCK_VERSION_RAW_V2;
		content.append((const char*)bars.data(), sizeof(WTSBarStruct) * bars.size());
		return r72::write_text(file, content);
	}

	// BlockHeaderV2 + 8 字节非 zstd 负载：尺寸校验通过，解压时
	// ZSTD_getFrameContentSize 返回错误，resize 抛 std::length_error。
	bool write_corrupt_compressed_bar_block(const fs::path& file)
	{
		std::string content(sizeof(BlockHeaderV2), 0);
		BlockHeaderV2* head = (BlockHeaderV2*)content.data();
		strcpy(head->_blk_flag, BLK_FLAG);
		head->_type = BT_HIS_Day;
		head->_version = BLOCK_VERSION_CMP_V2;
		head->_size = 8;
		content.append(8, '\0');
		return r72::write_text(file, content);
	}
}	// namespace

// 生产: HisDataReplayer.cpp:1181-1186（offset>0 的换交易日分支）
//       1221（日内 minuteToTime 回绕时 _cur_date 前进一天）
TEST_F(R210Fixture, R210His_MinuteTaskNightSessionCrossesMidnight)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(
		_root, _base, 202001022100ull, 202001030230ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, nullptr));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r210_task_night");
	// FN0100：offset=300（正偏移，夜盘算下一天），小节 2100-0100 跨零点。
	rp.register_task(1, 0, 60, "min", "CHINA", "FN0100");
	ASSERT_TRUE(rp.prepare());

	rp.run(false);

	EXPECT_GE(sink.beginCalls, 1u);
	EXPECT_GE(sink.scheduleCalls, 1u);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:1930（orddtl 空订阅 cursor>count）
//       1994（ordque 空订阅 cursor>count）
// csv 模式下 L2 缓存无法建立，订阅只留下默认 HftDataList(cursor=UINT_MAX,count=0)；
// 主 K 线日线 + tick 数据使 run_by_bars 每根 bar 之间进入 replayHftDatas。
TEST_F(R210Fixture, R210His_BarReplaySkipsEmptyL2Lists)
{
	r72::Loader loader;
	loader.finalBars[kCu] = {
		r72::make_bar(20200102u, 150000u, 70000.0),
		r72::make_bar(20200103u, 150000u, 70010.0)
	};
	loader.ticks[std::string(kCu) + "#20200102"] = {
		r72::make_tick_struct(kCu, 20200102u, 93100000u, 70000.0, 69990.0, 70010.0),
		r72::make_tick_struct(kCu, 20200102u, 93200000u, 70010.0, 70000.0, 70020.0)
	};
	loader.ticks[std::string(kCu) + "#20200103"] = {
		r72::make_tick_struct(kCu, 20200103u, 93100000u, 70010.0, 70000.0, 70020.0),
		r72::make_tick_struct(kCu, 20200103u, 93200000u, 70020.0, 70010.0, 70030.0)
	};

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(
		_root, _base, 202001020930ull, 202001031500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r210_hft_empty_l2");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCu, "d", 2, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();

	// csv 模式无法读取 L2，两个订阅都只留下空缓存。
	rp.sub_order_detail(0, kRb);
	rp.sub_order_queue(0, kRb);

	rp.run(false);

	EXPECT_GT(sink.tickCalls, 0u);
	EXPECT_EQ(1u, sink.doneCalls);
}

// 生产: HisDataReplayer.cpp:2255-2256（csv 块处理抛 std::exception 的 catch）
TEST_F(R210Fixture, R210His_CorruptCompressedBarBlockIsCaught)
{
	const fs::path file = _base / "his" / "day" / "SHFE" / "cu2406.dsb";
	ASSERT_TRUE(write_corrupt_compressed_bar_block(file));

	r72::Loader loader;
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(
		_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r210_corrupt_bar");
	ASSERT_TRUE(rp.prepare());

	// loader 无该合约数据 -> 回落到 csv/dsb；解压异常被 catch 后返回 NULL。
	EXPECT_EQ(nullptr, rp.get_kline_slice(kCu, "d", 2, 1, true));
}

// 生产: HisDataReplayer.cpp:80-85
// 未压缩、非旧版 V2 块：bCmped=false、bOldVer=false，按 bKeepHead=false 剥头。
TEST_F(R210Fixture, R210His_UncompressedV2BarBlockHeaderStripped)
{
	std::vector<WTSBarStruct> bars = {
		r72::make_bar(20200102u, 150000u, 70000.0),
		r72::make_bar(20200103u, 150000u, 70010.0)
	};
	const fs::path file = _base / "his" / "day" / "SHFE" / "cu2406.dsb";
	ASSERT_TRUE(write_uncompressed_bar_block(file, bars));

	r72::Loader loader;
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(
		_root, _base, 202001020930ull, 202001031500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r210_raw_v2_bar");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* kline = rp.get_kline_slice(kCu, "d", 2, 1, true);
	ASSERT_NE(nullptr, kline);
	ASSERT_EQ(static_cast<std::size_t>(1), kline->get_block_counts());
	EXPECT_EQ(2u, kline->get_block_size(0));
	kline->release();
}

// 生产: HftMocker.cpp:561（stra_cancel 任务把 ordInfo->_left 清零）
// 卖价远离最新价，确保 procOrder 不会先成交；随后 handle_tick 排空 cancel 任务。
TEST_F(R210Fixture, R210Hft_CancelPendingOrderViaTick)
{
	r72::Loader loader;
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(
		_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r210_hft_cancel");
	ASSERT_TRUE(rp.prepare());

	HftMocker m(&rp, "r210_hft_cancel");
	OrderIDs ids = m.stra_buy(kRb, 90.0, 10.0, "r210");
	ASSERT_EQ(static_cast<std::size_t>(1), ids.size());
	EXPECT_DOUBLE_EQ(10.0, m.stra_get_undone(kRb));

	ASSERT_TRUE(m.stra_cancel(ids[0]));

	WTSTickData* tick = r72::make_tick(
		kRb, 20200102u, 93100000u, 100.0, 99.9, 100.1);
	m.handle_tick(kRb, tick, 0);
	tick->release();

	EXPECT_DOUBLE_EQ(0.0, m.stra_get_undone(kRb));
}

// CtaMocker 公开面固化：上下文 id、空仓查询、带 hook 的收尾落盘。
// CtaMocker.cpp 的 46 行缺口全部落在等待/私有分支，见 manifests/wave29/R210.md。
TEST_F(R210Fixture, R210Cta_PublicContextAndReplayDone)
{
	r72::Loader loader;
	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(
		_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	rp.register_sink(&sink, "r210_cta");
	ASSERT_TRUE(rp.prepare());

	CtaMocker m(&rp, "r210_cta");
	EXPECT_GT(m.id(), 0u);
	EXPECT_EQ(m.id(), m.id());
	EXPECT_DOUBLE_EQ(0.0, m.stra_get_position(kRb));
	EXPECT_FALSE(m.step_calc());

	// install_hook 后 _in_backtest 仍为 false，replay_done 只走通知日志，不会 wait。
	m.install_hook();
	m.handle_replay_done();

	EXPECT_TRUE(fs::exists(_out / "r210_cta" / "positions.csv"));
}
