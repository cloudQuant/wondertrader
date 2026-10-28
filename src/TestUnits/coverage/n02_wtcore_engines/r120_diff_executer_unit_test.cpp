/**
 * r120_diff_executer_unit_test.cpp — lane R120 / wave20
 *
 * 目标：src/WtCore/WtDiffExecuter.cpp 的线程池分支与「目标仓位迁移归零」分支。
 *
 * 背景：R110 的同名场景把 policy.name 写成 "WtExeFact.WtMinImpactExeUnit"，
 * 而 WtExeFact::createDiffExeUnit 只认 "WtDiffMinImpactExeUnit"
 * （src/WtExeFact/WtExeFact.cpp:112-118），getUnit() 恒为空、整段早退。
 * 本 lane 改用正确的差量执行单元名，真实走通 getUnit -> init -> 池调度。
 *
 * 覆盖行段：
 *   - on_position_changed：0 增量早退 :508-509；池调度 :534-540
 *   - set_position：等值跳过 :567-568；池调度 :584-590；
 *     目标迁移归零（含池/非池两支）:610-638
 *   - on_tick 池调度 :657-664；on_trade :686-703；on_order :727-733
 *   - on_entrust :754-761；on_channel_ready :782-787, 803-809；
 *     on_channel_lost :833-838；on_account :871-877
 *
 * 执行单元来自构建产物 libWtExeFact（WtDiffMinImpactExeUnit），只做内存状态机；
 * 句柄未喂过匹配 tick，do_calc 在「没有最新行情」处返回，不会真实下单。
 * 线程池任务在析构 wait() 前用短暂 sleep 排空；全部路径确定性、无网络。
 */
#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/TraderAdapter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecuterFactory.h>

#include <rapidjson/document.h>

#include <chrono>
#include <string>
#include <thread>

using namespace wtp;

namespace {

namespace rj = rapidjson;

const char* const kAg = "SHFE.ag.2512";
const char* const kRb = "SHFE.rb.2510";
const char* const kAu = "SHFE.au.2212";

/** 差量执行单元配置：name 必须是 createDiffExeUnit 认识的名字 */
WTSVariant* makeDiffCfg(uint32_t poolsize)
{
    const std::string json =
        "{\"scale\":1.0,\"poolsize\":" + std::to_string(poolsize) + ","
        "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtDiffMinImpactExeUnit\","
        "\"offset\":0,\"expire\":0,\"pricemode\":0,\"span\":0,"
        "\"byrate\":false,\"lots\":1,\"rate\":0.0}}}";
    return WTSCfgLoader::load_from_content(json.c_str());
}

/** 基础数据替身：补上「按标准代码 + 空交易所」回查合约的路径 */
class R120DiffBd : public r71::R71BaseDataMgr
{
public:
    WTSContractInfo* getContract(const char* code, const char* exchg = "",
                                 uint32_t uDate = 0) override
    {
        WTSContractInfo* ct = r71::R71BaseDataMgr::getContract(code, exchg, uDate);
        if (ct == NULL && exchg != NULL && strlen(exchg) == 0)
            ct = r71::R71BaseDataMgr::getContract(code, "SHFE", uDate);
        return ct;
    }
};

class R120DiffTest : public ::testing::Test
{
protected:
    r71::R71GenEnv    env;
    r71::R71PluginDir plug;
    r71::R71TraderEnv traderEnv;
    R120DiffBd        bd;
    r71::R71ExecStub  stub;
    r71::R71DataMgr   dm;
    WtExecuterFactory factory;

    WTSCommodityInfo* comm = NULL;
    WTSSessionInfo*   sess = NULL;

    void SetUp() override
    {
        ASSERT_TRUE(plug.ok()) << "缺少 libWtExeFact";
        ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));

        bd.regisSession("TRADING");
        sess = bd.session("TRADING");
        ASSERT_NE(nullptr, sess);
        sess->addTradingSection(900, 1500);

        comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
        comm->setSessionInfo(sess);
        WTSContractInfo* agCt = r71::makeCt("SHFE", "ag", "ag2512");
        bd.regis(comm, agCt);
        // set_position 迁移归零时用标准代码回查合约
        bd.regisAlias(kAg, agCt);

        WTSCommodityInfo* rb = r71::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        WTSContractInfo* rbCt = r71::makeCt("SHFE", "rb", "rb2510");
        bd.regis(rb, rbCt);
        bd.regisAlias(kRb, rbCt);

        WTSCommodityInfo* au = r71::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        WTSContractInfo* auCt = r71::makeCt("SHFE", "au", "au2212");
        bd.regis(au, auCt);
        bd.regisAlias(kAu, auCt);

        stub.comm_ = comm;
        stub.sess_ = sess;
        stub.hot_ = &traderEnv.hot;

