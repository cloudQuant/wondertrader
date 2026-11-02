/**
 * r81_hft_gap_test.cpp — lane R81 / HftStraBaseCtx 剩余行段
 *
 * 覆盖 src/WtCore/HftStraBaseCtx.cpp：
 *   stra_cancel/stra_buy/stra_sell 的风控拒绝分支（checkCancelLimits/checkOrderLimits 为 false）
 *   stra_get_position/stra_get_undone 的自定义规则映射分支、卖单合约缺失守卫
 *   on_trade 的脏用户数据落盘、do_set_position 的开仓/平仓/反手与空头盈亏取反
 *   load_userdata 的空文件早退
 *
 * 风控通过成功的 TraderAdapter::init(riskmon + 本地计数 fake trader plugin) 装配，
 * 只用进程内接口回报，不连接柜台、不启交易线程或网络 socket。
 */
#include "r81_env.h"

#include <memory>
#include <string>
#include <filesystem>
#include <system_error>

#include <Share/DLLHelper.hpp>
#include <WtCore/ActionPolicyMgr.h>
#include <WtCore/TraderAdapter.h>

using namespace wtp;

#ifndef WT_R81_RISK_FAKE_TRADER_PATH
#error "CoverageTests_wtcore must provide the R81 risk fake trader plugin"
#endif

namespace {

WTSVariant* makeRiskParams(uint32_t orderLimit, const std::string& moduleName)
{
    WTSVariant* params = WTSVariant::createObject();
    params->append("module", moduleName.c_str());
    params->append("savedata", false);

    WTSVariant* riskmon = WTSVariant::createObject();
    riskmon->append("active", true);

    WTSVariant* policyCfg = WTSVariant::createObject();
    WTSVariant* rule = WTSVariant::createObject();
    rule->append("cancel_total_limits", (uint32_t)0);
    rule->append("cancel_times_boundary", (uint32_t)0);
    rule->append("cancel_stat_timespan", (uint32_t)0);
    rule->append("order_total_limits", orderLimit);
    rule->append("order_times_boundary", (uint32_t)0);
    rule->append("order_stat_timespan", (uint32_t)0);
    policyCfg->append("default", rule);

    riskmon->append("policy", policyCfg);
    params->append("riskmon", riskmon);
    return params;
}

class ScopedR81RiskPluginEnvironment
{
public:
	ScopedR81RiskPluginEnvironment()
		: _old_cwd(std::filesystem::current_path())
		, _old_inst_dir(WtHelper::getInstDir())
	{
	}

	~ScopedR81RiskPluginEnvironment()
	{
		if (_inst_dir_changed)
			WtHelper::setInstDir(_old_inst_dir.c_str());
		if (_cwd_changed)
		{
			std::error_code ec;
			std::filesystem::current_path(_old_cwd, ec);
		}
	}

	bool prepare(const std::string& moduleName)
	{
		const std::filesystem::path cachedCwd(WtHelper::getCWD());
		std::error_code ec;
		if (!std::filesystem::equivalent(cachedCwd, _old_cwd, ec) || ec)
		{
			_error = "WtHelper CWD cache differs from the process CWD";
			if (ec)
				_error += ": " + ec.message();
			return false;
		}

		const std::filesystem::path source(WT_R81_RISK_FAKE_TRADER_PATH);
		if (!std::filesystem::is_regular_file(source))
		{
			_error = "R81 fake trader plugin missing: " + source.string();
			return false;
		}

		const std::string libraryName = DLLHelper::wrap_module(moduleName.c_str(), "lib");
		const std::filesystem::path cwdModule = cachedCwd / "traders" / libraryName;
		if (std::filesystem::exists(cwdModule))
		{
			_error = "CWD contains a shadowing R81 fake trader plugin: " + cwdModule.string();
			return false;
		}

		const std::filesystem::path root = std::filesystem::path(r81::temp_root()) / moduleName;
		const std::filesystem::path tradersDir = root / "traders";
		std::filesystem::create_directories(tradersDir, ec);
		if (ec)
		{
			_error = "cannot create temporary traders directory: " + ec.message();
			return false;
		}

		const std::string instDir = root.string() + "/";
		_plugin_path = std::filesystem::path(instDir + "traders/" + libraryName);
		std::filesystem::copy_file(source, _plugin_path,
			std::filesystem::copy_options::overwrite_existing, ec);
		if (ec || !std::filesystem::is_regular_file(_plugin_path))
		{
			_error = "cannot stage R81 fake trader plugin";
			if (ec)
				_error += ": " + ec.message();
			return false;
		}

		std::filesystem::current_path(root, ec);
		if (ec)
		{
			_error = "cannot switch to temporary R81 plugin CWD: " + ec.message();
			return false;
		}
		_cwd_changed = true;

		WtHelper::setInstDir(instDir.c_str());
		_inst_dir_changed = true;
		return true;
	}

