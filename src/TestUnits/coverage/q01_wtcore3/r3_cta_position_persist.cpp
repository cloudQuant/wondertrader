/**
 * r3_cta_position_persist.cpp —— lane R3（WtCore 第三波）
 *
 * 覆盖 src/WtCore/CtaStraBaseCtx.cpp 的以下行段（行号按当前 dev 版本）：
 *   L77-188   init_outputs：7 个 csv 的「新文件写表头 / 老文件 seek_to_end」两条分支
 *   L190-218  log_signal / log_trade / log_close 的写盘
 *   L219-275  save_userdata / load_userdata（含空文件、坏 json 早退）
 *   L277-494  load_data：fund / positions+details / signals / conditions / utils 五段回读
 *   L495-643  save_data：同上五段落盘
 *   L679-750  dump_chart_info
 *   L752-793  update_dyn_profit（_profit/_max_profit/_max_loss/_max_price/_min_price 累计）
 *   L795-935  on_tick：信号落地（bFireAtOnce 真/假两条）+ 条件单五种 action 分支
 *   L937-1016 on_schedule：主 k 线收盘触发、清条件单、条件单 settime 落地、barno 自增
 *   L1018-1038 on_session_begin：冻结持仓释放 + 用户数据补写
 *   L1041-1071 enum_position：bForExecute 时把信号标记为已触发
 *   L1073-1109 on_session_end：positions.csv / funds.csv + save_data
 *   L1411-1575 do_set_position：同向加仓 / 平仓拆分 / 反手 / T+1 冻结 / 滑点 / 手续费
 *   L1966-2063 图表接口（mark 只能在 schedule 内添加等 4 个拒绝分支）
 * 复用 lane p01 的替身（p01_cta_env.h：WtCtaEngine 派生 + IBaseDataMgr/IHotMgr 假实现）。
 */
#include "../p01_wtcore_stractx/p01_cta_env.h"

#include <cctype>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

/** 能窥视 protected 簿记的 CTA 上下文（只读访问，不改语义） */
class R3CtaCtx : public P01CtaCtx
{
public:
    R3CtaCtx(WtCtaEngine* engine, const char* name, int32_t slippage = 0)
        : P01CtaCtx(engine, name, slippage) {}

    PositionMap& posmap() { return _pos_map; }
    SignalMap& sigmap() { return _sig_map; }
    CondEntrustMap& conds() { return _condtions; }
    decltype(_kline_tags)& klines() { return _kline_tags; }
    StringHashMap& ud() { return _user_datas; }
    void dumpChart() { dump_chart_info(); }
    decltype(_fund_info)& fund() { return _fund_info; }
    uint32_t& barno() { return _last_barno; }
    uint64_t& condmin() { return _last_cond_min; }
    std::string& mainkey() { return _main_key; }
    bool& in_schedule() { return _is_in_schedule; }

    /** 在 on_calculate 里注入动作（用来走「调度中登记条件单 / 打点」的分支） */
    std::function<void(R3CtaCtx&)> calc_hook;

    virtual void on_calculate(uint32_t curDate, uint32_t curTime) override
    {
        P01CtaCtx::on_calculate(curDate, curTime);
        if (calc_hook)
            calc_hook(*this);
    }
};

/** 能窥视组合层簿记的 CTA 引擎 */
class R3CtaEngine : public P01CtaEngine
{
public:
    R3CtaEngine() {}

    void attach2(IBaseDataMgr* bd, IHotMgr* hot) { attach(bd, hot); }
    size_t port_pos_count() { return _pos_map.size(); }
    double port_pos(const char* code)
    {
        auto it = _pos_map.find(code);
        return it == _pos_map.end() ? 0.0 : it->second->_volume;
    }
    size_t port_sig_count() { return _sig_map.size(); }
};

class R3CtaTest : public ::testing::Test
{
protected:
    P01TempEnv        env{ "r3_cta" };
    FakeBaseDataMgr   bd;
    FakeHotMgr        hot;
    R3CtaEngine       engine;
    std::unique_ptr<R3CtaCtx> ctx;
    std::string       name_;

    static const char* kCode;    // 可多空、非 T1
    static const char* kT1Code;  // TM_LongT1 => isT1()

