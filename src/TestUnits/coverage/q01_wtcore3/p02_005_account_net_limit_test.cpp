/**
 * p02_005_account_net_limit_test.cpp —— BUG-P02-005（迭代03，2026-11-04，exec-units 包）
 *
 * 卡8补充裁定（2026-11-03）：账户上限按每个实际合约的账户净持仓绝对值分别限制；
 * 同账户下的执行器共享该合约的额度判断，不把不同合约手数相加；与组限额共同生效，
 * 任一超限即拒绝。账户身份只读使用 WP1 的 TraderAdapter::accountGate()
 * （同一 account_id 共享，未配置时为单 adapter 私有）。
 *
 * 观测：account_net_limits::Registry 只保存已提交批次的目标（交易数量，已乘 scale）；
 * 被拒批次不写入执行器目标账本，也不改变已发布值。工厂不加载任何插件，
 * 单元创建在宿主侧预检失败（不进入插件代码），不发任何订单。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include <Includes/IDataManager.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/AccountNetPositionLimits.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtLocalExecuter.h>

using namespace wtp;

namespace {

const char* const kAg = "SHFE.ag.2512";
const char* const kAu = "SHFE.au.2512";

class P025Stub : public IExecuterStub
{
public:
    P025Stub()
    {
        comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "P025", "CNY");
        comm->setTradingMode(TM_Both);
    }
    ~P025Stub() { comm->release(); }
    uint64_t get_real_time() override { return 20250110093000000ULL; }
    WTSCommodityInfo* get_comm_info(const char*) override { return comm; }
    WTSSessionInfo* get_sess_info(const char*) override { return NULL; }
    IHotMgr* get_hot_mon() override { return NULL; }
    uint32_t get_trading_day() override { return 20250110; }
    WTSCommodityInfo* comm = NULL;
};

class P025DataMgr : public IDataManager
{
};

WTSVariant* cfgOf(const std::string& json)
{
    return WTSCfgLoader::load_from_content(json.c_str());
}

std::string execCfg(const char* extra)
{
    return std::string("{\"scale\":1.0,\"poolsize\":0,") + extra +
        "\"policy\":{\"default\":{\"name\":\"P025NotLoaded.P025Unit\"}}}";
}

/// Local 宿主析构前须完成协调 quiesce（卡9；见 R71/R110 守卫）
bool quiesceLocal(WtLocalExecuter& local)
{
    WtExecuterMgr manager;
    manager.add_executer(ExecCmdPtr(&local, [](IExecCommand*) {}));
    WtExecQuiesceCoordinator coordinator({ &manager });
    return coordinator.quiesce_all() && coordinator.is_complete() && manager.wait_quiesced();
}

class P025LocalScope
{
public:
    explicit P025LocalScope(WtLocalExecuter& local) : _local(local) {}
    P025LocalScope(const P025LocalScope&) = delete;
    P025LocalScope& operator=(const P025LocalScope&) = delete;
    ~P025LocalScope()
    {
        if (!quiesceLocal(_local))
        {
            ADD_FAILURE() << "local executer quiesce did not complete";
            std::abort();
        }
    }

private:
    WtLocalExecuter& _local;
};

double ownerTarget(const void* owner, const char* code)
{
    return account_net_limits::Registry::instance().ownerTarget(owner, code);
}

double accountNet(TraderAdapter& trader, const char* code)
{
    return account_net_limits::Registry::instance().accountNet(trader.accountGate().get(), code);
}

wt_hashmap<std::string, double> targets(std::initializer_list<std::pair<const std::string, double>> items)
{
    wt_hashmap<std::string, double> result;
    for (const auto& item : items)
        result[item.first] = item.second;
    return result;
}

class P025AccountNetLimit : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _dir = std::filesystem::temp_directory_path() /
            ("p025_account_net_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
        std::filesystem::create_directories(_dir, ec);
        WtHelper::setGenerateDir((_dir.string() + "/").c_str());
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    void initLocal(WtLocalExecuter& exe, const char* extra)
    {
        WTSVariant* cfg = cfgOf(execCfg(extra));
        ASSERT_NE(cfg, nullptr);
        exe.setStub(&_stub);
        ASSERT_TRUE(exe.init(cfg));
        cfg->release();
    }

    template <typename Executer>
    void initPlain(Executer& exe, const char* extra)
    {
        WTSVariant* cfg = cfgOf(execCfg(extra));
        ASSERT_NE(cfg, nullptr);
        exe.setStub(&_stub);
        ASSERT_TRUE(exe.init(cfg));
        cfg->release();
    }

    std::filesystem::path _dir;
    P025Stub _stub;
    P025DataMgr _dm;
    WtExecuterFactory _factory;
};

} // namespace

// 同账户两个执行器共享同一合约额度：3+3 超过 5 被拒，3+2 恰好等于 5 通过；
// 被拒批次不改变已发布目标，增量调整同样受限。
TEST_F(P025AccountNetLimit, executers_on_one_account_share_the_contract_limit)
{
    TraderAdapter trader;
    WtLocalExecuter a(&_factory, "p025_share_a", &_dm);
    WtLocalExecuter b(&_factory, "p025_share_b", &_dm);
    P025LocalScope qa(a), qb(b);
    initLocal(a, "\"account_limits\":{\"SHFE.ag.2512\":5},");
    initLocal(b, "\"account_limits\":{\"SHFE.ag.2512\":5},");
    a.setTrader(&trader);
    b.setTrader(&trader);

    a.set_position(targets({ { kAg, 3.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 3.0);
    b.set_position(targets({ { kAg, 3.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&b, kAg), 0.0) << "3 + 3 exceeds the shared limit 5";
    EXPECT_DOUBLE_EQ(accountNet(trader, kAg), 3.0);
    b.set_position(targets({ { kAg, 2.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&b, kAg), 2.0);
    EXPECT_DOUBLE_EQ(accountNet(trader, kAg), 5.0);

    a.on_position_changed(kAg, 1.0); // incremental 3 -> 4 would make the account 6
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 3.0);
    a.on_position_changed(kAg, -1.0); // reduction is always admissible
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 2.0);
    EXPECT_DOUBLE_EQ(accountNet(trader, kAg), 4.0);
}

// 不把不同合约手数相加：default 5 分别作用于 ag 与 au，各 5 手同时通过；净额按符号相加。
TEST_F(P025AccountNetLimit, different_contracts_are_limited_separately_and_net_by_sign)
{
    TraderAdapter trader;
    WtLocalExecuter a(&_factory, "p025_sep_a", &_dm);
    WtLocalExecuter b(&_factory, "p025_sep_b", &_dm);
    P025LocalScope qa(a), qb(b);
    initLocal(a, "\"account_limits\":{\"default\":5},");
    initLocal(b, "\"account_limits\":{\"default\":5},");
    a.setTrader(&trader);
    b.setTrader(&trader);

    a.set_position(targets({ { kAg, 5.0 }, { kAu, 5.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 5.0);
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAu), 5.0) << "10 lots across two contracts are not summed";

    b.set_position(targets({ { kAg, -4.0 } })); // opposite sign nets the account to 1
    EXPECT_DOUBLE_EQ(ownerTarget(&b, kAg), -4.0);
    EXPECT_DOUBLE_EQ(accountNet(trader, kAg), 1.0);
    b.set_position(targets({ { kAg, -11.0 } })); // account would be -6
    EXPECT_DOUBLE_EQ(ownerTarget(&b, kAg), -4.0);
}

// 组限额与账户净额同时生效，任一超限拒绝。
TEST_F(P025AccountNetLimit, group_limit_and_account_limit_both_apply)
{
    TraderAdapter trader, other1, other2;
    WtLocalExecuter acctTight(&_factory, "p025_grp_acct", &_dm);
    WtLocalExecuter groupTight(&_factory, "p025_grp_group", &_dm);
    WtLocalExecuter both(&_factory, "p025_grp_ok", &_dm);
    P025LocalScope q1(acctTight), q2(groupTight), q3(both);
    initLocal(acctTight, "\"groups\":{\"G\":{\"SHFE.ag.2512\":1,\"SHFE.au.2512\":1}},"
        "\"group_limits\":{\"G\":10},\"account_limits\":{\"SHFE.ag.2512\":2},");
    initLocal(groupTight, "\"groups\":{\"G\":{\"SHFE.ag.2512\":1,\"SHFE.au.2512\":1}},"
        "\"group_limits\":{\"G\":1},\"account_limits\":{\"SHFE.ag.2512\":10},");
    initLocal(both, "\"groups\":{\"G\":{\"SHFE.ag.2512\":1,\"SHFE.au.2512\":1}},"
        "\"group_limits\":{\"G\":2},\"account_limits\":{\"SHFE.ag.2512\":2},");
    acctTight.setTrader(&trader);
    groupTight.setTrader(&other1);
    both.setTrader(&other2);

    acctTight.set_position(targets({ { "G", 3.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&acctTight, kAg), 0.0) << "group 3 <= 10 but ag 3 > account 2";
    groupTight.set_position(targets({ { "G", 3.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&groupTight, kAg), 0.0) << "account 3 <= 10 but group 3 > 1";
    both.set_position(targets({ { "G", 2.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&both, kAg), 2.0);
    EXPECT_DOUBLE_EQ(ownerTarget(&both, kAu), 2.0);
}

// 同一合约多个声明取最严；已超限账户允许减仓、拒绝加仓。
TEST_F(P025AccountNetLimit, strictest_declaration_wins_and_reduction_is_allowed)
{
    TraderAdapter trader;
    WtLocalExecuter a(&_factory, "p025_strict_a", &_dm);
    WtLocalExecuter c(&_factory, "p025_strict_c", &_dm);
    P025LocalScope qa(a), qc(c);
    initLocal(a, "\"account_limits\":{\"SHFE.ag\":5},");
    initLocal(c, "\"account_limits\":{\"SHFE.ag.2512\":3},");
    a.setTrader(&trader);
    c.setTrader(&trader);

    a.set_position(targets({ { kAg, 5.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 5.0);
    c.set_position(targets({})); // C registers its stricter declaration
    a.set_position(targets({ { kAg, 4.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 4.0) << "a reduction toward the limit is accepted";
    a.set_position(targets({ { kAg, 5.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 4.0) << "an increase above the strictest limit 3 is rejected";
    a.set_position(targets({ { kAg, 3.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&a, kAg), 3.0);
}

// Local、Arbi、Diff 三入口在同一账户共享额度；Diff 目标已按 scale 换算。
TEST_F(P025AccountNetLimit, local_arbi_and_diff_share_one_account)
{
    TraderAdapter trader;
    WtLocalExecuter local(&_factory, "p025_mix_local", &_dm);
    P025LocalScope ql(local);
    WtArbiExecuter arbi(&_factory, "p025_mix_arbi", &_dm);
    WtDiffExecuter diff(&_factory, "p025_mix_diff", &_dm, NULL);
    initLocal(local, "\"account_limits\":{\"SHFE.ag.2512\":6},");
    initPlain(arbi, "\"account_limits\":{\"SHFE.ag.2512\":6},");
    {
        WTSVariant* cfg = cfgOf("{\"scale\":2.0,\"poolsize\":0,\"account_limits\":{\"SHFE.ag.2512\":6},"
            "\"policy\":{\"default\":{\"name\":\"P025NotLoaded.P025Unit\"}}}");
        ASSERT_NE(cfg, nullptr);
        diff.setStub(&_stub);
        ASSERT_TRUE(diff.init(cfg));
        cfg->release();
    }
    local.setTrader(&trader);
    arbi.setTrader(&trader);
    diff.setTrader(&trader);

    local.set_position(targets({ { kAg, 2.0 } }));
    arbi.set_position(targets({ { kAg, 2.0 } }));
    EXPECT_DOUBLE_EQ(accountNet(trader, kAg), 4.0);
    diff.on_position_changed(kAg, 2.0); // scale 2 -> 4 lots, account would be 8
    EXPECT_DOUBLE_EQ(ownerTarget(&diff, kAg), 0.0);
    diff.on_position_changed(kAg, 1.0); // 2 lots, account 6
    EXPECT_DOUBLE_EQ(ownerTarget(&diff, kAg), 2.0);
    EXPECT_DOUBLE_EQ(accountNet(trader, kAg), 6.0);
    arbi.set_position(targets({ { kAg, 3.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&arbi, kAg), 2.0);
}

// 账户身份：同一 account_id 的两个 adapter 共享额度；不同账户互不影响。
TEST_F(P025AccountNetLimit, account_identity_follows_the_shared_account_gate)
{
    TraderAdapter x1, x2, y;
    ASSERT_TRUE(x1.bindAccount("p025-shared", ""));
    ASSERT_TRUE(x2.bindAccount("p025-shared", ""));
    ASSERT_TRUE(y.bindAccount("p025-other", ""));
    ASSERT_EQ(x1.accountGate().get(), x2.accountGate().get());
    WtLocalExecuter a(&_factory, "p025_id_a", &_dm);
    WtLocalExecuter b(&_factory, "p025_id_b", &_dm);
    WtLocalExecuter c(&_factory, "p025_id_c", &_dm);
    P025LocalScope qa(a), qb(b), qc(c);
    initLocal(a, "\"account_limits\":{\"SHFE.ag.2512\":5},");
    initLocal(b, "\"account_limits\":{\"SHFE.ag.2512\":5},");
    initLocal(c, "\"account_limits\":{\"SHFE.ag.2512\":5},");
    a.setTrader(&x1);
    b.setTrader(&x2);
    c.setTrader(&y);

    a.set_position(targets({ { kAg, 4.0 } }));
    b.set_position(targets({ { kAg, 2.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&b, kAg), 0.0) << "two adapters of one account share the limit";
    c.set_position(targets({ { kAg, 5.0 } }));
    EXPECT_DOUBLE_EQ(ownerTarget(&c, kAg), 5.0) << "another account is independent";
}

// 卡8b=A：account_limits 在 init 前整体校验，非法配置不产生任何状态。
TEST_F(P025AccountNetLimit, invalid_account_limits_are_rejected_eagerly)
{
    std::string error;
    for (const char* bad : { "\"account_limits\":5,", "\"account_limits\":{\"SHFE.ag\":-1},",
             "\"account_limits\":{\"SHFE.ag\":\"x\"},", "\"account_limits\":{\"\":1}," })
    {
        WTSVariant* cfg = cfgOf(execCfg(bad));
        ASSERT_NE(cfg, nullptr) << bad;
        EXPECT_FALSE(WtLocalExecuter::validateConfig(cfg, error)) << bad;
        EXPECT_FALSE(WtArbiExecuter::validateConfig(cfg, error)) << bad;
        EXPECT_FALSE(WtDiffExecuter::validateConfig(cfg, error)) << bad;
        WtLocalExecuter local(&_factory, "p025_bad_local", &_dm);
        P025LocalScope ql(local);
        EXPECT_FALSE(local.init(cfg)) << bad;
        cfg->release();
    }
    WTSVariant* good = cfgOf(execCfg("\"account_limits\":{\"SHFE.ag\":0,\"default\":2.5},"));
    ASSERT_NE(good, nullptr);
    EXPECT_TRUE(WtLocalExecuter::validateConfig(good, error)) << error;
    good->release();
}