	const std::filesystem::path& plugin_path() const { return _plugin_path; }
	const std::string& error() const { return _error; }

private:
	std::filesystem::path _old_cwd;
	std::string _old_inst_dir;
	std::filesystem::path _plugin_path;
	std::string _error;
	bool _cwd_changed = false;
	bool _inst_dir_changed = false;
};

class R81RiskFakePluginProbe
{
public:
	using Counter = uint32_t (*)();

	~R81RiskFakePluginProbe()
	{
		if (_module != nullptr)
			DLLHelper::free_library(_module);
	}

	bool load(const std::filesystem::path& path)
	{
		_module = DLLHelper::load_library(path.string().c_str());
		if (_module == nullptr)
			return false;
		_order_insert_count = (Counter)DLLHelper::get_symbol(_module, "r81RiskFakeOrderInsertCount");
		_order_action_count = (Counter)DLLHelper::get_symbol(_module, "r81RiskFakeOrderActionCount");
		return _order_insert_count != nullptr && _order_action_count != nullptr;
	}

	uint32_t order_insert_count() const { return _order_insert_count(); }
	uint32_t order_action_count() const { return _order_action_count(); }

private:
	DllHandle _module = nullptr;
	Counter _order_insert_count = nullptr;
	Counter _order_action_count = nullptr;
};

class ScopedR81TradingTime
{
public:
	ScopedR81TradingTime()
		: _date(WtHelper::getDate())
		, _time(WtHelper::getTime())
		, _secs(WtHelper::getSecs())
		, _trading_date(WtHelper::getTradingDate())
	{
		WtHelper::setTime(20250110, 931, 0);
		WtHelper::setTDate(20250110);
	}

	~ScopedR81TradingTime()
	{
		WtHelper::setTime(_date, _time, _secs);
		WtHelper::setTDate(_trading_date);
	}

private:
	uint32_t _date;
	uint32_t _time;
	uint32_t _secs;
	uint32_t _trading_date;
};

WTSOrderInfo* makeOrder(WTSContractInfo* cInfo, const char* orderid)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(cInfo->getCode());
    o->setExchange(cInfo->getExchg());
    o->setContractInfo(cInfo);
    o->setDirection(WDT_LONG);
    o->setOffsetType(WOT_OPEN);
    o->setVolume(1);
    o->setVolLeft(1);
    o->setEntrustID("R81-ENTRUST-1");
    o->setUserTag("otp.r81_hft_risk.77");
    o->setOrderState(WOS_NotTraded_Queuing);
    o->setOrderID(orderid);
    return o;
}

class R81HftTest : public ::testing::Test
{
protected:
    r81::R81GenEnv env;
    r81::R81BaseDataMgr bd;
    r81::R81HotMgr hot;
    r81::R81HftEngine engine;
    WTSContractInfo* agCt = NULL;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* ag = r81::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        agCt = r81::makeCt("SHFE", "ag", "ag2512");
        bd.regis(ag, agCt);

        WTSCommodityInfo* au = r81::makeComm("SHFE", "au", "TRADING", TM_Long);
        au->setSessionInfo(sess);
        bd.regis(au, r81::makeCt("SHFE", "au", "au2212"));

        engine.attach(&bd, &hot);
    }
};