    void SetUp() override
    {
        auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        name_ = std::string("r3_") + info->name();
        for (auto& ch : name_)
            if (!std::isalnum((unsigned char)ch) && ch != '_')
                ch = '_';

        bd.regisSession("TRADING");
        // 空时段的 session 会让 isInTradingTime 恒为 false（timeToMinutes 先判 empty）
        bd.session("TRADING")->addTradingSection(900, 1130);

        WTSCommodityInfo* rbComm = p01MakeComm("SHFE", "rb", "TRADING", TM_Both);
        rbComm->setSessionInfo(bd.getSession("TRADING"));
        bd.regis(rbComm, p01MakeCt("SHFE", "rb", "rb2510"));
        bd.regis(rbComm, p01MakeCt("SHFE", "rb", "rb2601"));

        WTSCommodityInfo* ofComm = p01MakeComm("SZSE", "of", "TRADING", TM_LongT1);
        ofComm->setSessionInfo(bd.getSession("TRADING"));
        bd.regis(ofComm, p01MakeCt("SZSE", "of", "of159915"));

        engine.attach(&bd, &hot);
        engine.set_date_time(20250110, 931, 0);
        engine.set_trading_date(20250110);

        outDir();
        ctx.reset(new R3CtaCtx(&engine, name_.c_str(), 0));
    }

