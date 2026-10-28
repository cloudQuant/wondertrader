/**
 * r15_sel_persist.cpp —— lane R15 / SelStraBaseCtx.cpp 的 JSON 落地与回读
 *
 * 覆盖面（对照 manifests/api-brief-R15.md）：
 *   L64        init_outputs 的目录创建 + 「已存在 ⇒ seek_to_end」重开分支（q01 只打了新建）
 *   L173-195   save_userdata 的 rapidjson 组包 + 写文件（经回调 flush 触发）
 *   L211       load_userdata 的 read_file_content（经 on_init 触发）
 *   L241       load_data 的 read_file_content（经 on_init 触发）
 *   L381-475   save_data 的 positions/fund/signals 三段组包 + PrettyWriter 落盘
 *   L1048-1063 stra_log_info/debug/warn/error 四个转发体
 *
 * 只引用 WtCore 一族（BUG-L00-005）：替身全部来自 q01_sel_env.h。
 * save_data/load_data/do_set_position 在 SelStraBaseCtx 里是 private（L50-153），
 * 本文件一律用公开链驱动：on_init(load) / on_tick(浮盈) / on_session_begin(ud 刷)
 * / on_session_end(save) / stra_set_position→append_signal→save_data / on_schedule。
 * q01 的 DISABLED_SelCrashClass 已证明「有交易时段的会话 + 待发信号」会让
 * do_set_position→engine.handle_pos_change→get_cur_price 解引用 NULL _data_mgr，
 * 所以所有 on_tick 都在「无交易时段的会话」下驱动（isInTradingTime 恒 false）。
 * 不起线程、不读挂钟：引擎时间固定 20250110/931/0，raw time 0 ⇒ gentime 完全确定。
 * load_data 对缺字段 JSON 会走 rapidjson 无判空下标（登记 BUG-R15-003），
 * 手工 JSON 一律带齐 fund/positions/signals 三段。
 */
#include "q01_sel_env.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <Includes/WTSVariant.hpp>
#include <WtCore/SelStraBaseCtx.h>
#include <WtCore/WtSelEngine.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

static const char* kAg = "SHFE.ag.2512";

std::string slurp(const std::string& path)
{
    std::ifstream in(path.c_str());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

uint32_t countLines(const std::string& s)
{
    uint32_t n = 0;
    for (char c : s)
        if (c == '\n')
            ++n;
    return n;
}

class R15SelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r15sel"));
        _bdm.regisSession("TRADING");   // 无交易时段：on_tick 的信号永不触发 do_set_position
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _eng.attach(&_bdm, &_hot);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(WtHelper::getStraDataDir()), ec);
        std::filesystem::create_directories(std::filesystem::path(WtHelper::getStraUsrDatDir()), ec);
    }
    void TearDown() override { _env.reset(); }

    std::string dataFile(const char* name)
    {
        return std::string(WtHelper::getStraDataDir()) + name + ".json";
    }

    std::string udFile(const char* name)
    {
        return std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name + ".json";
    }

    std::string outDir(const char* name)
    {
        std::filesystem::path p(WtHelper::getOutputDir());
        p /= name;
        return p.string();
    }

    void writeFile(const std::string& path, const std::string& content)
    {
        std::ofstream out(path.c_str(), std::ios::trunc);
        out << content;
        out.close();
    }

    /** 与 q01_sel_position_test 同形的合法账户 JSON，仓位可参数化的只有 code/vol 两个槽 */
    void writePosJson(const std::string& path, const std::string& body, const std::string& sigs = "{}")
    {
        std::string j = "{\"fund\":{\"total_profit\":0.0,\"total_dynprofit\":0.0,"
            "\"total_fees\":0.0,\"tdate\":20250110},\"positions\":[";
        j += body;
        j += "],\"signals\":";
        j += sigs;
        j += "}";
        writeFile(path, j);
    }

    /** 一笔 2 手多头 @5200、平仓盈亏 1.5 的账本（全字段带齐，绕开 BUG-R15-003） */
    static std::string posAg2()
    {
        return "{\"code\":\"SHFE.ag.2512\",\"volume\":2,\"closeprofit\":1.5,"
            "\"dynprofit\":0,\"lastentertime\":202501100931,\"lastexittime\":0,"
            "\"frozen\":0,\"frozendate\":0,"
            "\"details\":[{\"long\":true,\"price\":5200.0,\"volume\":2,"
            "\"opentime\":202501100931,\"opentdate\":20250110,\"maxprice\":5200.0,"
            "\"minprice\":5200.0,\"profit\":0,\"maxprofit\":0,\"maxloss\":0,"
            "\"opentag\":\"buy1\"}]}";
    }

    WTSTickData* tickAt(double price)
    {
        return makeTick("SHFE", "ag2512", 20250110, 20250110, 931, price);
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    SelEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

} // namespace

