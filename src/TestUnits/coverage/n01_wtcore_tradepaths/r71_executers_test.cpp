/**
 * r71_executers_test.cpp — lane R71 / Local/Arbi/Diff 执行器余量补测
 *
 * 覆盖 WtLocalExecuter.cpp / WtArbiExecuter.cpp / WtDiffExecuter.cpp：
 *   getUnit 的已存在/自动创建/通道就绪回调
 *   set_position 的组合匹配、目标变更、过期目标清零、strict_sync 通道持仓
 *   on_tick/on_order/on_trade/on_entrust/on_channel_ready/lost/on_account 向真实单元转发
 *   on_position_changed 的增量目标与差量更新
 *   getPosition/getUndoneQty/getOrders/getTicks/grabLastTick 的 trader/data_mgr 分支
 *
 * 执行单元由真实 WtExeFact 插件创建（只加载该 dylib 的临时符号链接）；
 * 交易侧用进程内 ITraderApi 替身，不连接任何柜台。
 */
#include "r71_wtcore_env.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtLocalExecuter.h>

using namespace wtp;

namespace {

class R71ExecTest : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    r71::R71PluginDir plug;
    r71::R71TraderEnv traderEnv;
    r71::R71ExecStub stub;
    r71::R71DataMgr dm;
    WtExecuterFactory factory;
    TraderAdapter trader;

    wtp::WTSCommodityInfo* comm = NULL;
    wtp::WTSSessionInfo* sess = NULL;

    void SetUp() override
    {
        ASSERT_TRUE(plug.ok()) << "缺少 libWtExeFact，无法创建真实执行单元";
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

        ASSERT_TRUE(traderEnv.build(trader, "r71_exec"));
    }

    WTSVariant* cfg(const char* name, const char* policyUnit, const char* extra)
    {
        std::string json = std::string("{\"scale\":1.0,\"poolsize\":0,")
            + (extra != NULL ? extra : "")
            + "\"policy\":{\"default\":{\"name\":\"" + policyUnit
            + "\",\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
              "\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
        std::string file = r71::temp_root() + name + ".json";
        if (!r71::write_file(file, json))
            return NULL;
        return WTSCfgLoader::load_from_file(file.c_str());
    }

    WTSTickData* tick(uint32_t atime, double price, const char* code = "SHFE.ag.2512")
    {
        return r71::makeTick("SHFE", code, 20250110, 20250110, atime, price);
    }
};

// 覆盖 WtLocalExecuter.cpp:170-222 getUnit、L274-316 查询、L465-495 on_position_changed、
// L667-723/737-791/798-857/875-902 单元回调转发
TEST_F(R71ExecTest, LocalUnitLifecycleAndCallbackDispatch)
{
    WtLocalExecuter local(&factory, "r71_local", &dm);
    local.setStub(&stub);
    WTSVariant* c = cfg("r71_local", "WtExeFact.WtMinImpactExeUnit", NULL);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(local.init(c));
    c->release();
    local.setTrader(&trader);

    const uint32_t base = traderEnv.api.n_insert;
    // 第一次仓位变化负责自动创建单元，目标 2
    local.on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    local.on_tick("SHFE.ag.2512", t1);
    t1->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 1);

    // 增量目标 -> 单元 set_position；新 tick 触发第二笔
    local.on_position_changed("SHFE.ag.2512", 1.0);
    WTSTickData* t2 = tick(93101000, 3501.0);
    ASSERT_NE(t2, nullptr);
    local.on_tick("SHFE.ag.2512", t2);
    t2->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 2);

    local.on_order(1, "SHFE.ag.2512", true, 1, 0, 3501.0, false);
    local.on_trade(1, "SHFE.ag.2512", true, 1, 3501.0);
    local.on_entrust(1, "SHFE.ag.2512", true, "ok");
    local.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);

    EXPECT_DOUBLE_EQ(local.getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(local.getUndoneQty("SHFE.ag.2512"), 0.0);
    OrderMap* orders = local.getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    orders->release();

    // 单元已存在 -> 通道事件同步转发
    local.on_channel_lost();
    local.on_channel_ready();
    EXPECT_FALSE(local.cancel(42u));
    local.on_channel_lost();
    EXPECT_TRUE(local.buy("SHFE.ag.2512", 3500.0, 1.0).empty());
    local.on_channel_ready();
    EXPECT_EQ(local.buy("SHFE.ag.2512", 3500.0, 1.0).size(), 1u);

    // on_position 先登记通道持仓；hot 返回空 -> 自动清理链早退
    local.on_position("SHFE.ag.2512", true, 1, 1, 1, 1, 20250110);
    EXPECT_EQ(traderEnv.hot.n_prev_, 1u);

    // data_mgr 非空 -> 行情查询转发
    WTSTickSlice* ticks = local.getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = local.grabLastTick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();
}

// 覆盖 WtLocalExecuter.cpp:75-158 init（含 string 型 includes/excludes）、
// L505-658 set_position 的组合匹配/目标循环/过期清零/strict_sync
TEST_F(R71ExecTest, LocalSetPositionGroupsStaleAndStrictSync)
{
    WtLocalExecuter local(&factory, "r71_local_pos", &dm);
    local.setStub(&stub);
    WTSVariant* c = cfg("r71_local_pos", "WtExeFact.WtMinImpactExeUnit",
        "\"strict_sync\":true,\"clear\":{\"active\":true,\"includes\":\"SHFE.ag\",\"excludes\":\"DCE.i\"},");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(local.init(c));
    c->release();
    local.setTrader(&trader);

    // 目标集合里显式登记通道持仓之外的 cu
    local.on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);
    EXPECT_EQ(traderEnv.hot.n_prev_, 1u); // 上面这行 hot 调用不算，自动清理分支随后再查

    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb.2510"] = 3.0;
    targets["SHFE.au.2212"] = 3.0;
    local.set_position(targets);

    // 新目标为空：旧目标清零；strict_sync 再清 cu（不在 _target_pos）
    wt_hashmap<std::string, double> empty;
    local.set_position(empty);
    WTSTickData* t3 = tick(93102000, 3500.0, "SHFE.cu.2510");
    ASSERT_NE(t3, nullptr);
    local.on_tick("SHFE.cu.2510", t3);
    t3->release();
}