    std::string outDir()
    {
        std::filesystem::path p(WtHelper::getOutputDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        p = std::filesystem::path(WtHelper::getStraDataDir());
        std::filesystem::create_directories(p, ec);
        p = std::filesystem::path(WtHelper::getStraUsrDatDir());
        std::filesystem::create_directories(p, ec);
        return std::string(WtHelper::getOutputDir());
    }

    // 喂一条 tick（bEmitStrategy 固定 false，避免策略回调干扰）
    void feed(const char* code, double price, const char* rawcode = "rb2510")
    {
        WTSTickData* tick = p01MakeTick("SHFE", rawcode, 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        ctx->on_tick(code, tick, false);
        tick->release();
    }

    void feedAt(const char* code, const char* exchg, const char* rawcode, double price)
    {
        WTSTickData* tick = p01MakeTick(exchg, rawcode, 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        ctx->on_tick(code, tick, false);
        tick->release();
    }

    static std::string readAll(const std::string& path)
    {
        std::ifstream ifs(path, std::ios::binary);
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    std::string ctxFile(const char* fname)
    {
        return std::string(WtHelper::getOutputDir()) + name_ + "/" + fname;
    }
};

const char* R3CtaTest::kCode = "SHFE.rb2510";
const char* R3CtaTest::kT1Code = "SZSE.of159915";

//////////////////////////////////////////////////////////////////////////
// init_outputs
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, init_outputs_writes_seven_headers_then_appends_on_reopen)
{
    ctx->on_init();
    const char* heads[] = {
        "trades.csv", "code,time,direct,action,price,qty,tag,fee,barno",
        "closes.csv", "code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit,entertag,exittag,openbarno,closebarno",
        "funds.csv", "date,closeprofit,positionprofit,dynbalance,fee",
        "signals.csv", "code,target,sigprice,gentime,usertag",
        "positions.csv", "date,code,volume,closeprofit,dynprofit",
        "indice.csv", "bartime,index_name,line_name,value",
        "marks.csv", "bartime,price,icon,tag"
    };
    for (int i = 0; i < 7; i++)
    {
        std::string body = readAll(ctxFile(heads[i * 2]));
        EXPECT_TRUE(std::filesystem::exists(ctxFile(heads[i * 2]))) << heads[i * 2];
        EXPECT_NE(body.find(heads[i * 2 + 1]), std::string::npos) << heads[i * 2];
    }

    // 第二轮：同名 ctx 再 on_init 走 seek_to_end 分支，表头不得重复
    R3CtaCtx again(&engine, name_.c_str(), 0);
    again.on_init();
    std::string sig = readAll(ctxFile("signals.csv"));
    size_t first = sig.find("code,target");
    EXPECT_EQ(sig.find("code,target", first + 1), std::string::npos) << sig;
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：反手建仓（_volume*diff<=0 且明细为空的分支）
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, flat_signal_on_tick_opens_detail_and_pushes_portfolio_delta)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 3, "open_a");
    ASSERT_EQ(ctx->sigmap().size(), 1u);
    EXPECT_EQ(ctx->sigmap()[kCode]._volume, 3.0);
    EXPECT_EQ(ctx->sigmap()[kCode]._sigtype, 1u);   // 非调度期 => 立即可触发
    EXPECT_EQ(engine.port_pos_count(), 0u);

    feed(kCode, 3500.0);

    EXPECT_EQ(ctx->sigmap().count(kCode), 0u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);
    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 1u);
    EXPECT_TRUE(ctx->posmap()[kCode]._details[0]._long);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._details[0]._volume, 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_cost(kCode, "open_a"), 3500.0);
    EXPECT_EQ(ctx->stra_get_detail_entertime(kCode, "open_a"), 202501100931ULL);
    EXPECT_NE(ctx->stra_get_last_entertime(kCode), 0u);
    // bFireAtOnce=true 会把增量提交给引擎组合层
    EXPECT_EQ(engine.port_pos_count(), 1u);
    EXPECT_DOUBLE_EQ(engine.port_pos(kCode), 3.0);

    std::string trades = readAll(ctxFile("trades.csv"));
    EXPECT_NE(trades.find("SHFE.rb2510,202501100931,LONG,OPEN,"), std::string::npos) << trades;
    EXPECT_NE(trades.find(",open_a,"), std::string::npos) << trades;
    std::string sigs = readAll(ctxFile("signals.csv"));
    EXPECT_NE(sigs.find("SHFE.rb2510,3,3500,"), std::string::npos) << sigs;
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：同向加仓分支（_volume*diff>0）
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, same_direction_signal_appends_second_detail_and_keeps_avg_price)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 3, "open_a");
    feed(kCode, 3500.0);
    ctx->stra_enter_long(kCode, 2, "open_b");
    feed(kCode, 3510.0);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 5.0);
    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 2u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode, false, "open_b"), 2.0);
    // (3500*3 + 3510*2) / 5 = 3504
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kCode), 3504.0);
    EXPECT_DOUBLE_EQ(engine.port_pos(kCode), 5.0);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._closeprofit, 0.0);
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：平仓拆分 + 盈亏 + closes.csv
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, partial_close_splits_detail_and_books_closeprofit)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 3, "open_a");
    feed(kCode, 3500.0);

    ctx->stra_exit_long(kCode, 2, "close_a");
    ASSERT_EQ(ctx->sigmap()[kCode]._volume, 1.0);
    feed(kCode, 3600.0);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 1.0);
    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 1u);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._details[0]._volume, 1.0);
    // (3600-3500)*2*volScale(10) = 2000
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._closeprofit, 2000.0);
    EXPECT_DOUBLE_EQ(ctx->fund()._total_profit, 2000.0);
    EXPECT_NE(ctx->stra_get_last_exittime(kCode), 0u);
    // 剩下的 1 手按 3600 计价：dynprofit = 100*1*10 = 1000
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit(kCode), 1000.0);
    EXPECT_DOUBLE_EQ(ctx->fund()._total_dynprofit, 1000.0);
    EXPECT_DOUBLE_EQ(engine.port_pos(kCode), 1.0);

    std::string closes = readAll(ctxFile("closes.csv"));
    EXPECT_NE(closes.find("SHFE.rb2510,LONG,202501100931,3500,202501100931,3600,2,2000.00,2000.00,open_a,close_a"),
              std::string::npos) << closes;
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：反手（平仓量用完仍有剩余 => 反向建明细）
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, set_position_through_zero_reverses_side_with_single_detail)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 2, "open_a");
    feed(kCode, 3500.0);
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kCode), 2.0);

    ctx->stra_set_position(kCode, -3, "rev");
    feed(kCode, 3500.0);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -3.0);
    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 1u);
    EXPECT_FALSE(ctx->posmap()[kCode]._details[0]._long);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._details[0]._volume, 3.0);
    EXPECT_STREQ(ctx->posmap()[kCode]._details[0]._opentag, "rev");
    EXPECT_DOUBLE_EQ(ctx->stra_get_last_enterprice(kCode), 3500.0);
    EXPECT_DOUBLE_EQ(engine.port_pos(kCode), -3.0);
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：T+1 冻结 + on_session_begin 释放
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, t1_entry_freezes_position_until_next_session_begin)
{
    ctx->on_init();
    ctx->stra_enter_long(kT1Code, 2, "t1open");
    feedAt(kT1Code, "SZSE", "of159915", 12.5);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kT1Code), 2.0);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kT1Code]._frozen, 2.0);
    EXPECT_EQ(ctx->posmap()[kT1Code]._frozen_date, 20250110u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kT1Code, true), 0.0);   // 可用 = 2 - 2

    ctx->on_session_begin(20250111);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kT1Code]._frozen, 0.0);
    EXPECT_EQ(ctx->posmap()[kT1Code]._frozen_date, 0u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kT1Code, true), 2.0);

    // 冻结日期不小于新交易日时不得释放
    ctx->posmap()[kT1Code]._frozen = 1.0;
    ctx->posmap()[kT1Code]._frozen_date = 20250112;
    ctx->on_session_begin(20250112);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kT1Code]._frozen, 1.0);
}

