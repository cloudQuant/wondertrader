/**
 * r73_executers_test.cpp — lane R73 / Local/Arbi/Diff 执行器查询与清理分支补测
 *
 * 覆盖 src/WtCore/WtLocalExecuter.cpp、WtArbiExecuter.cpp、WtDiffExecuter.cpp
 * 中 R59/R71 未覆盖的分支：
 *   - ExecuteContext 门面：writeLog / getCommodityInfo / getSessionInfo / getCurTime
 *   - 无 trader / 无 data_mgr / 无 unit 时的查询与下单守卫
 *   - on_position 的 _auto_clear=false、非分月代码、上一期主力不匹配、
 *     excludes 命中与 includes 不命中五条早退
 *   - Diff 的 ticks/last_tick 委托与 on_position 空实现
 */
#include "r73_fixture.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtLocalExecuter.h>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.rb.2510";

// 返回 NULL 的数据管理器替身，用来覆盖「data_mgr 非空但无数据」的返回路径
class NullDataMgr : public IDataManager
{
public:
    WTSTickSlice* get_tick_slice(const char*, uint32_t, uint64_t = 0) override
    {
        ++tick_calls;
        return NULL;
    }

    WTSTickData* grab_last_tick(const char*) override
    {
        ++last_calls;
        return NULL;
    }

    uint32_t tick_calls = 0;
    uint32_t last_calls = 0;
};

WTSVariant* makeCfg(const char* clear, uint32_t poolsize = 0)
{
    std::string json = std::string("{\"scale\":1.0,\"poolsize\":")
        + std::to_string(poolsize) + "," + (clear == NULL ? "" : clear)
        + "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\"}}}";
    return WTSCfgLoader::load_from_content(json.c_str());
}

class R73ExecTest : public ::testing::Test
{
protected:
    r73::TempDir _tmp{"exec"};
    r73::BaseDataMgr _bd;
    r73::HotMgr _hot;
    NullDataMgr _dm;
    r73::ExecStub _stub;
    WtExecuterFactory _factory;

    WTSCommodityInfo* _comm = NULL;
    WTSSessionInfo* _sess = NULL;

    void SetUp() override
    {
        WtHelper::setGenerateDir((_tmp.path("generated") + "/").c_str());
        WtHelper::setTime(20250110, 1000, 0);

        _sess = _bd.regisSession("TRADING");
        ASSERT_NE(_sess, nullptr);

        _comm = r73::makeComm("SHFE", "rb", "TRADING", TM_Both);
        _comm->setSessionInfo(_sess);
        _bd.regis(_comm, r73::makeCt("SHFE", "rb", "rb2510"));

        _stub.comm = _comm;
        _stub.sess = _sess;
        _stub.hot = &_hot;
    }

    void initLocal(WtLocalExecuter& exe, const char* clear, uint32_t pool = 0)
    {
        WTSVariant* cfg = makeCfg(clear, pool);
        ASSERT_NE(cfg, nullptr);
        ASSERT_TRUE(exe.init(cfg));
        cfg->release();
        exe.setStub(&_stub);
    }

    void initArbi(WtArbiExecuter& exe, const char* clear)
    {
        WTSVariant* cfg = makeCfg(clear);
        ASSERT_NE(cfg, nullptr);
        ASSERT_TRUE(exe.init(cfg));
        cfg->release();
        exe.setStub(&_stub);
    }

    void initDiff(WtDiffExecuter& exe, const char* clear)
    {
        WTSVariant* cfg = makeCfg(clear);
        ASSERT_NE(cfg, nullptr);
        ASSERT_TRUE(exe.init(cfg));
        cfg->release();
        exe.setStub(&_stub);
    }
};

// 覆盖 WtLocalExecuter.cpp:237-263（data_mgr 非空但空数据）、L274-316
// （trader 为空）、L328-410（通道未就绪/L2 撤单）、L419-449（门面访问器）
TEST_F(R73ExecTest, LocalAccessorsAndGuards)
{
    WtLocalExecuter exe(&_factory, "r73_local", &_dm);
    initLocal(exe, NULL);

    exe.writeLog("r73 local write log");
    EXPECT_EQ(_comm, exe.getCommodityInfo(kCode));
    EXPECT_EQ(_sess, exe.getSessionInfo(kCode));
    EXPECT_EQ(20250110093100000ull, exe.getCurTime());

    EXPECT_TRUE(exe.getTicks(kCode, 3) == NULL);
    EXPECT_TRUE(exe.grabLastTick(kCode) == NULL);
    EXPECT_EQ(1u, _dm.tick_calls);
    EXPECT_EQ(1u, _dm.last_calls);

    exe.setTrader(NULL);
    EXPECT_DOUBLE_EQ(0.0, exe.getPosition(kCode));
    EXPECT_DOUBLE_EQ(0.0, exe.getUndoneQty(kCode));
    EXPECT_TRUE(exe.getOrders(kCode) == NULL);
    EXPECT_TRUE(exe.buy(kCode, 3000.0, 1.0).empty());
    EXPECT_TRUE(exe.sell(kCode, 3000.0, 1.0).empty());
    EXPECT_FALSE(exe.cancel(1u));
    EXPECT_TRUE(exe.cancel(kCode, true, 1.0).empty());
}