        ASSERT_TRUE(traderEnv.build(trader, "r120_diff"));
    }

    std::string exec_file(const char* name) const
    {
        return std::string(WtHelper::getExecDataDir()) + name + ".json";
    }

    double jsonValue(const std::string& content, const char* section,
                     const char* code, const char* field) const
    {
        rj::Document root;
        root.Parse(content.c_str());
        EXPECT_FALSE(root.HasParseError()) << content;
        if (root.HasParseError() || !root.HasMember(section))
            return -99999.0;

        const rj::Value& arr = root[section];
        for (const rj::Value& item : arr.GetArray())
        {
            if (std::string(item["code"].GetString()) != code)
                continue;
            return item[field].GetDouble();
        }
        return -99999.0;
    }

    std::string read_exec(const char* name) const
    {
        return r71::read_file(exec_file(name));
    }

    TraderAdapter trader;
};

//////////////////////////////////////////////////////////////////
// 池调度覆盖全部回报入口 + 差量记账落盘
// src/WtCore/WtDiffExecuter.cpp:508-509, 534-540, 584-590, 657-664,
// 686-703, 727-733, 754-761, 782-787, 803-809, 833-838, 871-877
//////////////////////////////////////////////////////////////////
TEST_F(R120DiffTest, PoolForwardsEveryCallbackAndPersistsDiff)
{
    WtDiffExecuter exe(&factory, "r120diff_pool", &dm, &bd);
    exe.setStub(&stub);
    WTSVariant* cfg = makeDiffCfg(2);
    ASSERT_NE(nullptr, cfg);
    ASSERT_TRUE(exe.init(cfg));
    cfg->release();
    exe.setTrader(&trader);

    // 通道就绪：交易适配器已登录完成
    EXPECT_TRUE(trader.isReady());
    EXPECT_STREQ("r120_diff", trader.id());

    // 0 增量：先建单元，再在 :509 直接返回
    exe.on_position_changed(kAg, 0.0);

    // 非 0 增量：目标 0 -> 2，差量 0 -> 2；池调度 set_position
    exe.on_position_changed(kAg, 2.0);

    // 组合目标：等值再下发一次，走 :567-568 continue；再归零/恢复各一次
    wt_hashmap<std::string, double> targets;
    targets[kAg] = 2.0;
    exe.set_position(targets);
    targets[kAg] = 0.0;
    exe.set_position(targets);
    targets[kAg] = 2.0;
    exe.set_position(targets);

    // 成交回报：差量 2 -> 1，并 save_data 落盘
    exe.on_trade(11u, kAg, true, 1.0, 3500.0);

    const std::string content = read_exec("r120diff_pool");
    ASSERT_FALSE(content.empty());
    EXPECT_DOUBLE_EQ(2.0, jsonValue(content, "targets", kAg, "target"));
    EXPECT_DOUBLE_EQ(1.0, jsonValue(content, "diffs", kAg, "diff"));

    // 池化转发：行情/订单/委托/资金/通道
    WTSTickData* tick = r71::makeTick("SHFE", "ag2512", 20250110, 20250110,
                                      93100000, 3500.0, 3499.0, 3501.0);
    ASSERT_NE(nullptr, tick);
    exe.on_tick(kAg, tick);
    tick->release();

    exe.on_order(11u, kAg, true, 1.0, 0.0, 3500.0, false);
    exe.on_entrust(11u, kAg, true, "ok");
    exe.on_account("CNY", 1000.0, 1000.0, 1000.0, 1000.0,
                   0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    exe.on_channel_lost();
    exe.on_channel_ready();

    // 池任务排空（析构还有一次 wait）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

//////////////////////////////////////////////////////////////////
// set_position 目标迁移归零（池分支）并逐项改写差量
// src/WtCore/WtDiffExecuter.cpp:584-590, 610-638
//////////////////////////////////////////////////////////////////
TEST_F(R120DiffTest, SetPositionMigratesExpiredTargetToZero)
{
    WtDiffExecuter exe(&factory, "r120diff_migrate", &dm, &bd);
    exe.setStub(&stub);
    WTSVariant* cfg = makeDiffCfg(1);
    ASSERT_NE(nullptr, cfg);
    ASSERT_TRUE(exe.init(cfg));
    cfg->release();

    wt_hashmap<std::string, double> targets;
    targets[kAg] = 5.0;
    exe.set_position(targets);       // 建 ag 单元，目标 5，差量 5

    exe.set_position(targets);       // 等值跳过

    wt_hashmap<std::string, double> next;
    next[kRb] = 1.0;
    exe.set_position(next);          // ag 不在新目标 -> 归零，差量 5 -> 10

    const std::string content = read_exec("r120diff_migrate");
    ASSERT_FALSE(content.empty());
    EXPECT_DOUBLE_EQ(0.0, jsonValue(content, "targets", kAg, "target"));
    EXPECT_DOUBLE_EQ(10.0, jsonValue(content, "diffs", kAg, "diff"));
    EXPECT_DOUBLE_EQ(1.0, jsonValue(content, "targets", kRb, "target"));
    EXPECT_DOUBLE_EQ(1.0, jsonValue(content, "diffs", kRb, "diff"));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

}  // namespace