//////////////////////////////////////////////////////////////////////////
// enum_position + bFireAtOnce=false 支路
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, enum_position_for_execute_marks_signal_triggered_and_skips_portfolio)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 4, "sig_only");

    std::vector<std::pair<std::string, double>> seen;
    ctx->enum_position([&seen](const char* stdCode, double qty) {
        seen.emplace_back(stdCode, qty);
    }, true);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].first, std::string(kCode));
    EXPECT_DOUBLE_EQ(seen[0].second, 4.0);
    EXPECT_TRUE(ctx->sigmap()[kCode]._triggered);

    feed(kCode, 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 4.0);
    // 已触发的信号不再向引擎提交增量
    EXPECT_EQ(engine.port_pos_count(), 0u);
}

//////////////////////////////////////////////////////////////////////////
// on_tick 条件单：OL / CL
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, condition_ol_and_cl_fire_on_matching_tick)
{
    ctx->on_init();
    // 先建 -2 手空，走 COND_ACTION_OL 的「当前为空仓 => 目标=qty」支路
    ctx->stra_enter_short(kCode, 2, "short_base");
    feed(kCode, 3500.0);
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kCode), -2.0);

    ctx->stra_enter_long(kCode, 3, "ol_tag", 3400.0);     // 限价 3400 => 价格 <= 3400 触发
    ASSERT_EQ(ctx->conds()[kCode].size(), 1u);
    EXPECT_EQ(ctx->conds()[kCode][0]._action, (char)COND_ACTION_OL);
    EXPECT_EQ(ctx->conds()[kCode][0]._alg, WCT_SmallerOrEqual);

    feed(kCode, 3450.0);                                  // 未触发
    EXPECT_EQ(ctx->conds()[kCode].size(), 1u);
    EXPECT_EQ(ctx->cond_fires, 0u);

    feed(kCode, 3390.0);                                  // 触发：只登记 sigtype=2 的信号
    EXPECT_EQ(ctx->conds().count(kCode), 0u);
    ASSERT_EQ(ctx->sigmap().count(kCode), 1u);
    EXPECT_EQ(ctx->sigmap()[kCode]._sigtype, 2u);
    EXPECT_DOUBLE_EQ(ctx->sigmap()[kCode]._volume, 3.0);   // 空仓反手 => +3

    feed(kCode, 3390.0);                                  // 下一跳落地并回调 on_condition_triggered
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);
    EXPECT_EQ(ctx->cond_fires, 1u);
    EXPECT_EQ(ctx->_cond_tag, "ol_tag");
    EXPECT_DOUBLE_EQ(ctx->_cond_target, 3.0);

    // COND_ACTION_CL：现有 3 手多，限价 3600 => 价格 >= 3600 平 2
    ctx->stra_exit_long(kCode, 2, "cl_tag", 3600.0);
    ASSERT_EQ(ctx->conds()[kCode].size(), 1u);
    EXPECT_EQ(ctx->conds()[kCode][0]._action, (char)COND_ACTION_CL);
    feed(kCode, 3610.0);                                   // 触发 => 目标 3-min(3,2)=1
    ASSERT_EQ(ctx->sigmap().count(kCode), 1u);
    EXPECT_DOUBLE_EQ(ctx->sigmap()[kCode]._volume, 1.0);
    feed(kCode, 3610.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 1.0);
    EXPECT_EQ(ctx->cond_fires, 2u);
}