/* ---------------- init_outputs：新建 vs 重开不重复表头（L64） ---------------- */

TEST_F(R15SelTest, init_outputs_reopen_keeps_single_header)
{
    {
        SelCtx ctx(&_eng, "s15_reopen");
        ctx.on_init();
    }
    const std::string trades = outDir("s15_reopen") + "/trades.csv";
    const std::string signals = outDir("s15_reopen") + "/signals.csv";
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(trades)));
    EXPECT_EQ(slurp(trades), "code,time,direct,action,price,qty,tag,fee\n");
    EXPECT_EQ(slurp(signals), "code,target,sigprice,gentime,usertag\n");

    {
        SelCtx ctx2(&_eng, "s15_reopen");
        ctx2.on_init();          // isNewFile==false ⇒ seek_to_end 分支
    }
    EXPECT_EQ(countLines(slurp(trades)), 1u);                  // 表头没有被二次写入
    EXPECT_EQ(countLines(slurp(signals)), 1u);
}

/* ---------------- save_userdata / load_userdata（L173-211） ---------------- */

TEST_F(R15SelTest, userdata_flushed_by_callback_then_reloaded_on_init)
{
    {
        SelCtx ctx(&_eng, "s15_ud");
        ctx.on_init();
        ctx.stra_save_user_data("alpha", "1.25");   // 只置脏
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(udFile("s15_ud"))));

        ctx.on_session_begin(20250110);             // _ud_modified ⇒ save_userdata（L173-195）
        ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(udFile("s15_ud"))));
        const std::string text = slurp(udFile("s15_ud"));
        EXPECT_NE(text.find("\"alpha\""), std::string::npos);
        EXPECT_NE(text.find("1.25"), std::string::npos);

        ctx.on_session_begin(20250110);             // 标记已清 ⇒ 不重复写
        EXPECT_EQ(slurp(udFile("s15_ud")).size(), text.size());
    }
    SelCtx ctx2(&_eng, "s15_ud");
    ctx2.on_init();                                 // L211 read_file_content
    EXPECT_STREQ(ctx2.stra_load_user_data("alpha", ""), "1.25");
    EXPECT_STREQ(ctx2.stra_load_user_data("gamma", "dflt"), "dflt");
}

TEST_F(R15SelTest, load_userdata_missing_file_returns_early)
{
    SelCtx ctx(&_eng, "s15_udnone");
    ctx.on_init();
    EXPECT_STREQ(ctx.stra_load_user_data("anything", "dflt"), "dflt");
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(udFile("s15_udnone"))));
}

/* ---------------- save_data 三段组包（L381-475） ---------------- */

TEST_F(R15SelTest, empty_session_end_writes_zero_fund_line_and_three_sections)
{
    SelCtx ctx(&_eng, "s15_empty");
    ctx.on_init();
    ctx.on_session_end(20250110);                  // 末尾 save_data + funds.csv 一行

    const std::string funds = slurp(outDir("s15_empty") + "/funds.csv");
    EXPECT_NE(funds.find("date,closeprofit,positionprofit,dynbalance,fee"), std::string::npos);
    EXPECT_NE(funds.find("20250110,0.00,0.00,0.00,0.00"), std::string::npos);

    const std::string text = slurp(dataFile("s15_empty"));
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("\"positions\""), std::string::npos);
    EXPECT_NE(text.find("\"fund\""), std::string::npos);
    EXPECT_NE(text.find("\"signals\""), std::string::npos);
    EXPECT_NE(text.find("20250110"), std::string::npos);       // fund.tdate 取引擎交易日
}

