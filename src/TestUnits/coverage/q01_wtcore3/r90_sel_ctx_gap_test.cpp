/**
 * r90_sel_ctx_gap_test.cpp — lane R90 / wave17
 *
 * 目标：src/WtCore/SelStraBaseCtx.cpp 剩余公开/受保护路径。
 *
 * 覆盖行段：
 *   - load_data：details 缺 maxprice/minprice 的默认值、零仓位、frozen：:321-327, :286-290
 *   - stra_set_position 的 T+1 冻结拒绝：:755-765
 *   - update_dyn_profit 零仓位与 maxloss：:515-535
 *   - on_schedule/on_tick/on_session_end 的 userdata 落盘：:626-630, :711-715
 *   - stra_get_price/stra_get_day_price 的空引擎回落：:726-732, :1006-1012
 *   - 信号触发 do_set_position 的开/平/反手与 log_close：:790-891, :159-168
 *   - stra_get_bars/stra_get_last_tick 的引擎切片委托：:944-976
 *
 * 引擎用 q01 的裸 SelEngine 派生并补 port_fund/data_mgr；不 init/不 run/不起线程。
 */
#include "q01_sel_env.h"
#include "r90_env.h"
#include "../n02_wtcore_engines/r73_fixture.h"

#include <WtCore/WtDtMgr.h>
#include <Includes/WTSRiskDef.hpp>

#include <fstream>

using namespace q01sel;

namespace {

::testing::Environment* const g_r90_env =
    ::testing::AddGlobalTestEnvironment(new r90::GenEnv);

class R90SelEngine : public q01sel::SelEngine
{
public:
    R90SelEngine() { _port_fund = WTSPortFundInfo::create(); }

    void setDataMgr(WtDtMgr* dm) { _data_mgr = dm; }
    void primePrice(const char* code, double price) { _price_map[code] = price; }
};

class R90SelCtx : public ::testing::Test
{
protected:
    BaseDataMgr bd;
    HotMgr hot;
    WTSCommodityInfo* agComm = NULL;
    WTSContractInfo* agCt = NULL;
    WTSCommodityInfo* auComm = NULL;
    WTSContractInfo* auCt = NULL;
    R90SelEngine* engine = NULL;

    void SetUp() override
    {
        WtHelper::setGenerateDir(r90::temp_root().c_str());
        WtHelper::setTime(20250110, 931, 0);

        bd.regisSession("TRADING");
        agComm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        agComm->setSessionInfo(bd.getSession("TRADING"));
        agCt = makeCt("SHFE", "ag", "ag2512");
        bd.regis(agComm, agCt);

        // 仅多 T+1，用于冻结拒绝分支
        auComm = makeComm("SHFE", "au", "TRADING", TM_LongT1, CC_Future);
        auComm->setSessionInfo(bd.getSession("TRADING"));
        auCt = makeCt("SHFE", "au", "au2212");
        bd.regis(auComm, auCt);

        WTSCommodityInfo* rbComm = makeComm("SHFE", "rb", "TRADING", TM_Both, CC_Future);
        rbComm->setSessionInfo(bd.getSession("TRADING"));
        bd.regis(rbComm, makeCt("SHFE", "rb", "rb2510"));

        engine = new R90SelEngine();  // WtSelEngine 析构读未初始化 _tm_ticker(BUG-R42-001)，故意不回收
        engine->attach(&bd, &hot);
    }

    void TearDown() override {}

    std::string dataFile(const char* name)
    {
        return std::string(WtHelper::getStraDataDir()) + name + ".json";
    }

    void writeData(const char* name, const std::string& json)
    {
        std::ofstream ofs(dataFile(name).c_str(), std::ios::binary | std::ios::trunc);
        ofs << json;
    }
};

const char* const kAg = "SHFE.ag.2512";
const char* const kAu = "SHFE.au.2212";

//////////////////////////////////////////////////////////////////
// 缺省 maxprice/minprice、零仓位、T+1 冻结、负浮盈、userdata 落盘
// src/WtCore/SelStraBaseCtx.cpp:321-327, 515-535, 627-630, 697,
// 712-715, 755-765, 726-732, 1006-1012
//////////////////////////////////////////////////////////////////
TEST_F(R90SelCtx, LoadDefaultsFrozenAndSessionFlush)
{
    writeData("r90load", R"JSON({
      "fund": {"total_profit":100.0, "total_dynprofit":50.0, "total_fees":5.0, "tdate":20250110},
      "positions": [
        {"code":"SHFE.ag.2512","volume":2.0,"closeprofit":100.0,"dynprofit":50.0,
         "lastentertime":202501100931,"lastexittime":202501101000,
         "details":[{"long":true,"price":5200.0,"volume":2.0,"opentime":202501100931,
                     "opentdate":20250110,"profit":50.0,"maxprofit":60.0,"maxloss":-5.0,
                     "opentag":"buy1"}]},
        {"code":"SHFE.au.2212","volume":2.0,"closeprofit":0.0,"dynprofit":0.0,
         "lastentertime":0,"lastexittime":0,"frozen":1.0,"frozendate":20250110,
         "details":[{"long":true,"price":400.0,"volume":2.0,"opentime":202501100931,
                     "opentdate":20250110,"profit":0.0,"maxprofit":0.0,"maxloss":0.0,
                     "opentag":"au1"}]},
        {"code":"SHFE.rb.2510","volume":0.0,"closeprofit":0.0,"dynprofit":0.0,
         "lastentertime":0,"lastexittime":0,"details":[]}
      ],
      "signals": {}
    })JSON");

