/**
 * r141_replayer_block_dsb_test.cpp  (lane R141 · wave22)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 的块解码装配
 *   - 老版本 tick 块（BLOCK_VERSION_RAW）：WTSTickStructOld -> WTSTickStruct
 *     （L109-123），经 cacheRawTicksFromCSV 的 bin/ticks dsb 路径读入；
 *   - 新版压缩 bar 块（BLOCK_VERSION_CMP_V2）：长度校验 + 解压（L64-75）；
 *   - 老版本 bar 块（BLOCK_VERSION_RAW）：WTSBarStructOld -> WTSBarStruct
 *     （L92-108）。
 *
 * 只走公开 API：init/register_sink/prepare/get_tick_slice/get_kline_slice。
 * dsb 文件写入系统临时目录，TearDown 清理；不联网、不起线程。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <Includes/WTSContractInfo.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

#include "r141_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
	const char* const kCu = "SHFE.cu.2406";
}

class R141ReplayerBlockDsb : public ::testing::Test
{
protected:
	HisDataReplayer _rp;
	r141::NullSink _sink;
	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	void SetUp() override
	{
		_root = r141::repo_root();
		ASSERT_FALSE(_root.empty());
		_base = r141::temp_dir("block_dsb");
		_out = r141::temp_dir("block_out");
		WtHelper::setOutputDir(_out.string().c_str());

		// 老版本 tick dsb：<base>/bin/ticks/<stdCode>_tick_<date>.dsb
		ASSERT_TRUE(r141::write_text(
			_base / "bin" / "ticks" /
				(std::string(kRb) + "_tick_20200102.dsb"),
			r141::make_old_tick_block(2, 3800.0)));

		// 新版压缩日线 dsb：<base>/his/day/SHFE/2405.dsb
		ASSERT_TRUE(r141::write_text(
			_base / "his" / "day" / "SHFE" / "2405.dsb",
			r141::make_cmp_bar_block(3, 3800.0)));

		// 老版本日线 dsb：<base>/his/day/SHFE/2406.dsb
		ASSERT_TRUE(r141::write_text(
			_base / "his" / "day" / "SHFE" / "2406.dsb",
			r141::make_old_bar_block(3, 70000.0)));

		WTSVariant* cfg = r141::make_cfg(_root, _base,
			202001020932ull, 202001031500ull, true);
		ASSERT_TRUE(_rp.init(cfg));
		cfg->release();

		_rp.register_sink(&_sink, "r141_block");
		ASSERT_TRUE(_rp.prepare());
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}
};

// 生产: HisDataReplayer.cpp:64-75（压缩块长度校验/解压）、92-123（老结构体升版）
TEST_F(R141ReplayerBlockDsb, OldTickAndBothBarVersions)
{
	ASSERT_NE(nullptr, _rp.get_commodity_info(kRb));
	ASSERT_NE(nullptr, _rp.get_commodity_info(kCu));

	// 老 tick 块：两笔 tick 都转成新版，09:32 的游标取到最后一笔
	WTSTickSlice* ticks = _rp.get_tick_slice(kRb, 10);
	ASSERT_NE(nullptr, ticks);
	ASSERT_EQ(1u, ticks->size());
	EXPECT_DOUBLE_EQ(3801.0, ticks->at(0)->price);
	EXPECT_EQ(20200102u, ticks->at(0)->action_date);
	ticks->release();

	// 新版压缩日线：3 根全部解压出来，当前交易日取第一根
	WTSKlineSlice* rb = _rp.get_kline_slice(kRb, "d", 10, 1, false);
	ASSERT_NE(nullptr, rb);
	ASSERT_EQ(1, rb->size());
	EXPECT_DOUBLE_EQ(3800.25, rb->at(0)->close);
	rb->release();

	// 老版本日线：3 根 WTSBarStructOld 升版，当前交易日取第一根
	WTSKlineSlice* cu = _rp.get_kline_slice(kCu, "d", 10, 1, false);
	ASSERT_NE(nullptr, cu);
	ASSERT_EQ(1, cu->size());
	EXPECT_DOUBLE_EQ(70000.5, cu->at(0)->close);
	cu->release();
}