//////////////////////////////////////////////////////////////////////////
// on_tick 条件单：OS / CS / SP
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, condition_os_cs_and_sp_fire_on_matching_tick)
{
    ctx->on_init();
    // COND_ACTION_OS：stopprice 3300 => 价格 <= 3300 开空 2（当前 0 手 => 0-2）
    ctx->stra_enter_short(kCode, 2, "os_tag", 0.0, 3300.0);
    ASSERT_EQ(ctx->conds()[kCode].size(), 1u);
    EXPECT_EQ(ctx->conds()[kCode][0]._action, (char)COND_ACTION_OS);
    EXPECT_EQ(ctx->conds()[kCode][0]._alg, WCT_SmallerOrEqual);
    feed(kCode, 3290.0);
    feed(kCode, 3290.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -2.0);

    // COND_ACTION_CS：limitprice 3200 => 价格 <= 3200 平空 1（curQty<0 => -2+1）
    ctx->stra_exit_short(kCode, 1, "cs_tag", 3200.0);
    EXPECT_EQ(ctx->conds()[kCode][0]._action, (char)COND_ACTION_CS);
    feed(kCode, 3190.0);
    feed(kCode, 3190.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -1.0);

    // COND_ACTION_SP + isBuy=false（目标 -4 < 当前 -1）=> 限价单用 LargerOrEqual
    ctx->stra_set_position(kCode, -4, "sp_tag", 0.0, 3100.0);
    EXPECT_EQ(ctx->conds()[kCode][0]._action, (char)COND_ACTION_SP);
    EXPECT_EQ(ctx->conds()[kCode][0]._alg, WCT_LargerOrEqual);
    feed(kCode, 3110.0);
    feed(kCode, 3110.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), -4.0);
    EXPECT_EQ(ctx->cond_fires, 3u);
    EXPECT_EQ(ctx->_cond_tag, "sp_tag");
}

//////////////////////////////////////////////////////////////////////////
// on_schedule 主体
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, on_schedule_after_main_bar_close_clears_conditions_and_stamps_settime)
{
    ctx->on_init();
    // 造一条已收盘的主 k 线标记
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.date = 20250110;
    bar.close = 3500.0;
    ctx->on_bar(kCode, "m", 1, &bar);
    ctx->mainkey() = "SHFE.rb2510#m1";

    // 调度前登记一条条件单，on_calculate 里再登记一条
    ctx->stra_enter_long(kCode, 9, "stale", 3000.0);
    ASSERT_EQ(ctx->conds()[kCode].size(), 1u);
    bool entered = false;
    ctx->calc_hook = [&entered](R3CtaCtx& c) {
        c.stra_enter_long("SHFE.rb2601", 2, "fresh", 0.0, 4000.0);
        c.add_chart_mark(3500.0, "buy1", "mark_tag");
        entered = true;
    };

    EXPECT_TRUE(ctx->on_schedule(20250110, 931));
    EXPECT_TRUE(entered);
    EXPECT_EQ(ctx->calc_times, 1u);
    EXPECT_EQ(ctx->_last_calc_date, 20250110u);
    EXPECT_EQ(ctx->_last_calc_time, 931u);
    EXPECT_EQ(ctx->barno(), 1u);                          // L1014
    // 旧条件单被清空，on_calculate 里的新条件单被保留并写了 settime
    EXPECT_EQ(ctx->conds().count(kCode), 0u);
    ASSERT_EQ(ctx->conds().count("SHFE.rb2601"), 1u);
    EXPECT_EQ(ctx->condmin(), 202501100931ULL);
    EXPECT_FALSE(ctx->in_schedule());

    std::string marks = readAll(ctxFile("marks.csv"));
    EXPECT_NE(marks.find("202501100931,3500,buy1,mark_tag"), std::string::npos) << marks;

    std::string datas = readAll(std::string(WtHelper::getStraDataDir()) + name_ + ".json");
    EXPECT_NE(datas.find("\"settime\" : 202501100931"), std::string::npos) << datas;
    EXPECT_NE(datas.find("fresh"), std::string::npos) << datas;

    // 非交易时段（收盘之后）不再触发计算
    engine.set_date_time(20250110, 1500, 0);
    ctx->on_bar(kCode, "m", 1, &bar);
    EXPECT_FALSE(ctx->on_schedule(20250110, 1500));
    EXPECT_EQ(ctx->calc_times, 1u);
}

