/**
 * r202_003_risk_unknown_code_test.cpp —— BUG-R202-003 合同用例（卡8c=A，2026-11-04）
 *
 * 卡8c=A：风控开启时，未知代码（基础数据中解析不到实际合约）与排除集中的代码都拒绝下单；
 * 风控关闭时保持现状（允许）。公开查询 isTradeEnabled 与下单前门禁 checkOrderLimits
 * 必须给出同一结论。产品实现位于 TraderAdapter（WP1 所有），本用例不在 coverage lane
 * 常驻，由 exec-units 包的隔离快照脚本复制进 n01 lane 运行，用于产品修改前红控/修改后绿控。
 * 只使用本地 TraderMocker 模块，不连接任何柜台。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include <WtCore/TraderAdapter.h>
#include <WtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>
#include "../n02_wtcore_engines/r160_common.h"
#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

USING_NS_WTP;

namespace {

class R202003InstDir
{
public:
	explicit R202003InstDir(const std::string& path) : _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(path.c_str());
	}
	~R202003InstDir() { WtHelper::setInstDir(_old.c_str()); }

private:
	std::string _old;
};

WTSVariant* r202003Params(bool riskActive)
{
	WTSVariant* params = WTSVariant::createObject();
	params->append("module", "TraderMocker");
	params->append("mockerid", 920031u);
	params->append("span", 50u);
	WTSVariant* riskmon = WTSVariant::createObject();
	riskmon->append("active", riskActive);
	if (riskActive)
	{
		WTSVariant* policy = WTSVariant::createObject();
		WTSVariant* defaultRule = WTSVariant::createObject();
		defaultRule->append("order_total_limits", 1u);
		defaultRule->append("order_times_boundary", 0u);
		defaultRule->append("order_stat_timespan", 0u);
		defaultRule->append("cancel_total_limits", 0u);
		defaultRule->append("cancel_times_boundary", 0u);
		defaultRule->append("cancel_stat_timespan", 0u);
		policy->append("default", defaultRule, false);
		riskmon->append("policy", policy, false);
	}
	params->append("riskmon", riskmon, false);
	return params;
}

void expectSwitch(bool riskActive)
{
	const std::string moduleDir = r160::find_module_dir("TraderMocker");
	ASSERT_FALSE(moduleDir.empty()) << "libTraderMocker not found next to test binary";
	r160::TempDir tmp(riskActive ? "r202003_on" : "r202003_off");
	const std::string tradersDir = tmp.path("traders");
	std::error_code ec;
	std::filesystem::create_directories(tradersDir, ec);
	ASSERT_FALSE(ec);
	const std::string moduleName = DLLHelper::wrap_module("TraderMocker", "lib");
	std::filesystem::copy_file(std::filesystem::path(moduleDir) / moduleName,
		std::filesystem::path(tradersDir) / moduleName,
		std::filesystem::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();
	R202003InstDir instDir(tmp.path() + "/");
	r160::ScopedCwd cwd(tmp.path());
	ASSERT_TRUE(cwd.ok());

	p11fix::FakeBaseDataMgr bd;
	WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "R202003", "CNY");
	comm->setTradingMode(TM_Both);
	WTSContractInfo* ct = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
	bd.regis(comm, ct);

	WTSVariant* params = r202003Params(riskActive);
	{
		TraderAdapter adapter;
		ASSERT_TRUE(adapter.init(riskActive ? "r202003_on" : "r202003_off", params, &bd, nullptr));
		// Known contract: admitted in both modes.
		EXPECT_TRUE(adapter.isTradeEnabled("SHFE.ag.2512"));
		EXPECT_TRUE(adapter.checkOrderLimits("SHFE.ag.2512"));
		// Unknown contract: card 8c=A rejects it only while risk control is active.
		EXPECT_EQ(!riskActive, adapter.isTradeEnabled("SHFE.xx.9999"));
		EXPECT_EQ(!riskActive, adapter.checkOrderLimits("SHFE.xx.9999"));
	}
	params->release();
	ct->release();
	comm->release();
}

} // namespace

TEST(R202003RiskUnknownCode, RiskOffAdmitsKnownAndUnknownCodes)
{
	expectSwitch(false);
}

TEST(R202003RiskUnknownCode, RiskOnRejectsUnknownCodeAndKeepsKnownCode)
{
	expectSwitch(true);
}