    SelCtx ctx(engine, "r90load", 0);
    ctx.on_init();

    // 缺省 maxprice/minprice 时回落到开仓价
    EXPECT_DOUBLE_EQ(5200.0, ctx.stra_get_detail_profit(kAg, "buy1", 2));
    EXPECT_DOUBLE_EQ(5200.0, ctx.stra_get_detail_profit(kAg, "buy1", -2));
    EXPECT_DOUBLE_EQ(2.0, ctx.stra_get_position(kAg));

    // 空引擎回落
    SelCtx naked(NULL, "naked", 0);
    EXPECT_DOUBLE_EQ(0.0, naked.stra_get_price(kAg));
    EXPECT_DOUBLE_EQ(0.0, naked.stra_get_day_price(kAg, 0));

    // T+1 冻结 1 手：目标 0 < frozen -> 拒绝，仓位保持 2
    EXPECT_DOUBLE_EQ(2.0, ctx.stra_get_position(kAu));
    EXPECT_DOUBLE_EQ(1.0, ctx.stra_get_position(kAu, true));
    ctx.stra_set_position(kAu, 0.0, "freeze");
    EXPECT_DOUBLE_EQ(2.0, ctx.stra_get_position(kAu));

    // 零仓位持仓：update_dyn_profit 直接清零（rb 只在 load_data 中存在）
    WTSTickData* rt = makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3000.0);
    ctx.on_tick("SHFE.rb.2510", rt, false);
    rt->release();

    // 负浮盈 -> maxloss 更新；userdata 在 on_tick 中落盘
    ctx.stra_save_user_data("k", "v");
    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93100000, 5000.0);
    ctx.on_tick(kAg, tk, false);
    tk->release();

    std::string ud = std::string(WtHelper::getStraUsrDatDir()) + "ud_r90load.json";
    EXPECT_TRUE(std::filesystem::exists(ud));

    // session_end：零仓位 continue + userdata 二次落盘
    ctx.stra_save_user_data("k2", "v2");
    ctx.on_session_end(20250110);
    EXPECT_NE(std::string::npos, r90::read_file(ud).find("k2"));

}

//////////////////////////////////////////////////////////////////
// 信号触发 do_set_position：开 -> 反手 -> 平；含 log_close 的错写文件
// src/WtCore/SelStraBaseCtx.cpp:159-168, 790-891, 944-963
//////////////////////////////////////////////////////////////////
TEST_F(R90SelCtx, SignalExecutionOpenReverseAndFlat)
{
    r73::DataMgr dm;
    engine->setDataMgr(&dm);
    engine->primePrice(kAg, 5000.0);

    SelCtx ctx(engine, "r90exec", 0);
    ctx.on_init();

    // 先登记 bar 订阅并让 stra_get_bars 回填价格缓存
    WTSKlineSlice* bars = ctx.stra_get_bars(kAg, "d", 10);
    ASSERT_NE(nullptr, bars);
    EXPECT_DOUBLE_EQ(3007.0, ctx.stra_get_price(kAg));  // r73 夹具最后一根 close
    bars->release();

    WTSTickData* last = ctx.stra_get_last_tick(kAg);
    ASSERT_NE(nullptr, last);
    last->release();

    // 开多 3 手
    ctx.stra_set_position(kAg, 3.0, "open");
    WTSTickData* t1 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93100000, 5000.0);
    ctx.on_tick(kAg, t1, false);
    t1->release();
    EXPECT_DOUBLE_EQ(3.0, ctx.stra_get_position(kAg));
    EXPECT_DOUBLE_EQ(5000.0, ctx.stra_get_position_avgpx(kAg));

    // 反手 -1：平 3 多 + 开 1 空
    ctx.stra_set_position(kAg, -1.0, "reverse");
    WTSTickData* t2 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93101000, 5100.0);
    ctx.on_tick(kAg, t2, false);
    t2->release();
    EXPECT_DOUBLE_EQ(-1.0, ctx.stra_get_position(kAg));
    EXPECT_DOUBLE_EQ(5100.0, ctx.stra_get_position_avgpx(kAg));

    // 平空归零
    ctx.stra_set_position(kAg, 0.0, "flat");
    WTSTickData* t3 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93102000, 5200.0);
    ctx.on_tick(kAg, t3, false);
    t3->release();
    EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_position(kAg));

    // log_close 守卫写的是 _trade_logs（BUG-Q01-001）：平仓记录落在 trades.csv
    std::string folder = std::string(WtHelper::getOutputDir()) + "r90exec/";
    EXPECT_NE(std::string::npos, r90::read_file(folder + "trades.csv").find("CLOSE"));
    // closes.csv 只有表头，未写入任何平仓明细（BUG-Q01-001）
    EXPECT_EQ(std::string::npos,
        r90::read_file(folder + "closes.csv").find("SHFE.ag.2512"));
}

}  // namespace