//////////////////////////////////////////////////////////////////////////
// save_data / load_data 全量往返
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, save_then_load_roundtrip_restores_positions_signals_conditions_userdata)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 3, "open_a");
    feed(kCode, 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 3.0);

    ctx->stra_enter_short(kCode, 2, "pend");              // 保留一条未触发的信号
    ctx->stra_set_position("SHFE.rb2601", 5, "cond_sp", 3800.0);
    ctx->stra_save_user_data("rk1", "rv1");
    ctx->barno() = 7;
    ctx->condmin() = 202501100931ULL;
    ctx->on_session_end(20250110);

    std::string datas = readAll(std::string(WtHelper::getStraDataDir()) + name_ + ".json");
    EXPECT_NE(datas.find("\"volume\" : 3"), std::string::npos) << datas;
    EXPECT_NE(datas.find("\"lastbarno\" : 7"), std::string::npos) << datas;
    EXPECT_NE(datas.find("open_a"), std::string::npos) << datas;

    R3CtaCtx rd(&engine, name_.c_str(), 0);
    rd.on_init();

    EXPECT_DOUBLE_EQ(rd.stra_get_position(kCode), 3.0);
    ASSERT_EQ(rd.posmap()[kCode]._details.size(), 1u);
    EXPECT_DOUBLE_EQ(rd.posmap()[kCode]._details[0]._price, 3500.0);
    EXPECT_EQ(rd.posmap()[kCode]._details[0]._opentdate, 20250110u);
    EXPECT_STREQ(rd.posmap()[kCode]._details[0]._opentag, "open_a");
    ASSERT_EQ(rd.sigmap().count(kCode), 1u);
    EXPECT_DOUBLE_EQ(rd.sigmap()[kCode]._volume, -2.0);
    EXPECT_EQ(rd.sigmap()[kCode]._usertag, "pend");
    ASSERT_EQ(rd.conds().count("SHFE.rb2601"), 1u);
    EXPECT_EQ(rd.conds()["SHFE.rb2601"][0]._action, (char)COND_ACTION_SP);
    EXPECT_DOUBLE_EQ(rd.conds()["SHFE.rb2601"][0]._target, 3800.0);
    EXPECT_EQ(rd.barno(), 7u);
    EXPECT_EQ(rd.condmin(), 202501100931ULL);
    EXPECT_EQ(rd.ud()["rk1"], "rv1");
    EXPECT_EQ(rd.stra_load_user_data("rk1", "def"), "rv1");
    EXPECT_EQ(rd.stra_load_user_data("nope", "def"), "def");

    // on_session_end 的两条 csv
    std::string poss = readAll(ctxFile("positions.csv"));
    EXPECT_NE(poss.find("20250110,SHFE.rb2510,3,0.00,0.00"), std::string::npos) << poss;
    std::string funds = readAll(ctxFile("funds.csv"));
    EXPECT_NE(funds.find("20250110,"), std::string::npos) << funds;
}

//////////////////////////////////////////////////////////////////////////
// 坏文件 / 空文件早退
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, broken_and_empty_data_files_are_ignored_without_state_change)
{
    std::string datapath = std::string(WtHelper::getStraDataDir()) + name_ + ".json";
    std::string usrpath = std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name_ + ".json";

    { std::ofstream(datapath) << "{ not json "; std::ofstream(usrpath) << "{ \"a\" : "; }
    ctx->on_init();
    EXPECT_EQ(ctx->posmap().size(), 0u);
    EXPECT_EQ(ctx->sigmap().size(), 0u);
    EXPECT_EQ(ctx->ud().size(), 0u);

    std::ofstream emptyData(datapath), emptyUsg(usrpath);   // 空内容早退分支
    emptyData.close();
    emptyUsg.close();
    R3CtaCtx second(&engine, name_.c_str(), 0);
    second.on_init();
    EXPECT_EQ(second.posmap().size(), 0u);
    EXPECT_EQ(second.ud().size(), 0u);
}

