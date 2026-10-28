/**
 * r10_wtcore_hft_evt.cpp —— lane R10 / HftStraBaseCtx.cpp + EventNotifier.cpp
 *
 * ── HftStraBaseCtx.cpp ──────────────────────────────────────────────────
 * 本文件只打不触碰 _trader 的路径（_trader 只能由 setTrader 注入真 TraderAdapter，
 * 而报单/撤单主体需要柜台回报，登记为需要接缝，见 BUG-R10-004）。
 * 关键可达块：on_trade（_data_agent=false 时跳过 _sig_logs）→ do_set_position
 *   L785-904 的加仓/先进先出平仓/浮盈缩放全部可离线推导，是本文件的主用例。
 *   L906-939 update_dyn_profit 按 tick 买一价重算浮盈（替身 tick 的十档为 0 ⇒ 可精确算）。
 *   L946-967 on_session_end 把 _fund_info 落到 funds.csv（未交易时数值确定）。
 *   L65-135 init_outputs 的「文件已存在 ⇒ seek_to_end」分支（q01 只打了新建分支）。
 *   L536-599 on_trade/on_order/on_channel_ready/on_channel_lost/on_entrust/on_position。
 *   L601-629 stra_get_position_profit / stra_get_position_avgpx（读 ctx 自己的 _pos_map）。
 *
 * ── EventNotifier.cpp ───────────────────────────────────────────────────
 * 每一个公开 notify* 的第一行都是 `if (_mq_sid == 0) return;`，而 _mq_sid 只能由
 * init() 里插件导出的 create_server 赋值；_asyncio 也是私有 ⇒ 无法无线程排空。
 * 所以本文件只打：构造/析构、init 的两个早退分支、9 个 notify* 的入队守卫。
 * L114-400 的 9 个 lambda（事件序列化 + publish）标「需要接缝」，估计 300 行，
 * 详见 bugs/R10_wtcore.md BUG-R10-005。
 */
#include "q01_sel_env.h"

#include <fstream>
#include <sstream>
#include <string>

#include <Includes/IHftStraCtx.h>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtCore/EventNotifier.h>
#include <WtCore/HftStraBaseCtx.h>
#include <WtCore/WtHftEngine.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

static const char* kAg = "SHFE.ag.2512";

/** 裸高频引擎：只接基础数据/主力规则，不 init/run/起线程（与 q01 同法，本文件自带一份） */
class R10HftEngine : public WtHftEngine
{
public:
    R10HftEngine() {}
    void attach(IBaseDataMgr* bd, IHotMgr* hot)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        set_date_time(20250110, 931, 45, 931);
        set_trading_date(20250110);
    }
};

/** 把 protected 的 do_set_position / get_inner_code 提出来做白盒驱动（替身派生，非宏） */
class R10HftCtx : public HftStraBaseCtx
{
public:
    R10HftCtx(WtHftEngine* e, const char* name, bool agent, int32_t slip)
        : HftStraBaseCtx(e, name, agent, slip) {}
    using HftStraBaseCtx::get_inner_code;
    using HftStraBaseCtx::update_dyn_profit;   // protected，白盒驱动浮盈重算
};

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

class R10HftTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r10hft"));
        _bdm.regisSession("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _eng.attach(&_bdm, &_hot);
    }

    void TearDown() override { _env.reset(); }

    std::string outDir(const char* name)
    {
        std::filesystem::path p(WtHelper::getOutputDir());
        p /= name;
        return p.string();
    }

    std::string usrDir()
    {
        std::filesystem::path p(WtHelper::getStraUsrDatDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    R10HftEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

} // namespace

/* ======================= do_set_position 撮合账本 ======================= */

TEST_F(R10HftTest, on_trade_opens_position_then_avgpx_is_volume_weighted)
{
    R10HftCtx ctx(&_eng, "h10_open", false, 0);
    ctx.on_init();

    // 第一笔：买 1 手 @7100 —— 空仓时走平仓分支的 left>0 反手块（L884-902）
    ctx.on_trade(1001u, kAg, true, 1, 7100.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 7100.0);

    // 第二笔：加 2 手 @7120 —— 同向走加仓块（L807-829）
    ctx.on_trade(1002u, kAg, true, 2, 7120.0);
    // (7100*1 + 7120*2) / 3 = 21340/3
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 21340.0 / 3.0);

    // 目标仓位与当前仓位相同 => L795-796 直接 return，明细数量不变
    double before = ctx.stra_get_position_avgpx(kAg);
    ctx.on_trade(1003u, kAg, true, 0, 9999.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), before);

    // 尚未按 tick 重算 => 浮盈仍是 0
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kAg), 0.0);
}