// 覆盖 HftStraBaseCtx.cpp:188-200 stra_cancel 风控拒绝、211-259 stra_buy 两条拒绝、
// 262-323 stra_sell 两条拒绝
TEST_F(R81HftTest, RiskRejectsCancelBuyAndSellBranches)
{
    [[maybe_unused]] ScopedR81TradingTime tradingTime;
    std::string policyFile = r81::temp_root() + "r81_policy.json";
    ASSERT_TRUE(r81::write_file(policyFile, R"json({"default":{"order":[{"action":"open"}]}})json"));
    ActionPolicyMgr policy;
    ASSERT_TRUE(policy.init(policyFile.c_str()));

    const std::string moduleName = "R81RiskFakeTrader_" + std::to_string(::getpid());
    ScopedR81RiskPluginEnvironment pluginEnv;
    ASSERT_TRUE(pluginEnv.prepare(moduleName)) << pluginEnv.error();
    R81RiskFakePluginProbe apiProbe;
    ASSERT_TRUE(apiProbe.load(pluginEnv.plugin_path()));

    TraderAdapter trader;
	WTSVariant* params = makeRiskParams(1, moduleName);
	ASSERT_NE(params, nullptr);
	const bool initialized = trader.init("r81_hft_risk", params, &bd, &policy);
    params->release();
	ASSERT_TRUE(initialized);
    ASSERT_TRUE(trader.run());

    // 进程内推送一笔活单：total_orders 达到限额 1，且供撤单拒绝分支使用。
    WTSOrderInfo* order = makeOrder(agCt, "R81-ORDER-1");
    trader.onPushOrder(order);
    order->release();

    r81::R81HftCtx ctx(&engine, "r81_hft_risk", false, 0);
    ctx.setTrader(&trader);
    ctx.on_init();

    // 非规则代码：checkOrderLimits 命中总量超限并拉黑
    EXPECT_TRUE(ctx.stra_buy("SHFE.ag.2512", 3500.0, 1.0, "b1").empty());
    // 自定义规则代码：映射到 realCode 后同样被拒
    EXPECT_TRUE(ctx.stra_buy("SHFE.ag.HOT", 3500.0, 1.0, "b2").empty());
    // 卖单两条分支
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.2512", 3500.0, 1.0, "s1").empty());
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.HOT", 3500.0, 1.0, "s2").empty());
    // 已拉黑 -> checkCancelLimits 直接 false
    EXPECT_TRUE(ctx.stra_cancel("SHFE.ag.2512", true, 0.0).empty());
    EXPECT_EQ(apiProbe.order_insert_count(), 0u);
    EXPECT_EQ(apiProbe.order_action_count(), 0u);
}

// 覆盖 HftStraBaseCtx.cpp:631-685 规则/非规则仓量查询、262-310 卖单合约缺失
TEST_F(R81HftTest, RuleCodeQueriesAndMissingContractSell)
{
    TraderAdapter trader; // 未 init/run：getPosition/getUndoneQty 对空表安全
    r81::R81HftCtx ctx(&engine, "r81_hft_query", false, 0);
    ctx.setTrader(&trader);
    ctx.on_init();

    // 规则代码 -> getCustomRawCode 映射为 ag2512 -> SHFE.ag.2512
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.HOT"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_undone("SHFE.ag.HOT"), 0.0);
    EXPECT_STREQ(ctx.get_inner_code("SHFE.ag.2512"), "SHFE.ag.HOT");

    // 非规则代码走 else 直通
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_undone("SHFE.ag.2512"), 0.0);

    // 品种存在、合约不存在 -> 卖单在 getContract 处早退（买同路径已有覆盖）
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.9999", 3500.0, 1.0, "miss").empty());
    EXPECT_TRUE(ctx.stra_buy("SHFE.ag.9999", 3500.0, 1.0, "missb").empty());
}

// 覆盖 HftStraBaseCtx.cpp:536-553 on_trade 脏数据落盘、754-783 空 userdata 早退、
// 785-904 do_set_position 开/平/反手与空头盈亏
TEST_F(R81HftTest, UserDataEmptyFileAndTradeAccounting)
{
    const char* name = "r81_hft_trade";
    ASSERT_TRUE(r81::write_file(r81::user_data_file(name), ""));

    r81::R81HftCtx ctx(&engine, name, false, 0);
    ctx.on_init();
    EXPECT_STREQ(ctx.stra_load_user_data("missing", "def"), "def");

    // 脏标记 -> on_trade 开头落盘
    ctx.stra_save_user_data("k1", "v1");
    ctx.on_trade(1, "SHFE.ag.2512", true, 2.0, 3500.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 3500.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit("SHFE.ag.2512"), 0.0);

    // 多头部分平仓、全平
    ctx.on_trade(2, "SHFE.ag.2512", false, 1.0, 3600.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 3500.0);
    ctx.on_trade(3, "SHFE.ag.2512", false, 1.0, 3400.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 0.0);

    // 空仓反手做空，再买入平仓：覆盖 !_long 的盈亏取反
    ctx.on_trade(4, "SHFE.ag.2512", false, 2.0, 3500.0);
    ctx.on_trade(5, "SHFE.ag.2512", true, 1.0, 3400.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"),
        -3500.0); // 负数均价为既有口径（q02 已登记）
    ctx.on_trade(6, "SHFE.ag.2512", true, 1.0, 3300.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 0.0);

    const std::string ud = r81::read_file(r81::user_data_file(name));
    EXPECT_NE(ud.find("\"k1\""), std::string::npos);

    r81::R81HftCtx reloaded(&engine, name, false, 0);
    reloaded.on_init();
    EXPECT_STREQ(reloaded.stra_load_user_data("k1"), "v1");
}

} // namespace
