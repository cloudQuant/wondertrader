/**
 * q01_sel_savedata_test.cpp —— SelStraBaseCtx 落盘链（save_data / load_data / init_outputs / userdata）
 *
 * 覆盖：
 *  - on_init 首轮：init_outputs 建档 + load_data/load_userdata 文件缺失早退分支
 *  - stra_set_position -> append_signal -> save_data（写 signals/positions/fund 三段 JSON）
 *  - 第二个同名 ctx on_init -> load_data 读回 signals（信号恢复 + 订阅）
 *  - 手写完整 datas.json（positions+details+fund）喂 load_data，随后查询族逐项断言
 *  - stra_save_user_data/on_tick -> save_userdata 落盘 + 第二个 ctx load_userdata 读回
 */
#include "q01_sel_env.h"

#include <fstream>
#include <sstream>

using namespace q01sel;

class Q01SelPersistTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("persist"));
        _bdm.regisSession("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _eng.attach(&_bdm, &_hot);
    }

    void TearDown() override { _env.reset(); }

    std::string dataDir()
    {
        std::filesystem::path p(WtHelper::getStraDataDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    std::string usrDir()
    {
        std::filesystem::path p(WtHelper::getStraUsrDatDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    static void writeFile(const std::string& path, const std::string& content)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    SelEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

TEST_F(Q01SelPersistTest, on_init_creates_outputs_and_missing_files_return)
{
    SelCtx ctx(&_eng, "p_init", 0);
    ctx.on_init();   // init_outputs + load_data(missing) + load_userdata(missing)

    std::filesystem::path out(WtHelper::getOutputDir());
    out /= "p_init";
    for (const char* f : { "trades.csv", "closes.csv", "funds.csv", "signals.csv", "positions.csv" })
        EXPECT_TRUE(std::filesystem::exists(out / f)) << f;
}

TEST_F(Q01SelPersistTest, signal_saved_then_recovered)
{
    {
        SelCtx ctx(&_eng, "p_sig", 0);
        ctx.on_init();
        // 本地价格先落地，令 sigprice 可断言
        WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 940, 5300.0);
        ctx.on_tick("SHFE.ag.2512", tk, false);
        tk->release();

        ctx.stra_set_position("SHFE.ag.2512", 5.0, "tag5");   // append_signal -> save_data
        EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 0.0); // 信号未落仓位
    }

    // 第二个同名 ctx 读回信号（load_data -> signals 分支 -> _sig_map + 订阅）
    SelCtx ctx2(&_eng, "p_sig", 0);
    ctx2.on_init();

    double seen = -1;
    std::string seenCode;
    ctx2.enum_position([&](const char* code, double vol) {
        seenCode = code;
        seen = vol;
    });
    EXPECT_EQ(seenCode, "SHFE.ag.2512");
    EXPECT_DOUBLE_EQ(seen, 5.0);   // 来自恢复的信号目标仓位
}

TEST_F(Q01SelPersistTest, load_full_position_fixture)
{
    static const char* kJson = R"JSON({
      "fund": {"total_profit":100.0, "total_dynprofit":50.0, "total_fees":5.0, "tdate":20250110},
      "positions": [
        {
          "code": "SHFE.ag.2512",
          "volume": 2.0,
          "closeprofit": 100.0,
          "dynprofit": 50.0,
          "lastentertime": 202501100931,
          "lastexittime": 202501101000,
          "frozen": 0.0,
          "frozendate": 0,
          "details": [
            {"long":true,"price":5200.0,"volume":2.0,"opentime":202501100931,
             "opentdate":20250110,"maxprice":5250.0,"minprice":5180.0,
             "profit":50.0,"maxprofit":60.0,"maxloss":-5.0,"opentag":"buy1"}
          ]
        }
      ],
      "signals": {}
    })JSON";

    writeFile(dataDir() + "p_load.json", kJson);

    SelCtx ctx(&_eng, "p_load", 0);
    ctx.on_init();   // 读入 positions

    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512"), 2.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", true), 2.0);   // -frozen(0)
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", false, "buy1"), 2.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("SHFE.ag.2512", false, "miss"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SHFE.ag.2512"), 5200.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit("SHFE.ag.2512"), 50.0);
    EXPECT_EQ(ctx.stra_get_first_entertime("SHFE.ag.2512"), 202501100931ull);
    EXPECT_EQ(ctx.stra_get_last_entertime("SHFE.ag.2512"), 202501100931ull);
    EXPECT_EQ(ctx.stra_get_last_exittime("SHFE.ag.2512"), 202501101000ull);
    EXPECT_DOUBLE_EQ(ctx.stra_get_last_enterprice("SHFE.ag.2512"), 5200.0);
    EXPECT_STREQ(ctx.stra_get_last_entertag("SHFE.ag.2512"), "buy1");
    EXPECT_EQ(ctx.stra_get_detail_entertime("SHFE.ag.2512", "buy1"), 202501100931ull);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_cost("SHFE.ag.2512", "buy1"), 5200.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_profit("SHFE.ag.2512", "buy1", 0), 50.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_profit("SHFE.ag.2512", "buy1", 1), 60.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_profit("SHFE.ag.2512", "buy1", -1), -5.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_profit("SHFE.ag.2512", "buy1", 2), 5250.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_detail_profit("SHFE.ag.2512", "buy1", -2), 5180.0);

    // 资金（load_data fund 分支：tdate 命中才读 total_fees）
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(1), 100.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(2), 50.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(3), 5.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(0), 145.0);   // 100 - 5 + 50
}

TEST_F(Q01SelPersistTest, userdata_saved_then_loaded)
{
    {
        SelCtx ctx(&_eng, "p_ud", 0);
        ctx.on_init();
        ctx.stra_save_user_data("k1", "v1");
        ctx.stra_save_user_data("k2", "42");
        // on_tick 里因 _ud_modified 触发 save_userdata 落盘
        WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 945, 5310.0);
        ctx.on_tick("SHFE.ag.2512", tk, false);
        tk->release();
        EXPECT_TRUE(std::filesystem::exists(usrDir() + "ud_p_ud.json"));
    }

    SelCtx ctx2(&_eng, "p_ud", 0);
    ctx2.on_init();   // load_userdata
    EXPECT_STREQ(ctx2.stra_load_user_data("k1", ""), "v1");
    EXPECT_STREQ(ctx2.stra_load_user_data("k2", ""), "42");
    EXPECT_STREQ(ctx2.stra_load_user_data("none", "dflt"), "dflt");
}