TEST_F(R10HftTest, on_trade_close_is_fifo_and_leaves_the_last_detail)
{
    R10HftCtx ctx(&_eng, "h10_close", false, 0);
    ctx.on_init();

    ctx.on_trade(2001u, kAg, true, 1, 7100.0);   // 明细A：1 手 @7100
    ctx.on_trade(2002u, kAg, true, 2, 7120.0);   // 明细B：2 手 @7120，合计 3 手
    // 卖 2 手 => curPos=1，先进先出先吃 A(1手) 再吃 B(1手)
    ctx.on_trade(2003u, kAg, false, 2, 7150.0);

    // 剩余明细只可能是 B 的 1 手 @7120；若平仓顺序反了则会算出 7100
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 7120.0);
    // 平仓后浮盈被等比缩放（L861），A 清空、B 缩到 1 手 => 浮盈 0
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kAg), 0.0);
}

TEST_F(R10HftTest, update_dyn_profit_uses_bid_price_and_vol_scale)
{
    R10HftCtx ctx(&_eng, "h10_dyn", false, 0);
    ctx.on_init();
    ctx.on_trade(3001u, kAg, true, 1, 7100.0);
    ctx.on_trade(3002u, kAg, true, 2, 7120.0);

    // 替身 tick 十档全是 0（makeTick 只填 price），多头取买一价 0
    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93145, 7200.0);
    ASSERT_NE(tk, nullptr);
    ctx.update_dyn_profit(kAg, tk);
    tk->release();

    // volScale=10：1*(0-7100)*10 + 2*(0-7120)*10 = -213400
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kAg), -213400.0);

    // 未建仓的合约不在 _pos_map 里 => 两个查询都走 find==end 早退
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx("SSE.600000"), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit("SSE.600000"), 0.0);
    ctx.update_dyn_profit(kAg, NULL);   // 空仓合约：查不到明细直接返回（L908-909）
}

TEST_F(R10HftTest, volume_zero_position_resets_dynprofit)
{
    R10HftCtx ctx(&_eng, "h10_zero", false, 0);
    ctx.on_init();
    ctx.on_trade(4001u, kAg, true, 2, 7100.0);

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93146, 7100.0);
    ASSERT_NE(tk, nullptr);
    ctx.update_dyn_profit(kAg, tk);
    // 2 手 @7100，买一 0 => 2*(0-7100)*10 = -142000
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kAg), -142000.0);

    // 全部平掉 => _volume==0，重算时命中 L912-915 归零
    ctx.on_trade(4002u, kAg, false, 2, 7100.0);
    ctx.update_dyn_profit(kAg, tk);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_profit(kAg), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 0.0);   // L618-619 空仓返回 0
    tk->release();
}

TEST_F(R10HftTest, slippage_shifts_fill_price)
{
    // 1 个 tick 的滑点，priceTick=1.0（makeComm）
    R10HftCtx ctx(&_eng, "h10_slip", false, 1);
    ctx.on_init();
    // 买 2 手 @7100 => 反手块 trdPx = 7100 + 1*1*1 = 7101（L834-835，isBuy=true）
    ctx.on_trade(5001u, kAg, true, 2, 7100.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 7101.0);

    // 加 1 手 @7120 => 加仓块 trdPx = 7120 + 1*1*1 = 7121（L811-814）
    ctx.on_trade(5002u, kAg, true, 1, 7120.0);
    // (7101*2 + 7121*1)/3 = 21323/3
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 21323.0 / 3.0);
}

/* ======================= 回调与落盘 ======================= */

TEST_F(R10HftTest, report_callbacks_flush_userdata)
{
    R10HftCtx ctx(&_eng, "h10_ud", false, 0);
    ctx.on_init();

    // 未修改：五个回调都只是判一次 _ud_modified，不得写文件
    ctx.on_order(1u, kAg, true, 2, 2, 7100.0, false);
    ctx.on_channel_ready();
    ctx.on_channel_lost();
    ctx.on_entrust(1u, kAg, true, "ok");
    ctx.on_position(kAg, true, 0, 0, 1, 1, 20250110);
    ctx.on_session_end(20250110);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(usrDir() + "ud_h10_ud.json")));

    // 修改后：每个回调都会 save_userdata 并清标记
    ctx.stra_save_user_data("kk", "vv");
    ctx.on_order(2u, kAg, true, 2, 0, 7100.0, true);   // isCanceled 分支（L563-566）
    const std::string f = usrDir() + "ud_h10_ud.json";
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(f)));
    EXPECT_NE(slurp(f).find("vv"), std::string::npos);

    // 标记已被清掉 => 再回调一次不会重复写（文件长度不变）
    const std::string once = slurp(f);
    ctx.on_channel_ready();
    ctx.on_channel_lost();
    ctx.on_entrust(3u, kAg, false, "bad");
    EXPECT_EQ(slurp(f).size(), once.size());
}