//////////////////////////////////////////////////////////////////////////
// update_dyn_profit 的最高/最低跟踪
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, dyn_profit_tracks_best_and_worst_price_of_each_detail)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 2, "open_a");
    feed(kCode, 3500.0);
    feed(kCode, 3550.0);
    feed(kCode, 3450.0);

    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 1u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kCode, "open_a", 0), -1000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kCode, "open_a", 1), 1000.0);   // 最大浮盈
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kCode, "open_a", -1), -1000.0); // 最大浮亏
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kCode, "open_a", 2), 3550.0);   // _max_price
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kCode, "open_a", -2), 3450.0);  // _min_price
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit(kCode), -1000.0);
    EXPECT_DOUBLE_EQ(ctx->fund()._total_dynprofit, -1000.0);
    // 未登记的 userTag 走 strcmp 不匹配 => 0
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kCode, "ghost", 1), 0.0);

    // 平到 0 时浮动盈亏清零
    ctx->stra_set_position(kCode, 0, "flat");
    feed(kCode, 3450.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit(kCode), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kCode), 0.0);
}

//////////////////////////////////////////////////////////////////////////
// 图表接口：非调度期的拒绝分支 + dump_chart_info
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, chart_apis_reject_outside_schedule_and_dump_chart_info_writes_json)
{
    ctx->on_init();
    EXPECT_EQ(ctx->stra_get_tdate(), 20250110u);

    // 非调度期：mark 与 index value 都要被拒绝（L1976 / L2032）
    ctx->add_chart_mark(100.0, "icon_x", "tag_x");
    EXPECT_EQ(readAll(ctxFile("marks.csv")).find("icon_x"), std::string::npos);
    EXPECT_FALSE(ctx->set_index_value("macd", "dif", 1.0));

    // 未注册指标 / 未注册线
    EXPECT_FALSE(ctx->register_index_line("macd", "dif", 0));
    EXPECT_FALSE(ctx->add_index_baseline("macd", "dif", 0.0));

    ctx->set_chart_kline(kCode, "m1");
    ctx->register_index("macd", 1);
    EXPECT_TRUE(ctx->register_index_line("macd", "dif", 2));
    EXPECT_TRUE(ctx->add_index_baseline("macd", "zero", 0.0));

    ctx->in_schedule() = true;
    EXPECT_TRUE(ctx->set_index_value("macd", "dif", 3.5));
    EXPECT_FALSE(ctx->set_index_value("macd", "ghost_line", 1.0));
    EXPECT_FALSE(ctx->set_index_value("ghost_idx", "dif", 1.0));
    ctx->in_schedule() = false;

    ctx->dumpChart();
    std::string chart = readAll(ctxFile("rtchart.json"));
    EXPECT_NE(chart.find("\"kline\""), std::string::npos) << chart;
    EXPECT_NE(chart.find("SHFE.rb2510"), std::string::npos) << chart;
    EXPECT_NE(chart.find("\"macd\""), std::string::npos) << chart;
    EXPECT_NE(chart.find("\"dif\" : \"dif\""), std::string::npos) << chart;
    EXPECT_NE(chart.find("\"baselines\""), std::string::npos) << chart;

    std::string idx = readAll(ctxFile("indice.csv"));
    EXPECT_NE(idx.find("macd,dif,3.5"), std::string::npos) << idx;
}

//////////////////////////////////////////////////////////////////////////
// 用户数据在 on_tick / on_session_begin 上的补写
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, modified_userdata_is_flushed_on_tick_and_session_begin)
{
    ctx->on_init();
    ctx->stra_save_user_data("cnt", "17");
    EXPECT_TRUE(ctx->ud()["cnt"] == "17");

    std::string usrpath = std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name_ + ".json";
    EXPECT_FALSE(std::filesystem::exists(usrpath));

    feed(kCode, 3500.0);                       // on_tick 末尾 flush
    ASSERT_TRUE(std::filesystem::exists(usrpath));
    EXPECT_NE(readAll(usrpath).find("\"cnt\" : \"17\""), std::string::npos) << readAll(usrpath);

    ctx->stra_save_user_data("cnt", "18");
    ctx->on_session_begin(20250111);           // 第二条 flush 路径
    EXPECT_NE(readAll(usrpath).find("\"cnt\" : \"18\""), std::string::npos);

    R3CtaCtx rd(&engine, name_.c_str(), 0);
    rd.on_init();
    EXPECT_EQ(rd.stra_load_user_data("cnt", "0"), "18");
}