TEST_F(R15SelTest, dynprofit_update_saved_into_json_and_csv)
{
    SelCtx ctx(&_eng, "s15_dyn");
    writePosJson(dataFile("s15_dyn"), posAg2());
    ctx.on_init();
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg), 2.0);

    // 会话无交易时段 ⇒ 只走价表 + update_dyn_profit：2*(5350-5200)*volScale10 = 3000
    WTSTickData* tk = tickAt(5350.0);
    ASSERT_NE(tk, nullptr);
    ctx.on_tick(kAg, tk, false);
    tk->release();
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kAg), 3000.0);

    ctx.on_session_end(20250110);

    // positions.csv：date,code,volume,closeprofit,dynprofit
    EXPECT_NE(slurp(outDir("s15_dyn") + "/positions.csv").find(
        "20250110,SHFE.ag.2512,2,1.50,3000.00"), std::string::npos);
    // funds.csv：total_profit=1.5(载入), dyn=3000, balance=1.5+3000-0=3000.50
    EXPECT_NE(slurp(outDir("s15_dyn") + "/funds.csv").find(
        "20250110,1.50,3000.00,3000.50,0.00"), std::string::npos);
    // save_data 的 details 段：maxprice 被 5350 抬升、dynprofit=3000
    const std::string json = slurp(dataFile("s15_dyn"));
    EXPECT_NE(json.find("\"maxprice\": 5350"), std::string::npos);
    EXPECT_NE(json.find("\"dynprofit\": 3000"), std::string::npos);
    EXPECT_NE(json.find("buy1"), std::string::npos);           // opentag 原样落盘
}

TEST_F(R15SelTest, saved_dynprofit_reloaded_into_fund)
{
    {
        SelCtx ctx(&_eng, "s15_reload");
        writePosJson(dataFile("s15_reload"), posAg2());
        ctx.on_init();
        WTSTickData* tk = tickAt(5350.0);
        ASSERT_NE(tk, nullptr);
        ctx.on_tick(kAg, tk, false);
        tk->release();
        ctx.on_session_end(20250110);
    }
    SelCtx ctx2(&_eng, "s15_reload");
    ctx2.on_init();                                 // load_data 全量回读
    EXPECT_DOUBLE_EQ(ctx2.stra_get_position(kAg), 2.0);
    EXPECT_DOUBLE_EQ(ctx2.stra_get_position_profit(kAg), 3000.0);
    EXPECT_DOUBLE_EQ(ctx2.stra_get_position_avgpx(kAg), 5200.0);
    EXPECT_DOUBLE_EQ(ctx2.stra_get_fund_data(1), 1.5);          // 总平仓盈亏=Σcloseprofit
    EXPECT_DOUBLE_EQ(ctx2.stra_get_fund_data(2), 3000.0);
    EXPECT_DOUBLE_EQ(ctx2.stra_get_fund_data(0), 3001.5);       // profit-fees+dyn
    EXPECT_DOUBLE_EQ(ctx2.stra_get_fund_data(9), 0.0);          // default 分支
    EXPECT_DOUBLE_EQ(ctx2.stra_get_price(kAg), 0.0);            // 新实例价表为空⇒引擎 0
}

TEST_F(R15SelTest, stra_set_position_appends_signal_and_persists)
{
    SelCtx ctx(&_eng, "s15_sig");
    ctx.on_init();
    // 目标 2 手（当前 0）⇒ append_signal ⇒ log_signal + save_data（signals 段 L445-475）
    ctx.stra_set_position(kAg, 2, "TT");

    // _price_map 为空 ⇒ sigprice 0；raw time 0 ⇒ gentime=20250110*1e9
    const std::string csv = slurp(outDir("s15_sig") + "/signals.csv");
    EXPECT_NE(csv.find("SHFE.ag.2512,2,0,20250110000000000,TT"), std::string::npos);

    const std::string json = slurp(dataFile("s15_sig"));
    EXPECT_NE(json.find("\"usertag\": \"TT\""), std::string::npos);
    EXPECT_NE(json.find("\"volume\": 2"), std::string::npos);

    // 重复设置相同目标仓位 ⇒ stra_set_position 的 decimal::eq 早退，不追加信号
    ctx.stra_set_position(kAg, 2, "TT2");
    EXPECT_EQ(countLines(slurp(outDir("s15_sig") + "/signals.csv")), 2u);
}

