/**
 * r90_executer_pool_test.cpp — lane R90 / wave17
 *
 * 目标：src/WtCore/WtLocalExecuter.cpp 与 WtArbiExecuter.cpp 的线程池转发分支。
 * 复用 n01 R71 的进程内柜台/数据替身与真实 WtExeFact 执行单元（只读）。
 *
 * 覆盖行段：
 *   Local : set_position 组包/禁用/池调度/过期清零/strict_sync:537-547, 573-586, 608-618, 636-651
 *           on_tick, on_trade, on_order, on_entrust, channel, on_account::674-683, 709-717,
 *           744-752, 776-785, 810-821, 843-854, 886-894, 977-991
 *   Arbi  : set_position 组包/禁用/池调度/过期清零/strict_sync:431-440, 462-474, 492-502, 518-533
 *           on_tick, on_trade, on_order, on_entrust, channel, on_account::555-562, 622-628,
 *           655-661, 686-693, 717-722, 747-752, 788-794, 875-888
 */
#include "../n01_wtcore_tradepaths/r71_wtcore_env.h"
#include "r90_env.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtLocalExecuter.h>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.ag.2512";

class R90ExecPool : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71PluginDir plug;
    r71::R71TraderEnv traderEnv;
    r71::R71ExecStub stub;
    r71::R71DataMgr dm;
    WtExecuterFactory factory;
    TraderAdapter trader;

    WTSCommodityInfo* comm = NULL;
    WTSSessionInfo* sess = NULL;

    void SetUp() override
    {
        ASSERT_TRUE(plug.ok()) << "缺少 libWtExeFact";
        ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));

        traderEnv.bd.regisSession("TRADING");
        sess = traderEnv.bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
        comm->setSessionInfo(sess);
        WTSContractInfo* agct = r71::makeCt("SHFE", "ag", "ag2512");
        traderEnv.bd.regis(comm, agct);
        traderEnv.bd.regisAlias("SHFE.ag.2512", agct);

        WTSCommodityInfo* rb = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        traderEnv.bd.regis(rb, r71::makeCt("SHFE", "rb", "rb2510"));

        WTSCommodityInfo* au = r71::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        traderEnv.bd.regis(au, r71::makeCt("SHFE", "au", "au2212"));

        WTSCommodityInfo* cu = r71::makeComm("SHFE", "cu", "TRADING", TM_Both);
        cu->setSessionInfo(sess);
        traderEnv.bd.regis(cu, r71::makeCt("SHFE", "cu", "cu2510"));

        stub.comm_ = comm;
        stub.sess_ = sess;
        stub.hot_ = &traderEnv.hot;

        ASSERT_TRUE(traderEnv.build(trader, "r90_pool"));
    }

    WTSVariant* makeCfg()
    {
        const char* json =
            "{\"scale\":1.0,\"poolsize\":1,\"strict_sync\":true,"
            "\"clear\":{\"active\":true,\"includes\":\"SHFE.ag\"},"
            "\"groups\":{\"R90G\":{\"SHFE.rb.2510\":2.0,\"SHFE.au.2212\":2.0}},"
            "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\","
            "\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
            "\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
        return WTSCfgLoader::load_from_content(json);
    }

    WTSTickData* tick(const char* code, double price, uint32_t atime)
    {
        return r71::makeTick("SHFE", code, 20250110, 20250110, atime, price);
    }
};

//////////////////////////////////////////////////////////////////
// Local：池化 set_position/tick/回报/通道/资金/自动清理
// src/WtCore/WtLocalExecuter.cpp:537-547, 573-586, 608-618, 636-651,
// 674-683, 709-717, 744-752, 776-785, 810-821, 843-854, 886-894, 977-991
//////////////////////////////////////////////////////////////////
TEST_F(R90ExecPool, LocalPoolForwardsEverything)
{
    const uint32_t base = traderEnv.api.n_insert;

    {
        WtLocalExecuter local(&factory, "r90_local_pool", &dm);
        local.setStub(&stub);
        WTSVariant* cfg = makeCfg();
        ASSERT_NE(nullptr, cfg);
        ASSERT_TRUE(local.init(cfg));
        cfg->release();
        local.setTrader(&trader);

        // 单元创建（通道未就绪，创建时不回调）
        local.on_position_changed(kCode, 2.0);

        // 组合匹配 + 池调度 set_position
        wt_hashmap<std::string, double> targets;
        targets["SHFE.rb.2510"] = 3.0;
        targets["SHFE.au.2212"] = 3.0;
        local.set_position(targets);

        // strict_sync：通道持仓不在目标里 -> 池调度清零
        local.on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);

        // 过期目标清零（池调度，同时覆盖 strict_sync 的 cu）
        wt_hashmap<std::string, double> empty;
        local.set_position(empty);

        WTSTickData* t = tick(kCode, 3500.0, 93100000);
        ASSERT_NE(nullptr, t);
        local.on_tick(kCode, t);
        t->release();

        local.on_trade(1, kCode, true, 1, 3500.0);
        local.on_order(1, kCode, true, 1, 0, 3500.0, false);
        local.on_entrust(1, kCode, true, "ok");
        local.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
        local.on_channel_lost();
        local.on_channel_ready();

        // includes 命中 + 上一期主力匹配 -> 池调度 clear_all_position
        traderEnv.hot.prev_ = "ag2512";
        local.on_position(kCode, true, 1, 1, 1, 1, 20250110);
    }

    // 池析构会 wait；tick 触发的最小冲击单元至少发出一笔委托
    EXPECT_GT(traderEnv.api.n_insert, base);
}

//////////////////////////////////////////////////////////////////
// Arbi：池化 set_position/tick/回报/通道/资金/自动清理
// src/WtCore/WtArbiExecuter.cpp:431-440, 462-474, 492-502, 518-533,
// 555-562, 622-628, 655-661, 686-693, 717-722, 747-752, 788-794, 875-888
//////////////////////////////////////////////////////////////////
TEST_F(R90ExecPool, ArbiPoolForwardsEverything)
{
    const uint32_t base = traderEnv.api.n_insert;

    {
        WtArbiExecuter arbi(&factory, "r90_arbi_pool", &dm);
        arbi.setStub(&stub);
        WTSVariant* cfg = makeCfg();
        ASSERT_NE(nullptr, cfg);
        ASSERT_TRUE(arbi.init(cfg));
        cfg->release();
        arbi.setTrader(&trader);

        arbi.on_position_changed(kCode, 2.0);

        wt_hashmap<std::string, double> targets;
        targets["SHFE.rb.2510"] = 3.0;
        targets["SHFE.au.2212"] = 3.0;
        arbi.set_position(targets);

        arbi.on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);

        wt_hashmap<std::string, double> empty;
        arbi.set_position(empty);

        WTSTickData* t = tick(kCode, 3500.0, 93100000);
        ASSERT_NE(nullptr, t);
        arbi.on_tick(kCode, t);
        t->release();

        arbi.on_trade(1, kCode, true, 1, 3500.0);
        arbi.on_order(1, kCode, true, 1, 0, 3500.0, false);
        arbi.on_entrust(1, kCode, true, "ok");
        arbi.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
        arbi.on_channel_lost();
        arbi.on_channel_ready();

        traderEnv.hot.prev_ = "ag2512";
        arbi.on_position(kCode, true, 1, 1, 1, 1, 20250110);
    }

    EXPECT_GT(traderEnv.api.n_insert, base);
}

}  // namespace
