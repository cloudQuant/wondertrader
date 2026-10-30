/**
 * r59_trader_misc_test.cpp — TraderAdapter 其余可达分支
 *
 * 覆盖 p11 未触及的入口：
 *   initExt 空 API / 通道 init 失败、无 API 时 run() 早退（:209-235, :404-417）;
 *   init() 落地 _cfg 后 handleEvent(WTE_Connect, 0) 用配置里的账号登录（:1405-1417）;
 *   四个回报数组传 NULL 时状态机仍推进并广播 on_channel_ready（:1514-1543,
 *   :1545-1602, :1604-1730, :1739-1816 的空指针分支）;
 *   onPushTrade 无合约信息时直接忽略（:2148-2152）;
 *   TraderAdapterMgr::addAdapter 的 NULL/空名护栏与就绪通道 refresh_funds（:2369-2422）。
 */
#include "r59_trader_fixture.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace r59x;
using wtp::ActionPolicyMgr;
using wtp::TraderAdapter;
using wtp::TraderAdapterMgr;

namespace {

namespace fs = std::filesystem;

class R59Misc : public R59TraderTest
{
protected:
    void verifyNetTrade(bool isBuy, double opposingNewPosition, double tradeVolume,
        double expectedLong, double expectedShort, const char* id)
    {
        const double expectedOpened = isBuy ? expectedLong : expectedShort;
        ActionPolicyMgr pm;
        std::string policy = std::string("{\"default\":{\"order\":[{\"action\":\"open\",\"limit_") +
            (isBuy ? "l" : "s") + "\":" + std::to_string(static_cast<uint32_t>(expectedOpened + 1.0)) + "}]}}";
        ASSERT_TRUE(loadPolicy(pm, (std::string("r59_net_") + id + ".json").c_str(), policy.c_str()));

        TraderAdapter trader;
        build(trader, &pm);
        if (isBuy)
            seedPos(trader, { makePos(ct_, WDT_SHORT, 0, 0, opposingNewPosition, opposingNewPosition) });
        else
            seedPos(trader, { makePos(ct_, WDT_LONG, 0, 0, opposingNewPosition, opposingNewPosition) });

        WTSTradeInfo* trade = makeTrade(ct_, WDT_LONG, WOT_OPEN,
            tradeVolume, 7000, id, "NETORDER", "");
        trade->setNetDirection(isBuy);
        trader.onPushTrade(trade);

        EXPECT_DOUBLE_EQ(expectedLong, trader.getPosition(kStdCode, false, 1));
        EXPECT_DOUBLE_EQ(expectedLong, trader.getPosition(kStdCode, true, 1));
        EXPECT_DOUBLE_EQ(-expectedShort, trader.getPosition(kStdCode, false, 2));
        EXPECT_DOUBLE_EQ(-expectedShort, trader.getPosition(kStdCode, true, 2));
        EXPECT_DOUBLE_EQ(expectedLong - expectedShort, trader.getPosition(kStdCode, false, 3));

        trader.enumPosition([&](const char* code, bool, double preVol, double preAvail,
            double newVol, double newAvail) {
            EXPECT_STREQ(kStdCode, code);
            EXPECT_GE(preVol, 0.0);
            EXPECT_GE(preAvail, 0.0);
            EXPECT_GE(newVol, 0.0);
            EXPECT_GE(newAvail, 0.0);
        });

        // Daily open-volume caps expose the recorded opposite-side open amount:
        // the remaining cap must be exactly one lot after this net trade.
        api2_.vols_.clear();
        const uint32_t before = api2_.n_insert_;
        OrderIDs ids = isBuy
            ? trader.buy(kStdCode, 7000.0, 10.0, 0, false)
            : trader.sell(kStdCode, 7000.0, 10.0, 0, false);
        ASSERT_EQ(1u, ids.size());
        EXPECT_EQ(before + 1, api2_.n_insert_);
        ASSERT_EQ(1u, api2_.vols_.size());
        EXPECT_DOUBLE_EQ(1.0, api2_.vols_[0]);
    }
};

class R59InstDirGuard
{
public:
	explicit R59InstDirGuard(const std::string& instDir)
		: _oldInstDir(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(instDir.c_str());
	}