TEST_F(R10HftTest, session_end_writes_fund_line_when_agent_on)
{
    {
        R10HftCtx ctx(&_eng, "h10_fund", true, 0);
        ctx.on_init();       // init_outputs：新建四个 csv（写表头）
        ctx.on_session_end(20250110);
    }
    const std::string funds = outDir("h10_fund") + "/funds.csv";
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(funds)));
    std::string s = slurp(funds);
    EXPECT_EQ(s.find("date,closeprofit,positionprofit,dynbalance,fee"), 0u);
    // 没有任何交易 => _fund_info 全 0，格式化后数值完全确定
    EXPECT_NE(s.find("20250110,0.00,0.00,0.00,0.00"), std::string::npos);
    EXPECT_EQ(countLines(s), 2u);   // 表头 + 一行

    // 同名 ctx 再次 on_init => 走 seek_to_end 分支（L84-88/99-103/114-118），
    // 表头不会被重复写入，第二笔结算行追加在末尾
    {
        R10HftCtx ctx2(&_eng, "h10_fund", true, 0);
        ctx2.on_init();
        ctx2.on_session_end(20250111);
    }
    std::string s2 = slurp(funds);
    EXPECT_EQ(countLines(s2), 3u);                          // 表头 + 两行
    EXPECT_NE(s2.find("20250111,0.00,0.00,0.00,0.00"), std::string::npos);
    EXPECT_EQ(countLines(slurp(outDir("h10_fund") + "/trades.csv")), 1u);  // 只有表头
}

TEST_F(R10HftTest, close_and_trade_logs_stay_empty_without_trades)
{
    R10HftCtx ctx(&_eng, "h10_logs", true, 0);
    ctx.on_init();
    // agent=true 时 on_trade 会先调 stra_get_position（解引用 _trader），
    // 这里不交易，只验证两个 csv 的表头 + log_trade/log_close 的空文件守卫
    EXPECT_EQ(slurp(outDir("h10_logs") + "/trades.csv").size(),
        std::string("code,time,direct,action,price,qty,tag,fee\n").size());
    EXPECT_EQ(slurp(outDir("h10_logs") + "/closes.csv").size(),
        std::string("code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit,entertag,exittag\n").size());
    EXPECT_STREQ(ctx.get_inner_code(kAg), kAg);           // _code_map 为空 => 原样返回
}

TEST_F(R10HftTest, price_query_falls_back_to_engine)
{
    R10HftCtx ctx(&_eng, "h10_px", false, 0);
    ctx.on_init();
    // ctx 自己的 _price_map 为空 => 落到 _engine->get_cur_price（引擎也没价 => 0）
    EXPECT_DOUBLE_EQ(ctx.stra_get_price(kAg), 0.0);
    EXPECT_DOUBLE_EQ(ctx.stra_get_price("SSE.600000"), 0.0);

    // stra_get_rawcode/stra_get_comminfo 走引擎，不经 _trader
    EXPECT_EQ(ctx.stra_get_comminfo(kAg), _comm);
    EXPECT_STREQ(ctx.stra_get_rawcode(kAg).c_str(), "ag2512");
}

/* ======================= EventNotifier ======================= */

class R10EvtTest : public ::testing::Test
{
protected:
    void SetUp() override { _env.reset(new Q01TempEnv("r10evt")); }
    void TearDown() override { _env.reset(); }
    std::unique_ptr<Q01TempEnv> _env;
};

TEST_F(R10EvtTest, init_inactive_returns_false)
{
    EventNotifier notifier;
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("active", false);
    // L60-61：active=false 直接 false，不碰插件、不起线程
    EXPECT_FALSE(notifier.init(cfg));
}

TEST_F(R10EvtTest, init_without_mq_module_returns_false)
{
    EventNotifier notifier;
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("active", true);
    cfg->append("url", "mcp_20150808_9527");
    // 本构建产物里没有 libWtMsgQue.dylib => L71-76 装载失败早退（不起线程、不建 shm）
    EXPECT_FALSE(notifier.init(cfg));
}

TEST_F(R10EvtTest, every_notify_returns_before_enqueue)
{
    EventNotifier notifier;          // _mq_sid 保持 0
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("active", false);
    ASSERT_FALSE(notifier.init(cfg));

    // 9 个公开入口的守卫（L116/145/171/199/216/309/340/371）：入队前返回，
    // 既不会 post 到 _asyncio，也不会 deref _publisher
    notifier.notify_log("tag", "msg");
    notifier.notify_event("event");
    notifier.notify("CTRADER", "hello");

    // 两个带成交/委托信息的重载：_mq_sid==0 时与空指针一样在入队前返回
    notifier.notify("CTRADER", 6001u, kAg, (WTSTradeInfo*)NULL);
    notifier.notify("CTRADER", 6002u, kAg, (WTSOrderInfo*)NULL);

    notifier.notify_chart_index(20250110093145000ull, "s1", "ma", "ma5", 5.0);
    notifier.notify_chart_marker(20250110093145000ull, "s1", 7100.0, "arrowUp", "t1");
    notifier.notify_trade("s1", kAg, true, true, 20250110093145000ull, 7100.0, "tag");

    // 析构走 L46-56：_worker 为空 ⇒ 不 join；_remover 为空 ⇒ 不调用
    SUCCEED();
}

TEST_F(R10EvtTest, null_trade_and_order_infos_are_guarded)
{
    EventNotifier notifier;
    // _mq_sid==0 时先判空再判 sid，两个顺序都不能 deref（L199/L216）
    notifier.notify("CTRADER", 1u, kAg, (WTSTradeInfo*)NULL);
    notifier.notify("CTRADER", 1u, kAg, (WTSOrderInfo*)NULL);
    EXPECT_TRUE(true);
}
