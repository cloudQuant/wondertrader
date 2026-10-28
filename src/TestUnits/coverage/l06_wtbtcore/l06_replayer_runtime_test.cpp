/**
 * l06_replayer_runtime_test.cpp
 * 覆盖 src/WtBtCore/HisDataReplayer 中手续费分支之外的运行期接口:
 * 构造默认值 / init / get_session_info / update_price+get_cur_price /
 * get_day_price 空缓存路径 / get_rawcode / loadFees 容错 /
 * stop、clear_cache 的非运行态防护.
 * 手续费/minfee 分支已由 src/TestUnits/test_replayer_fee.cpp 覆盖, 此处不重复.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>

namespace
{
std::filesystem::path lane_root()
{
	// src/TestUnits/coverage/l06_wtbtcore
	return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

std::filesystem::path fee_fixture(const char* name)
{
	return lane_root() / "src/TestUnits/fixtures" / name;
}

std::filesystem::path make_temp_dir()
{
	auto path = std::filesystem::temp_directory_path() /
		("l06_replayer_" + std::to_string(::getpid()) + "_" +
			std::to_string(std::hash<std::string>{}(__FILE__)));
	std::error_code ec;
	std::filesystem::create_directories(path, ec);
	return path;
}

WTSVariant* build_replayer_cfg(const std::filesystem::path& workdir)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("mode", "csv");
	cfg->append("path", workdir.string().c_str());
	cfg->append("stime", static_cast<uint64_t>(0));
	cfg->append("etime", static_cast<uint64_t>(0));
	cfg->append("tick", false);
	cfg->append("adjust_flag", static_cast<uint32_t>(0));
	cfg->append("align_by_section", false);
	cfg->append("dont_simtick_if_notrade", false);
	cfg->append("fees", fee_fixture("fees_minfee.json").string().c_str());

	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (lane_root() / "dist/common/sessions.json").string().c_str());
	basefiles->append("commodity", (lane_root() / "dist/common/stk_comms.json").string().c_str());
	cfg->append("basefiles", basefiles);
	basefiles->release();
	return cfg;
}
}

class HisDataReplayerRuntimeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_workdir = make_temp_dir();
		ASSERT_TRUE(std::filesystem::exists(_workdir));
		WTSVariant* cfg = build_replayer_cfg(_workdir);
		ASSERT_TRUE(_replayer.init(cfg));
		cfg->release();
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_workdir, ec);
	}

	HisDataReplayer _replayer;
	std::filesystem::path _workdir;
};

TEST_F(HisDataReplayerRuntimeTest, defaultsAfterInit)
{
	// cfg.tick=false 覆盖构造函数的 tick 默认值
	EXPECT_FALSE(_replayer.is_tick_enabled());
	EXPECT_EQ(0u, _replayer.get_date());
	EXPECT_EQ(0u, _replayer.get_min_time());
	EXPECT_EQ(0u, _replayer.get_raw_time());
	EXPECT_EQ(0u, _replayer.get_secs());
	EXPECT_EQ(0u, _replayer.get_trading_date());
	EXPECT_NE(nullptr, _replayer.get_hot_mgr());
}

TEST_F(HisDataReplayerRuntimeTest, priceMapRoundTrip)
{
	EXPECT_DOUBLE_EQ(0.0, _replayer.get_cur_price("SSE.STK.000001"));
	_replayer.update_price("SSE.STK.000001", 10.5);
	EXPECT_DOUBLE_EQ(10.5, _replayer.get_cur_price("SSE.STK.000001"));
	_replayer.update_price("SSE.STK.000001", 11.25);
	EXPECT_DOUBLE_EQ(11.25, _replayer.get_cur_price("SSE.STK.000001"));
	// 未知合约仍返回 0
	EXPECT_DOUBLE_EQ(0.0, _replayer.get_cur_price("SSE.STK.999999"));
}

TEST_F(HisDataReplayerRuntimeTest, dayPriceWithEmptyCache)
{
	// tick 未启用 + _day_cache 为空 → 全部档位返回 0.0
	for (int flag = 0; flag <= 4; flag++)
		EXPECT_DOUBLE_EQ(0.0, _replayer.get_day_price("SSE.STK.000001", flag));
	// 未知合约同样 0
	EXPECT_DOUBLE_EQ(0.0, _replayer.get_day_price("DCE.NOPE", 0));
}

TEST_F(HisDataReplayerRuntimeTest, sessionInfoLookup)
{
	// sessions.json 里明确定义的时段
	WTSSessionInfo* stk = _replayer.get_session_info("SD0930");
	// WTSBaseDataMgr 未加载该 sid 时也不应崩溃, 两次调用结果需一致
	EXPECT_EQ(stk, _replayer.get_session_info("SD0930", false));

	// 按合约代码取时段: 同一代码两次结果一致
	WTSSessionInfo* byCode = _replayer.get_session_info("SSE.STK.000001", true);
	EXPECT_EQ(byCode, _replayer.get_session_info("SSE.STK.000001", true));

	// 不存在的合约 → commodity 缺失 → NULL
	EXPECT_EQ(nullptr, _replayer.get_session_info("SSE.STK.999999", true));
}

TEST_F(HisDataReplayerRuntimeTest, rawcodeWithoutRuleIsEmpty)
{
	// 股票标准代码没有品种规则, hasRule()=false → 空串
	EXPECT_EQ(std::string(""), _replayer.get_rawcode("SSE.STK.000001"));
}

TEST_F(HisDataReplayerRuntimeTest, calcFeeForUnconfiguredCode)
{
	// 已配置品种走 minfee/费率分支 (test_replayer_fee 已覆盖, 此处仅做回归锚点)
	EXPECT_DOUBLE_EQ(5.0, _replayer.calc_fee("SSE.STK.000001", 1.0, 100.0, 0));
	// 未配置费率的品种: _fee_map miss → 0
	EXPECT_DOUBLE_EQ(0.0, _replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 0));
	// 数量为 0 且费率缺失时不应抛异常
	EXPECT_DOUBLE_EQ(0.0, _replayer.calc_fee("SZSE.STK.000002", 0.0, 0.0, 1));
}

TEST_F(HisDataReplayerRuntimeTest, stopAndClearCacheAreSafeWhenIdle)
{
	// 未运行时 stop 仅告警 (L638 分支)
	_replayer.stop();
	// 空缓存清理不崩溃
	_replayer.clear_cache();
	EXPECT_FALSE(_replayer.is_tick_enabled());
}

TEST_F(HisDataReplayerRuntimeTest, lastTickMissingReturnsNull)
{
	EXPECT_EQ(nullptr, _replayer.get_last_tick("SSE.STK.000001"));
}