// 覆盖 WtArbiExecuter.cpp:163-202 getUnit、L217-281 查询、L404-540 set_position、
// L548-698 行情/回报转发、L707-801 通道/资金转发
TEST_F(R71ExecTest, ArbiUnitDispatchAndSetPosition)
{
    WtArbiExecuter arbi(&factory, "r71_arbi", &dm);
    arbi.setStub(&stub);
    WTSVariant* c = cfg("r71_arbi", "WtExeFact.WtMinImpactExeUnit",
        "\"strict_sync\":true,\"groups\":{\"R71G\":{\"SHFE.rb.2510\":2.0,\"SHFE.au.2212\":2.0}},");
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(arbi.init(c));
    c->release();
    arbi.setTrader(&trader);

    const uint32_t base = traderEnv.api.n_insert;
    arbi.on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    arbi.on_tick("SHFE.ag.2512", t1);
    t1->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 1);

    arbi.on_position_changed("SHFE.ag.2512", 1.0);
    WTSTickData* t2 = tick(93101000, 3501.0);
    ASSERT_NE(t2, nullptr);
    arbi.on_tick("SHFE.ag.2512", t2);
    t2->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 2);

    // 组合匹配：3 手按 2 手一组可合成 1 组
    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb.2510"] = 3.0;
    targets["SHFE.au.2212"] = 3.0;
    arbi.set_position(targets);

    arbi.on_position("SHFE.cu.2510", true, 0, 0, 1, 1, 20250110);
    wt_hashmap<std::string, double> empty;
    arbi.set_position(empty); // 旧目标清零 + strict_sync 清 cu

    arbi.on_order(1, "SHFE.ag.2512", true, 1, 0, 3501.0, false);
    arbi.on_trade(1, "SHFE.ag.2512", true, 1, 3501.0);
    arbi.on_entrust(1, "SHFE.ag.2512", true, "ok");
    arbi.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
    arbi.on_channel_lost();
    arbi.on_channel_ready();

    EXPECT_DOUBLE_EQ(arbi.getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(arbi.getUndoneQty("SHFE.ag.2512"), 0.0);
    OrderMap* orders = arbi.getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    orders->release();

    WTSTickSlice* ticks = arbi.getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = arbi.grabLastTick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();
}

// 覆盖 WtDiffExecuter.cpp:243-286 getUnit、L304-368 查询、L501-545 on_position_changed、
// L554-642 set_position、L650-817 行情/差量与通道恢复、L862-884 on_account
TEST_F(R71ExecTest, DiffUnitDispatchAndChannelRecovery)
{
    WtDiffExecuter diff(&factory, "r71_diff", &dm, &traderEnv.bd);
    diff.setStub(&stub);
    WTSVariant* c = cfg("r71_diff", "WtExeFact.WtDiffMinImpactExeUnit",
        NULL);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(diff.init(c));
    c->release();
    diff.setTrader(&trader);

    const uint32_t base = traderEnv.api.n_insert;
    diff.on_position_changed("SHFE.ag.2512", 2.0);
    WTSTickData* t1 = tick(93100000, 3500.0);
    ASSERT_NE(t1, nullptr);
    diff.on_tick("SHFE.ag.2512", t1);
    t1->release();
    EXPECT_EQ(traderEnv.api.n_insert, base + 1);

    diff.on_trade(1, "SHFE.ag.2512", true, 1.0, 3500.0);

    wt_hashmap<std::string, double> targets;
    targets["SHFE.ag.2512"] = 3.0;
    diff.set_position(targets);
    WTSTickData* t2 = tick(93101000, 3502.0);
    ASSERT_NE(t2, nullptr);
    diff.on_tick("SHFE.ag.2512", t2);
    t2->release();

    diff.on_order(1, "SHFE.ag.2512", true, 1, 0, 3502.0, false);
    diff.on_entrust(1, "SHFE.ag.2512", true, "ok");
    diff.on_channel_lost();
    diff.on_channel_ready(); // 恢复 _diff_pos
    diff.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);

    // 空目标 -> 过期目标清零（_bd_mgr 已登记标准代码别名，能进入 set_position(0) 分支）
    wt_hashmap<std::string, double> empty;
    diff.set_position(empty);

    EXPECT_DOUBLE_EQ(diff.getPosition("SHFE.ag.2512"), 0.0);
    EXPECT_DOUBLE_EQ(diff.getUndoneQty("SHFE.ag.2512"), 0.0);
    OrderMap* orders = diff.getOrders("SHFE.ag.2512");
    ASSERT_NE(orders, nullptr);
    orders->release();

    WTSTickSlice* ticks = diff.getTicks("SHFE.ag.2512", 1);
    ASSERT_NE(ticks, nullptr);
    ticks->release();
    WTSTickData* last = diff.grabLastTick("SHFE.ag.2512");
    ASSERT_NE(last, nullptr);
    last->release();
}

} // namespace