// 覆盖 WtLocalExecuter.cpp:917-993（on_position 五条早退：auto_clear=false、
// 非分月、上一期主力不匹配、excludes 命中、includes 不命中）
TEST_F(R73ExecTest, LocalAutoClearEarlyExits)
{
    {
        WtLocalExecuter exe(&_factory, "r73_local_off", NULL);
        initLocal(exe, "\"clear\":{\"active\":false},");
        exe.on_position(kCode, true, 0, 0, 1, 1, 20250110);
        EXPECT_EQ(0u, _hot.prev_code.size());
    }

    {
        WtLocalExecuter exe(&_factory, "r73_local_ex", NULL);
        initLocal(exe, "\"clear\":{\"active\":true,\"excludes\":\"SHFE.rb\"},");
        _hot.prev_code = "rb2510";

        // 非分月代码 -> 早退
        exe.on_position("SHFE.rb.HOT", true, 0, 0, 1, 1, 20250110);
        // 上一期主力不匹配 -> 早退
        _hot.prev_code = "";
        exe.on_position(kCode, true, 0, 0, 1, 1, 20250110);
        // 匹配但命中 excludes -> 早退
        _hot.prev_code = "rb2510";
        exe.on_position(kCode, true, 0, 0, 1, 1, 20250110);

        // includes 非空且不包含 -> 早退
        WtLocalExecuter exe2(&_factory, "r73_local_in", NULL);
        initLocal(exe2, "\"clear\":{\"active\":true,\"includes\":\"SHFE.ag\"},");
        exe2.on_position(kCode, true, 0, 0, 1, 1, 20250110);

        // includes 命中 -> 进入 getUnit（工厂未加载，unit 为空，不崩溃）
        WtLocalExecuter exe3(&_factory, "r73_local_hit", NULL);
        initLocal(exe3, "\"clear\":{\"active\":true,\"includes\":\"SHFE.rb\"},");
        exe3.on_position(kCode, true, 0, 0, 1, 1, 20250110);
    }
}

// 覆盖 WtArbiExecuter.cpp:217-253（空数据/trader 查询）、L292-357（通道未就绪）、
// L366-391（门面访问器）、L821-889（on_position 早退与 includes 命中）
TEST_F(R73ExecTest, ArbiAccessorsQueriesAndAutoClear)
{
    WtArbiExecuter exe(&_factory, "r73_arbi", &_dm);
    initArbi(exe, "\"clear\":{\"active\":true,\"includes\":\"SHFE.rb\"},");

    exe.writeLog("r73 arbi write log");
    EXPECT_EQ(_comm, exe.getCommodityInfo(kCode));
    EXPECT_EQ(_sess, exe.getSessionInfo(kCode));
    EXPECT_EQ(20250110093100000ull, exe.getCurTime());

    EXPECT_TRUE(exe.getTicks(kCode, 3) == NULL);
    EXPECT_TRUE(exe.grabLastTick(kCode) == NULL);
    EXPECT_EQ(1u, _dm.tick_calls);
    EXPECT_EQ(1u, _dm.last_calls);

    exe.setTrader(NULL);
    EXPECT_DOUBLE_EQ(0.0, exe.getPosition(kCode));
    EXPECT_DOUBLE_EQ(0.0, exe.getUndoneQty(kCode));
    EXPECT_TRUE(exe.getOrders(kCode) == NULL);
    EXPECT_TRUE(exe.buy(kCode, 3000.0, 1.0).empty());
    EXPECT_TRUE(exe.sell(kCode, 3000.0, 1.0).empty());
    EXPECT_FALSE(exe.cancel(1u));
    EXPECT_TRUE(exe.cancel(kCode, true, 1.0).empty());

    // auto_clear 关闭 / 非分月 / 主力不匹配 / includes 命中
    _hot.prev_code = "";
    exe.on_position("SHFE.rb.HOT", true, 0, 0, 1, 1, 20250110);
    exe.on_position(kCode, true, 0, 0, 1, 1, 20250110);
    _hot.prev_code = "rb2510";
    exe.on_position(kCode, true, 0, 0, 1, 1, 20250110);
}

// 覆盖 WtDiffExecuter.cpp:304-327（空数据查询）、L334-432（trader 为空/通道未就绪）、
// L440-481（门面访问器）、L897-900（on_position 空实现）
TEST_F(R73ExecTest, DiffAccessorsAndSlices)
{
    WtDiffExecuter exe(&_factory, "r73_diff", &_dm, &_bd);
    initDiff(exe, NULL);

    exe.writeLog("r73 diff write log");
    EXPECT_EQ(_comm, exe.getCommodityInfo(kCode));
    EXPECT_EQ(_sess, exe.getSessionInfo(kCode));
    EXPECT_EQ(20250110093100000ull, exe.getCurTime());

    EXPECT_TRUE(exe.getTicks(kCode, 3) == NULL);
    EXPECT_TRUE(exe.grabLastTick(kCode) == NULL);
    EXPECT_EQ(1u, _dm.tick_calls);
    EXPECT_EQ(1u, _dm.last_calls);

    exe.setTrader(NULL);
    EXPECT_DOUBLE_EQ(0.0, exe.getPosition(kCode));
    EXPECT_DOUBLE_EQ(0.0, exe.getUndoneQty(kCode));
    EXPECT_TRUE(exe.getOrders(kCode) == NULL);
    EXPECT_TRUE(exe.buy(kCode, 3000.0, 1.0).empty());
    EXPECT_TRUE(exe.sell(kCode, 3000.0, 1.0).empty());
    EXPECT_FALSE(exe.cancel(1u));
    EXPECT_TRUE(exe.cancel(kCode, true, 1.0).empty());

    exe.on_position(kCode, true, 0, 0, 1, 1, 20250110);
}

}  // namespace