	~R59InstDirGuard()
	{
		WtHelper::setInstDir(_oldInstDir.c_str());
	}

private:
	std::string _oldInstDir;
};

struct R59VariantDeleter
{
	void operator()(WTSVariant* variant) const
	{
		if (variant != nullptr)
			variant->release();
	}
};

// TraderAdapter.cpp:209-235, 404-417（initExt 空指针/api init 失败；无 api 的 run 早退）
TEST_F(R59Misc, init_ext_guards_and_run_without_api)
{
    TraderAdapter none;
    EXPECT_FALSE(none.initExt(id_.c_str(), nullptr, &bd_, nullptr));
    EXPECT_FALSE(none.run());

    p11fix::FakeTraderApi failing;
    failing.init_ret_ = false;
    TraderAdapter bad;
    EXPECT_FALSE(bad.initExt(id_.c_str(), &failing, &bd_, nullptr));
    EXPECT_EQ(1u, failing.n_init_);
}

// TraderAdapter.cpp:99-207, 1405-1417（成功加载 API 后 WTE_Connect 用配置账号登录）
TEST_F(R59Misc, connect_event_uses_cfg_credentials)
{
	const fs::path moduleRoot = tmp_ / "r59_credentials_module";
	const fs::path traderDir = moduleRoot / "traders";
	std::error_code ec;
	fs::create_directories(traderDir, ec);
	ASSERT_FALSE(ec);

	const fs::path pluginPath(R59_CREDENTIALS_PLUGIN_PATH);
	ASSERT_TRUE(fs::is_regular_file(pluginPath, ec)) << pluginPath.string();
	ASSERT_FALSE(ec);
	const fs::path installedPlugin = traderDir / pluginPath.filename();
	fs::copy_file(pluginPath, installedPlugin, fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	R59InstDirGuard instDir(moduleRoot.string() + fs::path::preferred_separator);
	const fs::path loginRecord = tmp_ / "r59_login_credentials.txt";
	fs::remove(loginRecord, ec);
	ASSERT_FALSE(ec);

	std::unique_ptr<WTSVariant, R59VariantDeleter> cfg(WTSVariant::createObject());
	ASSERT_NE(nullptr, cfg);
	ASSERT_TRUE(cfg->append("module", "R59CredentialsTrader"));
	ASSERT_TRUE(cfg->append("savedata", false));
	ASSERT_TRUE(cfg->append("user", "r59_user"));
	ASSERT_TRUE(cfg->append("pass", "r59_pass"));
	ASSERT_TRUE(cfg->append("r59_login_record", loginRecord.string().c_str()));

	TraderAdapter trader;
	ASSERT_TRUE(trader.init(id_.c_str(), cfg.get(), &bd_, nullptr));
	ASSERT_TRUE(trader.run());
	EXPECT_EQ(TraderAdapter::AS_LOGINING, trader.state());

	// The dynamically loaded API reports WTE_Connect from connect(); its login
	// method records the values it actually receives through the module boundary.
	std::ifstream recordIn(loginRecord, std::ios::binary);
	ASSERT_TRUE(recordIn.is_open());
	std::ostringstream record;
	record << recordIn.rdbuf();
	EXPECT_EQ("r59_user\nr59_pass\n", record.str());

	// A failed connection event must not make another login attempt.
	trader.handleEvent(WTE_Connect, -2);
	std::ifstream secondRead(loginRecord, std::ios::binary);
	ASSERT_TRUE(secondRead.is_open());
	std::ostringstream unchanged;
	unchanged << secondRead.rdbuf();
	EXPECT_EQ("r59_user\nr59_pass\n", unchanged.str());

	trader.release();
}

// TraderAdapter.cpp:1514-1602, 1604-1730, 1739-1816（四个回报数组为 NULL 时的状态机推进）
TEST_F(R59Misc, null_report_arrays_advance_state_machine)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_none.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);             // 已登录 AS_LOGINED，并触发一次查持仓
    EXPECT_EQ(1u, api2_.n_qpositions_);