TEST_F(R15SelTest, signal_recovered_from_file_and_saved_back)
{
    std::string sigs = "{\"SHFE.ag.2512\":{\"usertag\":\"UU\",\"volume\":5,"
        "\"sigprice\":5300.0,\"gentime\":20250110093100000}}";
    SelCtx ctx(&_eng, "s15_sigload");
    writePosJson(dataFile("s15_sigload"), "", sigs);
    ctx.on_init();                                  // signals 恢复段 + stra_sub_ticks

    WTSTickData* tk = tickAt(5310.0);
    ASSERT_NE(tk, nullptr);
    ctx.on_tick(kAg, tk, false);                     // 无交易时段 ⇒ 信号保持待发
    tk->release();
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg), 0.0);

    ctx.on_session_end(20250110);
    const std::string json = slurp(dataFile("s15_sigload"));
    EXPECT_NE(json.find("\"usertag\": \"UU\""), std::string::npos);
    EXPECT_NE(json.find("\"sigprice\": 5300"), std::string::npos);
    EXPECT_NE(json.find("\"gentime\": 20250110093100000"), std::string::npos);
}

TEST_F(R15SelTest, signal_of_expired_contract_is_skipped)
{
    std::string sigs = "{\"CFFEX.no.2512\":{\"usertag\":\"GONE\",\"volume\":1,"
        "\"sigprice\":100.0,\"gentime\":20250109093000000}}";
    SelCtx ctx(&_eng, "s15_sigskip");
    writePosJson(dataFile("s15_sigskip"), "", sigs);
    ctx.on_init();                                   // continue 分支：不入 _sig_map
    ctx.on_session_end(20250110);
    // 未被恢复 ⇒ 保存回来的 signals 段是空对象，GONE 只可能来自原始文件文本
    EXPECT_NE(slurp(dataFile("s15_sigskip")).find("\"signals\": {}"), std::string::npos);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position("CFFEX.no.2512"), 0.0);
}

/* ---------------- fund tdate / 过期仓位 / 坏文件（load 分支） ---------------- */

static std::string fundJson(uint32_t tdate, double fees)
{
    std::string s = "{\"fund\":{\"total_profit\":100.0,\"total_dynprofit\":20.0,";
    s += "\"total_fees\":";
    s += std::to_string(fees);
    s += ",\"tdate\":";
    s += std::to_string(tdate);
    s += "},\"positions\":[],\"signals\":{}}";
    return s;
}

TEST_F(R15SelTest, fund_fees_restored_only_when_tdate_matches_engine)
{
    SelCtx match(&_eng, "s15_fund_ok");
    writeFile(dataFile("s15_fund_ok"), fundJson(20250110u, 5.5));
    match.on_init();
    EXPECT_DOUBLE_EQ(match.stra_get_fund_data(3), 5.5);        // 同日 ⇒ 手续费恢复

    SelCtx miss(&_eng, "s15_fund_skip");
    writeFile(dataFile("s15_fund_skip"), fundJson(20241231u, 5.5));
    miss.on_init();
    EXPECT_DOUBLE_EQ(miss.stra_get_fund_data(3), 0.0);         // tdate 不同 ⇒ 不恢复
    // positions 段照常执行：空数组 ⇒ 总平仓/动态盈亏被重算为 0
    EXPECT_DOUBLE_EQ(miss.stra_get_fund_data(1), 0.0);
    EXPECT_DOUBLE_EQ(miss.stra_get_fund_data(2), 0.0);
}

TEST_F(R15SelTest, expired_position_zeroed_but_closeprofit_counted)
{
    // CFFEX.no.2512 不在替身基础数据里、hot 规则为空 ⇒ isExpired==true
    std::string body = "{\"code\":\"CFFEX.no.2512\",\"volume\":5,\"closeprofit\":7.5,"
        "\"dynprofit\":9.9,\"lastentertime\":20250109100000000,"
        "\"lastexittime\":0,\"frozen\":0,\"frozendate\":0,"
        "\"details\":[{\"long\":true,\"price\":100.0,\"volume\":5,"
        "\"opentime\":20250109100000000,\"opentdate\":20250109,\"maxprice\":101.0,"
        "\"minprice\":99.0,\"profit\":7.5,\"maxprofit\":8.0,\"maxloss\":1.0,"
        "\"opentag\":\"OLD\"}]}";
    SelCtx ctx(&_eng, "s15_expired");
    writePosJson(dataFile("s15_expired"), body);
    ctx.on_init();

    EXPECT_DOUBLE_EQ(ctx.stra_get_position("CFFEX.no.2512"), 0.0);   // 过期 ⇒ volume 强制 0
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(1), 7.5);               // closeprofit 仍计入总和
    EXPECT_DOUBLE_EQ(ctx.stra_get_fund_data(2), 0.0);               // dynprofit 被清零分支
    EXPECT_EQ(ctx.tick_updates, 0u);
}

