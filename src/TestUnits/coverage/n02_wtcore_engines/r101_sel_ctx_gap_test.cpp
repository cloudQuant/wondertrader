/**
 * r101_sel_ctx_gap_test.cpp — lane R101 / wave18
 *
 * 目标：src/WtCore/SelStraBaseCtx.cpp 剩余仓位记账与查询分支。
 *
 * 覆盖行段：
 *   - 信号触发 do_set_position 的同向加仓、部分平仓、清仓与明细清理：
 *     :786-891（含 :159-168 log_close）
 *   - 空明细/零仓位的持仓查询兜底：:1081-1142, :1176-1194
 *   - userTag 不匹配的 continue 与返回值：:1206-1223, :1245-1273
 *   - 引擎价格回落：:722-732, :1006-1012
 *   - load_userdata 空内容/坏 JSON 早退与 on_schedule 用户数据落盘：:198-219, :626-630
 */
#include <TestUnits/coverage/q01_wtcore3/q01_sel_env.h>
#include <TestUnits/coverage/q01_wtcore3/r90_env.h>
#include <TestUnits/coverage/n02_wtcore_engines/r73_fixture.h>

#include <WtCore/WtDtMgr.h>
#include <Includes/WTSRiskDef.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace q01sel;

namespace {

::testing::Environment* const g_r101_env =
    ::testing::AddGlobalTestEnvironment(new r90::GenEnv);

class R101SelEngine : public q01sel::SelEngine
{
public:
    R101SelEngine() { _port_fund = WTSPortFundInfo::create(); }

    void setDataMgr(WtDtMgr* dm) { _data_mgr = dm; }
};

const char* const kAg = "SHFE.ag.2512";
const char* const kAu = "SHFE.au.2212";
const char* const kRb = "SHFE.rb.2510";

std::string data_file(const char* name)
{
    return std::string(WtHelper::getStraDataDir()) + name + ".json";
}

void write_text(const std::string& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
    ofs << content;
}

std::string position_json()
{
    return R"JSON({
  "fund": {"total_profit": 0.0, "total_dynprofit": 0.0, "total_fees": 0.0, "tdate": 20250110},
  "positions": [
    {"code":"SHFE.ag.2512","volume":3.0,"closeprofit":0.0,"dynprofit":0.0,
     "lastentertime":202501100931,"lastexittime":0,"frozen":0.0,"frozendate":0,
     "details":[{"long":true,"price":5000.0,"volume":3.0,"opentime":202501100931,
       "opentdate":20250110,"maxprice":5000.0,"minprice":5000.0,
       "profit":0.0,"maxprofit":0.0,"maxloss":0.0,"opentag":"a1"}]},
    {"code":"SHFE.au.2212","volume":0.0,"closeprofit":0.0,"dynprofit":0.0,
     "lastentertime":0,"lastexittime":0,"details":[]},
    {"code":"SHFE.rb.2510","volume":2.0,"closeprofit":0.0,"dynprofit":0.0,
     "lastentertime":202501100931,"lastexittime":0,
     "details":[
       {"long":true,"price":4000.0,"volume":1.0,"opentime":202501100931,
        "opentdate":20250110,"maxprice":4000.0,"minprice":4000.0,
        "profit":0.0,"maxprofit":0.0,"maxloss":0.0,"opentag":"r1"},
       {"long":true,"price":4100.0,"volume":1.0,"opentime":202501100932,
        "opentdate":20250110,"maxprice":4100.0,"minprice":4100.0,
        "profit":0.0,"maxprofit":0.0,"maxloss":0.0,"opentag":"r2"}]}
  ],
  "signals": {}
})JSON";
}

class R101SelCtx : public ::testing::Test
{
protected:
    r73::TempDir tmp{"r101_sel"};
    r73::BaseDataMgr bd;
    r73::HotMgr hot;
    r73::DataMgr dm;
    R101SelEngine* engine = NULL;

    void SetUp() override
    {
        WtHelper::setGenerateDir((tmp.path("generated") + "/").c_str());
        WtHelper::setTime(20250110, 1000, 0);

        bd.regisSession("TRADING", 900, 1500);
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);

        WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
        ag->setSessionInfo(sess);
        bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));

        WTSCommodityInfo* au = r73::makeComm("SHFE", "au", "TRADING", TM_Both);
        au->setSessionInfo(sess);
        bd.regis(au, r73::makeCt("SHFE", "au", "au2212"));

        WTSCommodityInfo* rb = r73::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        bd.regis(rb, r73::makeCt("SHFE", "rb", "rb2510"));

        // WtSelEngine 析构会读未初始化 _tm_ticker（BUG-R42-001），故意不回收
        engine = new R101SelEngine();
        engine->attach(&bd, &hot);
    }

    void TearDown() override {}
};

