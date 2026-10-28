/**
 * r110_diff_executer_test.cpp — lane R110 / wave19
 *
 * 目标：src/WtCore/WtDiffExecuter.cpp（线程池分支与通道就绪后的委托转发）+
 *       WtLocalExecuter / WtArbiExecuter 的非池分支残余。
 *
 * 覆盖行段：
 *   Diff  : buy/sell/cancel 成功转发 :410-431；on_position_changed 的 0 增量与池调度
 *           :501-544；set_position 的组合匹配/等值跳过/池调度/目标移除 :560-638；
 *           on_tick/on_trade/on_order/on_entrust 池调度 :650-761；
 *           on_channel_ready/lost/on_account 池调度 :773-877
 *   Local : 组合匹配 :537-547；非池 set_position/清零 :565-593, 619-622；
 *           on_channel_ready/lost 非池 :817-821, 850-854；自动清理非池 :975-991
 *   Arbi  : cancel 成功转发 :323-344；组合匹配 :432-440；非池 set_position
 *           :457-468, 503-506；通道回调非池；自动清理非池 :878-887
 *
 * 执行单元用构建产物 libWtExeFact（WtMinImpactExeUnit），只做内存撮合，不连柜台；
 * 池任务在析构 wait() 前用短暂 sleep 让回调完成。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "../n01_wtcore_tradepaths/r71_wtcore_env.h"
#include "r90_env.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtLocalExecuter.h>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.ag.2512";

class R110ExecEnv : public ::testing::Test
{
protected:
    r71::R71GenEnv    env;
    r71::R71PluginDir plug;
    r71::R71TraderEnv traderEnv;
    r71::R71ExecStub  stub;
    r71::R71DataMgr   dm;
    WtExecuterFactory factory;
    TraderAdapter     trader;

    WTSCommodityInfo* comm = NULL;
    WTSSessionInfo*   sess = NULL;

    void SetUp() override
    {
        ASSERT_TRUE(plug.ok()) << "缺少 libWtExeFact";
        ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));

        traderEnv.bd.regisSession("TRADING");
        sess = traderEnv.bd.session("TRADING");
        ASSERT_NE(nullptr, sess);
        sess->addTradingSection(900, 1500);

        comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
        comm->setSessionInfo(sess);
        traderEnv.bd.regis(comm, r71::makeCt("SHFE", "ag", "ag2512"));

        WTSCommodityInfo* rb = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        traderEnv.bd.regis(rb, r71::makeCt("SHFE", "rb", "rb2510"));

        WTSCommodityInfo* au = r71::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        traderEnv.bd.regis(au, r71::makeCt("SHFE", "au", "au2212"));

        stub.comm_ = comm;
        stub.sess_ = sess;
        stub.hot_ = &traderEnv.hot;

        ASSERT_TRUE(traderEnv.build(trader, "r110_exec"));
    }

    WTSVariant* cfg(uint32_t poolsize)
    {
        const std::string json = "{\"scale\":1.0,\"poolsize\":" + std::to_string(poolsize)
            + ",\"clear\":{\"active\":true,\"includes\":\"SHFE.ag\"},"
            "\"groups\":{\"R110G\":{\"SHFE.rb.2510\":2.0,\"SHFE.au.2212\":2.0}},"
            "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\","
            "\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
            "\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }

    WTSTickData* tick(const char* code, double price, uint32_t atime)
    {
        return r71::makeTick("SHFE", code, 20250110, 20250110, atime, price);
    }
};

//////////////////////////////////////////////////////////////////
// Diff：池化委托/回报/通道/资金转发 + 目标仓位组合匹配
// src/WtCore/WtDiffExecuter.cpp:410-431, 501-544, 560-638, 650-761, 773-877
//////////////////////////////////////////////////////////////////
TEST_F(R110ExecEnv, DiffPoolForwardsEverything)
{
    WtDiffExecuter diff(&factory, "r110_diff_pool", &dm, &traderEnv.bd);
    diff.setStub(&stub);
    WTSVariant* c = cfg(1);
    ASSERT_NE(nullptr, c);
    ASSERT_TRUE(diff.init(c));
    c->release();
    diff.setTrader(&trader);

    // 通道就绪后成交/撤单直接转发到 TraderAdapter
    diff.buy(kCode, 3500.0, 1, false);
    diff.sell(kCode, 3501.0, 1, false);
    diff.cancel(1u);
    diff.cancel(kCode, true, 1);

    // 创建 ag 单元；再对 rb/au 下发组合目标，触发组合匹配与池调度
    diff.on_position_changed(kCode, 2.0);

    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb.2510"] = 3.0;
    targets["SHFE.au.2212"] = 3.0;
    diff.set_position(targets);

    // 相同目标再下发一次：等值跳过
    diff.set_position(targets);

    // 0 增量早退
    diff.on_position_changed(kCode, 0.0);

    WTSTickData* t = tick("ag2512", 3500.0, 93100000);
    ASSERT_NE(nullptr, t);
    diff.on_tick(kCode, t);
    t->release();

    diff.on_trade(1u, kCode, true, 1, 3500.0);
    diff.on_trade(0u, kCode, true, 1, 3500.0);  // localid==0 早退
    diff.on_order(1u, kCode, true, 1, 0, 3500.0, false);
    diff.on_entrust(1u, kCode, true, "ok");
    diff.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);

    diff.on_channel_lost();
    diff.on_channel_ready();

    // 清空目标：目标仓位记录里的合约逐个归零（池调度）
    wt_hashmap<std::string, double> empty;
    diff.set_position(empty);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

//////////////////////////////////////////////////////////////////
// Local/Arbi：非池分支 + 组合匹配 + 自动清理
// src/WtCore/WtLocalExecuter.cpp:537-547, 565-593, 619-622, 817-821, 850-854, 975-991
// src/WtCore/WtArbiExecuter.cpp:323-344, 432-440, 457-468, 503-506, 878-887
//////////////////////////////////////////////////////////////////
TEST_F(R110ExecEnv, LocalAndArbiNonPoolBranches)
{
    // ---- Local ----
    {
        WtLocalExecuter local(&factory, "r110_local_nopool", &dm);
        local.setStub(&stub);
        WTSVariant* c = cfg(0);
        ASSERT_NE(nullptr, c);
        ASSERT_TRUE(local.init(c));
        c->release();
        local.setTrader(&trader);

        local.on_position_changed(kCode, 2.0);

        wt_hashmap<std::string, double> targets;
        targets["SHFE.rb.2510"] = 3.0;
        targets["SHFE.au.2212"] = 3.0;
        local.set_position(targets);

        // 非池通道回调，单元已存在
        local.on_channel_lost();
        local.on_channel_ready();

        // 上一期主力 + clear include 命中 -> 非池 clear_all_position
        traderEnv.hot.prev_ = "ag2512";
        local.on_position(kCode, true, 0, 0, 1, 1, 20250110);

        wt_hashmap<std::string, double> empty;
        local.set_position(empty);
    }

    // ---- Arbi ----
    {
        WtArbiExecuter arbi(&factory, "r110_arbi_nopool", &dm);
        arbi.setStub(&stub);
        WTSVariant* c = cfg(0);
        ASSERT_NE(nullptr, c);
        ASSERT_TRUE(arbi.init(c));
        c->release();
        arbi.setTrader(&trader);

        arbi.cancel(1u);
        arbi.cancel(kCode, true, 1);

        arbi.on_position_changed(kCode, 2.0);

        wt_hashmap<std::string, double> targets;
        targets["SHFE.rb.2510"] = 3.0;
        targets["SHFE.au.2212"] = 3.0;
        arbi.set_position(targets);

        arbi.on_channel_lost();
        arbi.on_channel_ready();

        traderEnv.hot.prev_ = "ag2512";
        arbi.on_position(kCode, true, 0, 0, 1, 1, 20250110);

        wt_hashmap<std::string, double> empty;
        arbi.set_position(empty);
    }
}

}  // namespace