//////////////////////////////////////////////////////////////////////////
// on_bar：k 线标记 + 订阅开关
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, on_bar_marks_tag_closed_and_only_notifies_subscribed_keys)
{
    ctx->on_init();
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.date = 20250110;
    bar.close = 3500.0;

    // 未订阅 => 只打标记，不回调策略
    ctx->on_bar(kCode, "m", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 0u);
    const std::string kkey = "SHFE.rb2510#m1";
    ASSERT_EQ(ctx->klines().count(kkey), 1u);
    EXPECT_TRUE(ctx->klines()[kkey]._closed);

    ctx->klines()[kkey]._notify = true;
    ctx->on_bar(kCode, "m", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 1u);
    EXPECT_EQ(ctx->_last_close_period, "m1");
    EXPECT_EQ(ctx->_last_close_code, kCode);

    // 主 k 线收盘时走 log_debug 支路
    ctx->mainkey() = kkey;
    ctx->on_bar(kCode, "m", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 2u);
    // 空 newBar 早退
    ctx->on_bar(kCode, "m", 1, NULL);
    EXPECT_EQ(ctx->bar_closes, 2u);
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：全平（明细清零后被剔除）+ 浮动盈亏清零
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, full_close_drops_all_details_and_zeroes_dynprofit)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 2, "open_a");
    feed(kCode, 3500.0);
    feed(kCode, 3550.0);
    ASSERT_GT(ctx->stra_get_position_profit(kCode), 0.0);

    ctx->stra_exit_long(kCode, 5, "close_all");      // 超过持仓 => 按可平量截断
    EXPECT_DOUBLE_EQ(ctx->sigmap()[kCode]._volume, 0.0);
    feed(kCode, 3600.0);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 0.0);
    EXPECT_EQ(ctx->posmap()[kCode]._details.size(), 0u);      // count 归零后 erase
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._dynprofit, 0.0);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._closeprofit, 2000.0);  // (3600-3500)*2*10
    EXPECT_DOUBLE_EQ(engine.port_pos(kCode), 0.0);
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：滑点
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, slippage_shifts_entry_up_and_exit_down)
{
    R3CtaCtx slip(&engine, "r3_slip_ctx", 2);        // 2 个 tick
    std::filesystem::path d(std::string(WtHelper::getOutputDir()) + "r3_slip_ctx");
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    slip.on_init();
    slip.stra_enter_long(kCode, 1, "s");
    feed(kCode, 3500.0);
    // 引擎价格 3500 + 2*priceTick(1.0) = 3502
    EXPECT_DOUBLE_EQ(slip.stra_get_position(kCode), 1.0);
    EXPECT_DOUBLE_EQ(slip.stra_get_detail_cost(kCode, "s"), 3502.0);
    // 市价单成交价由 on_tick 写入的 _price_map 决定，这里再喂一条 3500 后平掉
    slip.stra_exit_long(kCode, 1, "x");
    feed(kCode, 3500.0);
    // 平仓：3500 - 2 = 3498，亏 4*1*10 = -40
    EXPECT_DOUBLE_EQ(slip.stra_get_position(kCode), 0.0);
    EXPECT_DOUBLE_EQ(slip.fund()._total_profit, -40.0);
}

//////////////////////////////////////////////////////////////////////////
// do_set_position：多条明细按先进先出扣减，扣完的从头清理
//////////////////////////////////////////////////////////////////////////
TEST_F(R3CtaTest, close_consumes_details_fifo_and_eroses_finished_ones)
{
    ctx->on_init();
    ctx->stra_enter_long(kCode, 2, "lot1");
    feed(kCode, 3500.0);
    ctx->stra_enter_long(kCode, 3, "lot2");
    feed(kCode, 3600.0);
    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 2u);

    ctx->stra_set_position(kCode, 1, "trim");        // 平 4 手：lot1 全部 + lot2 的 2 手
    feed(kCode, 3550.0);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kCode), 1.0);
    ASSERT_EQ(ctx->posmap()[kCode]._details.size(), 1u);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._details[0]._price, 3600.0);
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._details[0]._volume, 1.0);
    // (3550-3500)*2*10 + (3550-3600)*2*10 = 1000 - 1000 = 0
    EXPECT_DOUBLE_EQ(ctx->posmap()[kCode]._closeprofit, 0.0);
    EXPECT_DOUBLE_EQ(ctx->fund()._total_profit, 0.0);
    EXPECT_STREQ(ctx->stra_get_last_entertag(kCode), "lot2");
}

}  // namespace