//////////////////////////////////////////////////////////////////
// 同向加仓 -> 部分平仓 -> 清仓；空明细/零仓位/tag 不匹配查询
// src/WtCore/SelStraBaseCtx.cpp:159-168, 722-732, 786-891,
// 1006-1012, 1081-1142, 1176-1194, 1206-1273
//////////////////////////////////////////////////////////////////
TEST_F(R101SelCtx, SignalAccountingAndQueryFallbacks)
{
    engine->setDataMgr(&dm);
    write_text(data_file("r101acct"), position_json());

    SelCtx ctx(engine, "r101acct", 0);
    ctx.on_init();
    ASSERT_DOUBLE_EQ(3.0, ctx.stra_get_position(kAg));
    ASSERT_DOUBLE_EQ(2.0, ctx.stra_get_position(kRb));

    // 先喂一笔行情建立 price_map
    WTSTickData* t0 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93100000, 5000.0);
    ctx.on_tick(kAg, t0, false);
    t0->release();

    // 3 -> 5：同向加仓，新增明细
    ctx.stra_set_position(kAg, 5.0, "add");
    WTSTickData* t1 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93101000, 5100.0);
    ctx.on_tick(kAg, t1, false);
    t1->release();
    EXPECT_DOUBLE_EQ(5.0, ctx.stra_get_position(kAg));

    // 5 -> 4：部分平仓，首条明细 3 -> 2
    ctx.stra_set_position(kAg, 4.0, "cut");
    WTSTickData* t2 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93102000, 5200.0);
    ctx.on_tick(kAg, t2, false);
    t2->release();
    EXPECT_DOUBLE_EQ(4.0, ctx.stra_get_position(kAg));

    // 4 -> 0：逐条平完并清理明细
    ctx.stra_set_position(kAg, 0.0, "flat");
    WTSTickData* t3 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93103000, 5300.0);
    ctx.on_tick(kAg, t3, false);
    t3->release();
    EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_position(kAg));

    // 零仓位、空明细的查询兜底
    EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_position_avgpx(kAu));
    EXPECT_EQ(0u, ctx.stra_get_first_entertime(kAu));
    EXPECT_STREQ("", ctx.stra_get_last_entertag(kAu));
    EXPECT_EQ(0u, ctx.stra_get_last_entertime(kAu));
    EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_last_enterprice(kAu));

    // 有效明细 + userTag 命中/不命中
    EXPECT_DOUBLE_EQ(4050.0, ctx.stra_get_position_avgpx(kRb));
    EXPECT_EQ(202501100931ull, ctx.stra_get_detail_entertime(kRb, "r1"));
    EXPECT_EQ(0u, ctx.stra_get_detail_entertime(kRb, "nope"));
    EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_detail_profit(kRb, "nope", 0));

    // 引擎兜底价格：price_map 未命中的代码走 _data_mgr
    EXPECT_GT(ctx.stra_get_price("SHFE.zz.9999"), 0.0);
    EXPECT_GT(ctx.stra_get_day_price(kRb, 0), 0.0);
}

//////////////////////////////////////////////////////////////////
// load_userdata 空/坏文件早退 + on_schedule 落盘 userdata
// src/WtCore/SelStraBaseCtx.cpp:198-219, 626-630
//////////////////////////////////////////////////////////////////
TEST_F(R101SelCtx, UserDataAndScheduleFlush)
{
    engine->setDataMgr(&dm);
    write_text(data_file("r101empty"), position_json());
    write_text(std::string(WtHelper::getStraUsrDatDir()) + "ud_r101empty.json", "");
    {
        SelCtx ctx(engine, "r101empty", 0);
        ctx.on_init();
    }

    write_text(data_file("r101bad"), position_json());
    write_text(std::string(WtHelper::getStraUsrDatDir()) + "ud_r101bad.json", "{bad json");
    {
        SelCtx ctx(engine, "r101bad", 0);
        ctx.on_init();
    }

    write_text(data_file("r101sched"), position_json());
    SelCtx ctx(engine, "r101sched", 0);
    ctx.on_init();
    ctx.stra_save_user_data("dirty", "1");
    EXPECT_TRUE(ctx.on_schedule(20250110, 1000, 1000));

    const std::string ud = std::string(WtHelper::getStraUsrDatDir()) + "ud_r101sched.json";
    std::ifstream ifs(ud.c_str(), std::ios::binary);
    ASSERT_TRUE(ifs.is_open());
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, content.find("dirty"));
}

}  // namespace
