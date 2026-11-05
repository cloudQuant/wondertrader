/**
 * l06_replayer_runtime_test.cpp
 * 覆盖 src/WtBtCore/HisDataReplayer 中手续费分支之外的运行期接口:
 * 构造默认值 / init / get_session_info / update_price+get_cur_price /
 * get_day_price 空缓存路径 / get_rawcode / loadFees 容错 /
 * stop、clear_cache 的非运行态防护.
 * amount-based fee 缺 commodity 初始化保护及 by-volume 无 commodity 兼容.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
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
	return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path();
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

WTSVariant* build_replayer_cfg(const std::filesystem::path& workdir,
	const std::filesystem::path& fees, bool includeCommodity)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("mode", "csv");
	cfg->append("path", workdir.string().c_str());
	cfg->append("stime", static_cast<uint64_t>(202001020930));
	cfg->append("etime", static_cast<uint64_t>(202001081500));
	cfg->append("tick", false);
	cfg->append("adjust_flag", static_cast<uint32_t>(0));
	cfg->append("align_by_section", false);
	cfg->append("dont_simtick_if_notrade", false);
	cfg->append("fees", fees.string().c_str());

	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (lane_root() / "dist/common/sessions.json").string().c_str());
	if (includeCommodity)
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
		WTSVariant* cfg = build_replayer_cfg(_workdir, fee_fixture("fees_minfee.json"), true);
		ASSERT_TRUE(_replayer.init(cfg));
		cfg->release();
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_workdir, ec);
	}

	// Fee calculations now require a registered physical contract. Keep the
	// shared fixture setup unchanged for the unrelated runtime tests.
	bool initStockFeeReplayer(HisDataReplayer& replayer)
	{
		const auto path = _workdir / "stock_fee_contract.json";
		std::ofstream out(path);
		out << R"({"SSE":{"000001":{"name":"stock","exchg":"SSE","product":"STK","opendate":20100101,"expiredate":20301231,"volscale":2}}})";
		out.close();
		if (!out.good()) return false;
		WTSVariant* cfg = build_replayer_cfg(_workdir, fee_fixture("fees_minfee.json"), true);
		cfg->get("basefiles")->append("contract", path.string().c_str());
		const bool ok = replayer.init(cfg);
		cfg->release();
		return ok;
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

	// 未配置的品种没有 commodity/session 元数据。
	EXPECT_EQ(nullptr, _replayer.get_session_info("SSE.NOPE.999999", true));
}

TEST_F(HisDataReplayerRuntimeTest, rawcodeWithoutRuleIsEmpty)
{
	// 股票标准代码没有品种规则, hasRule()=false → 空串
	EXPECT_EQ(std::string(""), _replayer.get_rawcode("SSE.STK.000001"));
}

TEST_F(HisDataReplayerRuntimeTest, calcFeeForUnconfiguredCode)
{
	HisDataReplayer replayer;
	ASSERT_TRUE(initStockFeeReplayer(replayer));
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.000001", 1.0, 100.0, 0));
	// Missing contract, product or fee metadata remains an observable failure,
	// including a zero-quantity request; it is not an explicit zero fee.
	EXPECT_TRUE(std::isnan(replayer.calc_fee("SSE.NOPE.600000", 10.0, 100.0, 0)));
	EXPECT_TRUE(std::isnan(replayer.calc_fee("SZSE.STK.000002", 0.0, 0.0, 1)));
	wt_fee::Context context; context.product = "unchanged"; std::string error;
	EXPECT_FALSE(replayer.prepare_fee_context("SZSE.STK.000002", nullptr, context, error));
	EXPECT_EQ(context.product, "unchanged");
	EXPECT_FALSE(error.empty());
}

TEST_F(HisDataReplayerRuntimeTest, amountBasedFeeRequiresCommodityAtInit)
{
	WTSVariant* cfg = build_replayer_cfg(_workdir, fee_fixture("fees_minfee.json"), false);
	HisDataReplayer replayerWithoutCommodity;
	EXPECT_FALSE(replayerWithoutCommodity.init(cfg));
	cfg->release();
}

TEST_F(HisDataReplayerRuntimeTest, amountBasedFeeUsesPhysicalContractVolumeScale)
{
	HisDataReplayer replayer;
	ASSERT_TRUE(initStockFeeReplayer(replayer));
	// The physical contract overrides its commodity multiplier of one with two.
	wt_fee::Context context; std::string error;
	ASSERT_TRUE(replayer.prepare_fee_context("SSE.STK.000001", nullptr, context, error));
	EXPECT_EQ(context.volume_scale, 2.0);
	EXPECT_DOUBLE_EQ(20000.0, replayer.calc_fee("SSE.STK.000001", 100000.0, 100.0, 0));
}

TEST_F(HisDataReplayerRuntimeTest, volumeBasedFeeInitAllowsMissingCommodityButFillRequiresMetadata)
{
	const auto fees = _workdir / "fees_volume_only.json";
	std::ofstream out(fees);
	ASSERT_TRUE(out.is_open());
	out << R"({"DCE.a":{"open":1.25,"close":0.75,"closetoday":0.5,"byvolume":true}})";
	out.close();
	ASSERT_TRUE(out.good());

	WTSVariant* cfg = build_replayer_cfg(_workdir, fees, false);
	HisDataReplayer replayerWithoutCommodity;
	ASSERT_TRUE(replayerWithoutCommodity.init(cfg));
	cfg->release();
	wt_fee::Context missing; missing.product = "unchanged"; std::string error;
	EXPECT_FALSE(replayerWithoutCommodity.prepare_fee_context("DCE.a.2401", nullptr, missing, error));
	EXPECT_EQ(missing.product, "unchanged");
	EXPECT_TRUE(std::isnan(replayerWithoutCommodity.calc_fee("DCE.a.2401", 100.0, 2.0, 0)));
}

TEST_F(HisDataReplayerRuntimeTest, volumeBasedFeeUsesVerifiedPhysicalContract)
{
	const auto fees = _workdir / "fees_volume_only.json";
	std::ofstream out(fees);
	out << R"({"DCE.a":{"open":1.25,"close":0.75,"closetoday":0.5,"byvolume":true}})";
	out.close(); ASSERT_TRUE(out.good());
	const auto comms = _workdir / "fee_commodity.json";
	const auto contracts = _workdir / "fee_contract.json";
	std::ofstream commOut(comms);
	commOut << R"({"DCE":{"a":{"name":"soy","session":"SD0930","holiday":"CHINA","pricetick":1,"volscale":10}}})";
	commOut.close(); ASSERT_TRUE(commOut.good());
	std::ofstream contractOut(contracts);
	contractOut << R"({"DCE":{"a2401":{"name":"soy January","exchg":"DCE","product":"a","opendate":20100101,"expiredate":20301231,"volscale":10}}})";
	contractOut.close(); ASSERT_TRUE(contractOut.good());
	WTSVariant* cfg = build_replayer_cfg(_workdir, fees, false);
	cfg->get("basefiles")->append("commodity", comms.string().c_str());
	cfg->get("basefiles")->append("contract", contracts.string().c_str());
	HisDataReplayer verified;
	const bool initialized = verified.init(cfg); cfg->release(); ASSERT_TRUE(initialized);
	wt_fee::Context context; std::string error;
	ASSERT_TRUE(verified.prepare_fee_context("DCE.a.2401", nullptr, context, error));
	EXPECT_EQ(context.physical_contract, "DCE.a2401");
	EXPECT_EQ(context.volume_scale, 10.0);
	EXPECT_DOUBLE_EQ(2.5, verified.calc_fee("DCE.a.2401", 100.0, 2.0, 0));
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