    // 四个 NULL 回报：不写任何本地表，但状态机一步步推进到 AS_ALLREADY
    makeReady(trader);

    EXPECT_EQ(1u, api2_.n_qorders_);
    EXPECT_EQ(1u, api2_.n_qtrades_);
    EXPECT_EQ(1u, api2_.n_qaccount_);
    EXPECT_EQ(1u, sink_.n_ready_);
    EXPECT_TRUE(trader.isReady());
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 3));
    EXPECT_EQ(nullptr, trader.getOrders(kStdCode));
    EXPECT_TRUE(sink_.trades_.empty());
}

// TraderAdapter.cpp:2148-2152（onPushTrade 无合约信息直接返回）
TEST_F(R59Misc, push_trade_without_contract_is_ignored)
{
    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_trade.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));

    TraderAdapter trader;
    build(trader, &pm);

    // 没有合约信息的成交：在写任何表之前返回
    WTSTradeInfo* t = WTSTradeInfo::create(kRawCode, kExchg);
    t->setDirection(WDT_LONG);
    t->setOffsetType(WOT_OPEN);
    t->setVolume(2);
    t->setPrice(7000.0);
    trader.onPushTrade(t);

    EXPECT_TRUE(sink_.trades_.empty());
    EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
    EXPECT_DOUBLE_EQ(0.0, trader.getPosition(kStdCode, false, 3));
}

// TraderAdapter.cpp:2369-2422（addAdapter 的 NULL/空名护栏 + 就绪通道 refresh_funds）
TEST_F(R59Misc, manager_rejects_bad_channels_and_refreshes_ready_funds)
{
    TraderAdapterMgr mgr;
    TraderAdapterPtr nullAdapter;
    EXPECT_FALSE(mgr.addAdapter("r59m", nullAdapter));

    ActionPolicyMgr pm;
    ASSERT_TRUE(loadPolicy(pm, "r59_misc_mgr.json",
        R"json({"default":{"order":[{"action":"open"}]}})json"));
    TraderAdapterPtr ready(new TraderAdapter());
    build(*ready, &pm);
    makeReady(*ready);

    TraderAdapterPtr emptyName(new TraderAdapter());
    EXPECT_FALSE(mgr.addAdapter("", emptyName));

    ASSERT_TRUE(mgr.addAdapter("r59m", ready));
    EXPECT_EQ(1u, mgr.getAdapters().size());

    // 已就绪 -> refresh_funds 真的下发查资金
    uint32_t before = api2_.n_qaccount_;
    mgr.refresh_funds();
    EXPECT_EQ(before + 1, api2_.n_qaccount_);
}

// Net-mode fills close the opposing position first, then open only the remainder.
TEST_F(R59Misc, net_mode_sell_clamps_long_new_and_opens_only_remainder)
{
    verifyNetTrade(false, 2, 5, 0, 3, "NETSELL_LESS");
    verifyNetTrade(false, 5, 5, 0, 0, "NETSELL_EQUAL");
    verifyNetTrade(false, 7, 5, 2, 0, "NETSELL_GREATER");
}

TEST_F(R59Misc, net_mode_buy_clamps_short_new_and_opens_only_remainder)
{
    verifyNetTrade(true, 2, 5, 3, 0, "NETBUY_LESS");
    verifyNetTrade(true, 5, 5, 0, 0, "NETBUY_EQUAL");
    verifyNetTrade(true, 7, 5, 0, 2, "NETBUY_GREATER");
}

} // namespace