TEST_F(R15SelTest, broken_json_and_empty_file_are_ignored)
{
    SelCtx bad(&_eng, "s15_bad");
    writeFile(dataFile("s15_bad"), "{not json at all");
    bad.on_init();                                   // HasParseError ⇒ return
    EXPECT_DOUBLE_EQ(bad.stra_get_position(kAg), 0.0);

    SelCtx empty(&_eng, "s15_bare");
    writeFile(dataFile("s15_bare"), "");
    empty.on_init();                                 // content.empty() ⇒ return
    EXPECT_DOUBLE_EQ(empty.stra_get_position(kAg), 0.0);

    SelCtx none(&_eng, "s15_nofile");
    none.on_init();                                  // !exists ⇒ return
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(dataFile("s15_nofile"))));
}

/* ---------------- 冻结仓位在 session_begin 释放（load+begin 两段配合） ---------------- */

TEST_F(R15SelTest, frozen_position_released_on_next_session_begin)
{
    std::string body = "{\"code\":\"SHFE.ag.2512\",\"volume\":5,\"closeprofit\":0.0,"
        "\"dynprofit\":0,\"lastentertime\":20250109100000000,"
        "\"lastexittime\":0,\"frozen\":1,\"frozendate\":20250109,"
        "\"details\":[{\"long\":true,\"price\":5000.0,\"volume\":5,"
        "\"opentime\":20250109100000000,\"opentdate\":20250109,\"maxprice\":5000.0,"
        "\"minprice\":5000.0,\"profit\":0,\"maxprofit\":0,\"maxloss\":0,"
        "\"opentag\":\"f1\"}]}";
    SelCtx ctx(&_eng, "s15_frozen");
    writePosJson(dataFile("s15_frozen"), body);
    ctx.on_init();
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg), 5.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg, true), 4.0);   // 可用 = 5 - frozen 1

    ctx.on_session_begin(20250110);                  // frozendate(20250109) < 20250110 ⇒ 释放
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg, true), 5.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg), 5.0);
}

/* ---------------- on_schedule：save_data + 自动平仓信号 ---------------- */

TEST_F(R15SelTest, on_schedule_saves_state_and_emits_autoexit_signal)
{
    SelCtx ctx(&_eng, "s15_sched");
    writePosJson(dataFile("s15_sched"), posAg2());
    ctx.on_init();
    EXPECT_EQ(ctx.schedule_fires, 0u);

    // 有仓位、无新信号 ⇒ to_clear 命中 ⇒ append_signal(code, 0, "autoexit")
    EXPECT_TRUE(ctx.on_schedule(20250110, 932, 935));
    EXPECT_EQ(ctx.schedule_fires, 1u);
    EXPECT_EQ(ctx._sched_date, 20250110u);
    EXPECT_EQ(ctx._time_in_sched, 932u);             // 调度期间读到的是 schedule 时间

    // 调度结束恢复引擎时间
    EXPECT_EQ(ctx.stra_get_time(), 931u);
    const std::string csv = slurp(outDir("s15_sched") + "/signals.csv");
    EXPECT_NE(csv.find("SHFE.ag.2512,0,0,20250110000000000,autoexit"), std::string::npos);
    EXPECT_NE(slurp(dataFile("s15_sched")).find("\"usertag\": \"autoexit\""), std::string::npos);
}

/* ---------------- stra_log_*（L1048-1063） ---------------- */

TEST_F(R15SelTest, four_log_levels_do_not_touch_strategy_files)
{
    SelCtx ctx(&_eng, "s15_logs");
    ctx.on_init();
    const size_t tradesBefore = slurp(outDir("s15_logs") + "/trades.csv").size();
    ASSERT_FALSE(std::filesystem::exists(std::filesystem::path(dataFile("s15_logs"))));

    ctx.stra_log_debug("d");
    ctx.stra_log_info("i");
    ctx.stra_log_warn("w");
    ctx.stra_log_error("e");

    // 四个级别只转发 WTSLogger，不得产生/修改策略侧文件
    EXPECT_EQ(slurp(outDir("s15_logs") + "/trades.csv").size(), tradesBefore);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(dataFile("s15_logs"))));
    EXPECT_DOUBLE_EQ(ctx.stra_get_position(kAg), 0.0);
}
