/**
 * r15_hft_stra_context.cpp —— lane R15 / HftStraContext.cpp（13 个转发方法）
 * + HftStraBaseCtx.h 的 inline 订单标签链（setUserTag/getOrderTag/eraseOrderTag）
 *
 * 替身只引用 WtCore 一族（BUG-L00-005）：WtHftEngine / HftStraBaseCtx /
 * HftStraContext 全部经 WtCore 头走 CoverageTests_wtcore 的编译行。
 * 不触碰 _trader（报单链是 R10 登记的需要接缝，BUG-R10-003/006）。
 * 基类 on_tick/on_bar/on_order_queue/... 的真实主体只做「_ud_modified 时 save_userdata」
 * （HftStraBaseCtx.cpp L143-190），所以可证伪观察点 = ud 文件写与不写。
 */
#include "q01_sel_env.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <Includes/HftStrategyDefs.h>
#include <Includes/WTSDataDef.hpp>
#include <WtCore/HftStraBaseCtx.h>
#include <WtCore/HftStraContext.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHftEngine.h>
#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

using namespace q01sel;

namespace {

USING_NS_WTP;

static const char* kAg = "SHFE.ag.2512";

/** 裸高频引擎：只接替身基础数据，不 init/run/起线程（与 R10 同法） */
class R15HftEngine : public WtHftEngine
{
public:
    R15HftEngine() {}
    void attach(IBaseDataMgr* bd, IHotMgr* hot, WtDtMgr* dm)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        _data_mgr = dm;
        set_date_time(20250110, 931, 45, 931);
        set_trading_date(20250110);
    }
};

/** 被测上下文的测试派生：只为把 protected 的 inline 标签链与订阅集合拿出来白盒驱动 */
class R15HftCtx : public HftStraContext
{
public:
    R15HftCtx(WtHftEngine* e, const char* name, bool agent, int32_t slip)
        : HftStraContext(e, name, agent, slip) {}

    using HftStraBaseCtx::setUserTag;
    using HftStraBaseCtx::getOrderTag;
    using HftStraBaseCtx::eraseOrderTag;
    using HftStraBaseCtx::get_inner_code;

    void sub_tick_direct(const char* stdCode) { _tick_subs.insert(stdCode); }
    // The fixture has no trader; keep position-detail and close logs active
    // while avoiding the unrelated signal-log query through _trader.
    void disable_signal_log_for_test() { _sig_logs.reset(); }
};

class R15UnboundTraderCtx : public R15HftCtx
{
public:
    explicit R15UnboundTraderCtx(WtHftEngine* engine)
        : R15HftCtx(engine, "c15_unbound", false, 0) {}

    bool has_trader() const { return _trader != nullptr; }
    void seed_code_mapping() { _code_map["existing"] = "owner"; }
    bool original_code_mapping_unchanged() const
    {
        const auto it = _code_map.find("existing");
        return _code_map.size() == 1 && it != _code_map.end() && it->second == "owner";
    }
    void force_unbound_for_test() { _trader = nullptr; }
    void stra_log_error(const char* message) override
    {
        ++errors;
        last_error = message ? message : "";
        if (bind_on_error)
            setTrader(bind_on_error);
        if (reenter && !inside_error)
        {
            inside_error = true;
            nested_cancel = stra_cancel(4321);
            inside_error = false;
        }
    }
    bool reenter = false;
    bool inside_error = false;
    bool nested_cancel = true;
    TraderAdapter* bind_on_error = nullptr;
    unsigned errors = 0;
    std::string last_error;
};

class R15RebindingHotMgr : public HotMgr
{
public:
    R15UnboundTraderCtx* ctx = nullptr;
    TraderAdapter* replacement = nullptr;
    TraderAdapter* restore = nullptr;
    unsigned calls = 0;
    bool rebind_in_custom = false;
    unsigned rebind_on_call = 1;

    const char* getRuleTag(const char*) override
    {
        ++calls;
        if (rebind_in_custom)
            return "HOT";
        if (rebind_on_call != 0 && calls != rebind_on_call)
            return "";
        ctx->setTrader(replacement);
        if (restore)
            ctx->setTrader(restore);
        return "";
    }

    const char* getCustomRawCode(const char*, const char*, uint32_t) override
    {
        ++calls;
        ctx->setTrader(replacement);
        return "ag2512";
    }
};

class R15RebindingDataMgr : public BaseDataMgr
{
public:
    BaseDataMgr* delegate = nullptr;
    R15UnboundTraderCtx* ctx = nullptr;
    bool rebind_on_commodity = false;
    unsigned calls = 0;

    WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t date = 0) override
    {
        WTSContractInfo* result = delegate->getContract(code, exchg, date);
        ++calls;
        ctx->setTrader(nullptr);
        return result;
    }

    WTSCommodityInfo* getCommodity(const char* exchg, const char* product) override
    {
        WTSCommodityInfo* result = delegate->getCommodity(exchg, product);
        if (rebind_on_commodity)
        {
            ++calls;
            ctx->setTrader(nullptr);
        }
        return result;
    }
};

class R15StableHotMgr : public HotMgr
{
public:
    unsigned rule_calls = 0;
    unsigned custom_calls = 0;
    const char* getRuleTag(const char*) override { ++rule_calls; return "HOT"; }
    const char* getCustomRawCode(const char*, const char*, uint32_t) override
    { ++custom_calls; return "ag2512"; }
};

/** 策略替身：逐回调计数 + 记录关键入参 */
class R15SpyStra : public HftStrategy
{
public:
    R15SpyStra() : HftStrategy("r15spy") {}

    virtual const char* getName() override { return "r15spy"; }
    virtual const char* getFactName() override { return "r15fact"; }

    virtual void on_init(IHftStraCtx* ctx) override { ++n_init; last_ctx = ctx; }
    virtual void on_session_begin(IHftStraCtx* ctx, uint32_t tdate) override
    { ++n_begin; last_tdate = tdate; last_ctx = ctx; }
    virtual void on_session_end(IHftStraCtx* ctx, uint32_t tdate) override
    { ++n_end; last_end_tdate = tdate; last_ctx = ctx; }
    virtual void on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* tick) override
    { ++n_tick; last_code = code; last_tick = tick; last_ctx = ctx; }
    virtual void on_bar(IHftStraCtx* ctx, const char* code, const char* period,
        uint32_t times, WTSBarStruct* bar) override
    { ++n_bar; last_code = code; last_period = std::string(period) + std::to_string(times);
      last_bar = bar; last_ctx = ctx; }
    virtual void on_order_queue(IHftStraCtx* ctx, const char* code, WTSOrdQueData* q) override
    { ++n_que; last_code = code; last_que = q; last_ctx = ctx; }
    virtual void on_order_detail(IHftStraCtx* ctx, const char* code, WTSOrdDtlData* d) override
    { ++n_dtl; last_code = code; last_dtl = d; last_ctx = ctx; }
    virtual void on_transaction(IHftStraCtx* ctx, const char* code, WTSTransData* t) override
    { ++n_trans; last_code = code; last_trans = t; last_ctx = ctx; }
    virtual void on_trade(IHftStraCtx* ctx, uint32_t localid, const char* code, bool isBuy,
        double vol, double price, const char* userTag) override
    { ++n_trade; last_localid = localid; last_code = code; last_buy = isBuy;
      last_vol = vol; last_px = price; last_tag = userTag; last_ctx = ctx; }
    virtual void on_order(IHftStraCtx* ctx, uint32_t localid, const char* code, bool isBuy,
        double totalQty, double leftQty, double price, bool isCanceled, const char* userTag) override
    { ++n_order; last_localid = localid; last_code = code; last_buy = isBuy;
      last_total = totalQty; last_left = leftQty; last_px = price;
      last_cancel = isCanceled; last_tag = userTag; last_ctx = ctx; }
    virtual void on_position(IHftStraCtx* ctx, const char* code, bool isLong, double pv,
        double pa, double nv, double na) override
    { ++n_pos; last_code = code; last_long = isLong; last_prevol = pv; last_preavail = pa;
      last_newvol = nv; last_newavail = na; last_ctx = ctx; }
    virtual void on_channel_ready(IHftStraCtx* ctx) override { ++n_ready; last_ctx = ctx; }
    virtual void on_channel_lost(IHftStraCtx* ctx) override { ++n_lost; last_ctx = ctx; }
    virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message,
        const char* userTag) override
    { ++n_entrust; last_localid = localid; last_success = bSuccess;
      last_msg = message; last_tag = userTag; }

    uint32_t n_init = 0, n_begin = 0, n_end = 0, n_tick = 0, n_bar = 0;
    uint32_t n_que = 0, n_dtl = 0, n_trans = 0, n_trade = 0, n_order = 0;
    uint32_t n_pos = 0, n_ready = 0, n_lost = 0, n_entrust = 0;
    IHftStraCtx* last_ctx = NULL;
    std::string last_code, last_period, last_tag, last_msg;
    WTSTickData* last_tick = NULL;
    WTSBarStruct* last_bar = NULL;
    WTSOrdQueData* last_que = NULL;
    WTSOrdDtlData* last_dtl = NULL;
    WTSTransData* last_trans = NULL;
    uint32_t last_localid = 0, last_tdate = 0, last_end_tdate = 0;
    double last_vol = 0, last_px = 0, last_total = 0, last_left = 0;
    double last_prevol = 0, last_preavail = 0, last_newvol = 0, last_newavail = 0;
    bool last_buy = false, last_cancel = false, last_long = false, last_success = false;
};

std::string slurp(const std::string& path)
{
    std::ifstream in(path.c_str());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class R15CtxTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r15hftctx"));
        _bdm.regisSession("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _eng.attach(&_bdm, &_hot, &_dtmgr);
    }
    void TearDown() override { _env.reset(); }

    std::string outDir(const char* name)
    {
        std::filesystem::path p(WtHelper::getOutputDir());
        p /= name;
        return p.string();
    }

    std::string udFile(const char* name)
    {
        std::filesystem::path p(WtHelper::getStraUsrDatDir());
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        p /= std::string("ud_") + name + ".json";
        return p.string();
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    HotMgr _hot;
    WtDtMgr _dtmgr;  // 空数据管理器使 get_cur_price 的 fallback 有效并确定返回 0。
    R15HftEngine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

} // namespace

/* ---------------- HftStraContext.h 的 set/get（inline） ---------------- */

TEST_F(R15CtxTest, strategy_pointer_roundtrip)
{
    R15HftCtx ctx(&_eng, "c15_setget", false, 0);
    EXPECT_EQ(ctx.get_stragety(), nullptr);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    EXPECT_EQ(ctx.get_stragety(), &stra);
    // getName/getFactName/id 是替身自己的常量，只作装配校验
    EXPECT_STREQ(ctx.get_stragety()->getName(), "r15spy");
    EXPECT_STREQ(ctx.get_stragety()->getFactName(), "r15fact");
    EXPECT_STREQ(ctx.get_stragety()->id(), "r15spy");
}

/* ---------------- 空策略指针：所有事件只走基类，不崩 ---------------- */

TEST_F(R15CtxTest, null_strategy_events_only_touch_base)
{
    R15HftCtx ctx(&_eng, "c15_null", true, 0);
    ctx.on_init();                       // 基类 init_outputs：agent=true 也建 trades.csv
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(outDir("c15_null") + "/trades.csv")));

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93145, 7200.0);
    ASSERT_NE(tk, nullptr);
    // _strategy==NULL ⇒ 每个事件只走基类；基类主体只是 _ud_modified 时落 ud 文件
    ctx.on_tick(kAg, tk);
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    ctx.on_bar(kAg, "m", 1, &bar);
    ctx.on_session_begin(20250110);
    ctx.on_order(9u, kAg, true, 2, 2, 7200.0, false);
    ctx.on_channel_ready();
    ctx.on_channel_lost();
    ctx.on_entrust(9u, kAg, true, "ok");
    ctx.on_position(kAg, true, 0, 0, 1, 1, 20250110);
    tk->release();
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(udFile("c15_null"))));

    ctx.stra_save_user_data("nk", "nv");  // 置脏但不写
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(udFile("c15_null"))));
    ctx.on_tick(kAg, NULL);               // 基类回调把脏数据刷下去
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(udFile("c15_null"))));
    const std::string once = slurp(udFile("c15_null"));
    EXPECT_NE(once.find("nv"), std::string::npos);

    ctx.on_session_end(20250110);         // agent=true ⇒ 落 funds.csv
    EXPECT_NE(slurp(outDir("c15_null") + "/funds.csv").find("20250110,0.00,0.00,0.00,0.00"),
        std::string::npos);
    // 标记已清 ⇒ 再回调不重复写 ud
    ctx.on_channel_ready();
    EXPECT_EQ(slurp(udFile("c15_null")).size(), once.size());
}

/* ---------------- on_init / 会话开始结束转发 ---------------- */

TEST_F(R15CtxTest, on_init_and_session_callbacks_forward_to_strategy)
{
    R15HftCtx ctx(&_eng, "c15_init", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);

    ctx.on_init();
    EXPECT_EQ(stra.n_init, 1u);
    EXPECT_EQ(stra.last_ctx, static_cast<IHftStraCtx*>(&ctx));   // this 指针原样转发

    ctx.on_session_begin(20250110);
    EXPECT_EQ(stra.n_begin, 1u);
    EXPECT_EQ(stra.last_tdate, 20250110u);

    ctx.on_session_end(20250111);
    EXPECT_EQ(stra.n_end, 1u);
    EXPECT_EQ(stra.last_end_tdate, 20250111u);
}

TEST_F(R15CtxTest, session_end_forwards_date_then_base_writes_fund_line)
{
    // agent=true：基类 on_session_end 把 _fund_info 落一行 funds.csv
    R15HftCtx ctx(&_eng, "c15_ord", true, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();
    ctx.on_session_end(20250110);

    EXPECT_EQ(stra.n_end, 1u);
    EXPECT_EQ(stra.last_end_tdate, 20250110u);
    const std::string funds = outDir("c15_ord") + "/funds.csv";
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(funds)));
    // 策略没有交易 ⇒ 落地行数值确定：表头 + 20250110 全 0
    EXPECT_NE(slurp(funds).find("20250110,0.00,0.00,0.00,0.00"), std::string::npos);
}

/* ---------------- on_tick：订阅决定转发与否 ---------------- */

TEST_F(R15CtxTest, on_tick_without_subscription_skips_strategy)
{
    R15HftCtx ctx(&_eng, "c15_t1", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93145, 7150.0);
    ASSERT_NE(tk, nullptr);
    ctx.on_tick(kAg, tk);
    EXPECT_EQ(stra.n_tick, 0u);                 // _tick_subs 为空 ⇒ 不转发
    // 基类 on_tick 不记价（只刷脏用户数据）⇒ 价格仍回退到引擎的 0
    EXPECT_DOUBLE_EQ(ctx.stra_get_price(kAg), 0.0);
    tk->release();
}

TEST_F(R15CtxTest, on_tick_subscribed_forwards_same_pointer)
{
    R15HftCtx ctx(&_eng, "c15_t2", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();
    ctx.sub_tick_direct(kAg);

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93146, 7160.0);
    ASSERT_NE(tk, nullptr);
    ctx.on_tick(kAg, tk);
    EXPECT_EQ(stra.n_tick, 1u);
    EXPECT_STREQ(stra.last_code.c_str(), kAg);
    EXPECT_EQ(stra.last_tick, tk);              // ctx 级转发不做复权拷贝，原样传指针
    EXPECT_EQ(stra.last_ctx, static_cast<IHftStraCtx*>(&ctx));
    tk->release();

    // 别的合约没订阅 ⇒ 计数不变
    WTSTickData* tk2 = makeTick("SHFE", "ag2512", 20250110, 20250110, 93147, 7170.0);
    ASSERT_NE(tk2, nullptr);
    ctx.on_tick("SSE.600000", tk2);
    EXPECT_EQ(stra.n_tick, 1u);
    tk2->release();
}

/* ---------------- on_bar / 三档盘口数据转发 ---------------- */

TEST_F(R15CtxTest, on_bar_forwards_code_period_times_and_bar)
{
    R15HftCtx ctx(&_eng, "c15_bar", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 7210.0;
    bar.open = 7200.0;
    ctx.on_bar(kAg, "m", 5, &bar);
    EXPECT_EQ(stra.n_bar, 1u);
    EXPECT_STREQ(stra.last_code.c_str(), kAg);
    EXPECT_EQ(stra.last_period, "m5");          // 替身里拼 period+times 便于断言
    EXPECT_EQ(stra.last_bar, &bar);
}

TEST_F(R15CtxTest, ordque_orddtl_trans_forward_when_strategy_set)
{
    R15HftCtx ctx(&_eng, "c15_dtl", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    WTSOrdQueStruct que;
    memset(&que, 0, sizeof(que));
    wt_strcpy(que.exchg, "SHFE");
    wt_strcpy(que.code, "ag2512");
    que.action_date = 20250110;
    que.action_time = 93145000;
    WTSOrdQueData* qd = WTSOrdQueData::create(que);
    ASSERT_NE(qd, nullptr);
    ctx.on_order_queue(kAg, qd);
    EXPECT_EQ(stra.n_que, 1u);
    EXPECT_EQ(stra.last_que, qd);
    EXPECT_STREQ(stra.last_code.c_str(), kAg);

    WTSOrdDtlStruct dtl;
    memset(&dtl, 0, sizeof(dtl));
    wt_strcpy(dtl.exchg, "SHFE");
    wt_strcpy(dtl.code, "ag2512");
    WTSOrdDtlData* dd = WTSOrdDtlData::create(dtl);
    ASSERT_NE(dd, nullptr);
    ctx.on_order_detail(kAg, dd);
    EXPECT_EQ(stra.n_dtl, 1u);
    EXPECT_EQ(stra.last_dtl, dd);

    WTSTransStruct tr;
    memset(&tr, 0, sizeof(tr));
    wt_strcpy(tr.exchg, "SHFE");
    wt_strcpy(tr.code, "ag2512");
    WTSTransData* td = WTSTransData::create(tr);
    ASSERT_NE(td, nullptr);
    ctx.on_transaction(kAg, td);
    EXPECT_EQ(stra.n_trans, 1u);
    EXPECT_EQ(stra.last_trans, td);

    // _strategy 摘掉后同样的调用不再转发，计数冻结
    ctx.set_strategy(NULL);
    ctx.on_order_queue(kAg, qd);
    ctx.on_order_detail(kAg, dd);
    ctx.on_transaction(kAg, td);
    EXPECT_EQ(stra.n_que, 1u);
    EXPECT_EQ(stra.n_dtl, 1u);
    EXPECT_EQ(stra.n_trans, 1u);

    qd->release();
    dd->release();
    td->release();
}

/* ---------------- 成交/委托回报：内部代码 + 订单标签 ---------------- */

TEST_F(R15CtxTest, on_trade_forwards_inner_code_and_order_tag)
{
    R15HftCtx ctx(&_eng, "c15_trd", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    // 未登记标签 ⇒ getOrderTag(701) 落在空表，返回 ""
    ctx.on_trade(701u, kAg, true, 1, 7100.0);
    EXPECT_EQ(stra.n_trade, 1u);
    EXPECT_EQ(stra.last_localid, 701u);
    EXPECT_STREQ(stra.last_code.c_str(), kAg);  // _code_map 为空 ⇒ innerCode==stdCode
    EXPECT_STREQ(stra.last_tag.c_str(), "");
    EXPECT_DOUBLE_EQ(stra.last_px, 7100.0);
    EXPECT_DOUBLE_EQ(stra.last_vol, 1.0);
    EXPECT_TRUE(stra.last_buy);

    // 基类账本（_data_agent=false）：701 买 1 手 @7100、702 卖 1 手 ⇒ FIFO 全平。
    // 独立的标签用例覆盖 setUserTag；默认零容量问题见 BUG-R15-001。
    ctx.on_trade(702u, kAg, false, 1, 7110.0);
    EXPECT_EQ(stra.n_trade, 2u);
    EXPECT_STREQ(stra.last_tag.c_str(), "");        // 空标签链的真实可运行子集
    EXPECT_FALSE(stra.last_buy);

    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 0.0);
}

TEST_F(R15CtxTest, on_order_and_entrust_forward_tag_and_cancel_flag)
{
    R15HftCtx ctx(&_eng, "c15_ord2", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    // 缺少登记时返回空串。
    ctx.on_order(801u, kAg, true, 2, 0, 7100.0, true);
    EXPECT_EQ(stra.n_order, 1u);
    EXPECT_EQ(stra.last_localid, 801u);
    EXPECT_STREQ(stra.last_tag.c_str(), "");
    EXPECT_DOUBLE_EQ(stra.last_total, 2.0);
    EXPECT_DOUBLE_EQ(stra.last_left, 0.0);
    EXPECT_TRUE(stra.last_cancel);
    // 基类 on_order 不动账本 ⇒ 没有任何成交进过 _pos_map
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 0.0);

    ctx.on_entrust(801u, kAg, false, "rejected");
    EXPECT_EQ(stra.n_entrust, 1u);
    EXPECT_EQ(stra.last_localid, 801u);
    EXPECT_FALSE(stra.last_success);
    EXPECT_EQ(stra.last_msg, "rejected");
    EXPECT_STREQ(stra.last_tag.c_str(), "");
}

TEST_F(R15CtxTest, on_position_forwards_only_to_strategy)
{
    R15HftCtx ctx(&_eng, "c15_pos", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    ctx.on_position(kAg, true, 5, 3, 7, 6, 20250110);
    EXPECT_EQ(stra.n_pos, 1u);
    EXPECT_STREQ(stra.last_code.c_str(), kAg);
    EXPECT_TRUE(stra.last_long);
    EXPECT_DOUBLE_EQ(stra.last_prevol, 5.0);
    EXPECT_DOUBLE_EQ(stra.last_preavail, 3.0);
    EXPECT_DOUBLE_EQ(stra.last_newvol, 7.0);
    EXPECT_DOUBLE_EQ(stra.last_newavail, 6.0);
    // HftStraContext::on_position 不调基类 ⇒ ctx 自己的账本保持为空
    EXPECT_DOUBLE_EQ(ctx.stra_get_position_avgpx(kAg), 0.0);
}

TEST_F(R15CtxTest, channel_events_forward_and_flush_userdata_only_when_dirty)
{
    R15HftCtx ctx(&_eng, "c15_chn", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();

    ctx.on_channel_ready();
    ctx.on_channel_lost();
    EXPECT_EQ(stra.n_ready, 1u);
    EXPECT_EQ(stra.n_lost, 1u);
    EXPECT_EQ(stra.last_ctx, static_cast<IHftStraCtx*>(&ctx));

    // 无用户数据改动 ⇒ ready/lost 的基类分支不写 ud 文件
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(udFile("c15_chn"))));

    ctx.stra_save_user_data("k1", "v1");
    ctx.on_channel_ready();
    EXPECT_EQ(stra.n_ready, 2u);
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(udFile("c15_chn"))));
    EXPECT_NE(slurp(udFile("c15_chn")).find("v1"), std::string::npos);
}

/* ---------------- HftStraBaseCtx.h 的 inline 订单标签链 ---------------- */

TEST_F(R15CtxTest, order_tag_default_capacity_preserves_registered_tag)
{
    R15HftCtx ctx(&_eng, "c15_tag", false, 0);

    // 默认构造上下文也必须保存已登记的订单标签。
    ctx.setUserTag(1, "AAA");
    EXPECT_STREQ(ctx.getOrderTag(1), "AAA");
}

TEST_F(R15CtxTest, order_tag_set_get_erase_with_default_context)
{
    R15HftCtx ctx(&_eng, "c15_tag_capacity", false, 0);

    EXPECT_STREQ(ctx.getOrderTag(1), "");
    ctx.eraseOrderTag(1);
    EXPECT_STREQ(ctx.getOrderTag(1), "");

    ctx.setUserTag(1, "AAA");
    ctx.setUserTag(2, "BBB");
    ctx.setUserTag(3, "CCC");
    EXPECT_STREQ(ctx.getOrderTag(1), "AAA");
    EXPECT_STREQ(ctx.getOrderTag(3), "CCC");

    ctx.eraseOrderTag(2);
    EXPECT_STREQ(ctx.getOrderTag(1), "AAA");
    EXPECT_STREQ(ctx.getOrderTag(3), "CCC");
    EXPECT_STREQ(ctx.getOrderTag(9), "");
}

TEST_F(R15CtxTest, order_tag_lookup_miss_does_not_match_or_erase_neighbor)
{
    // 契约按精确 localid 匹配：缺失的中间 id 查询为空，删除缺失 id 不影响后继标签。
    R15HftCtx ctx(&_eng, "c15_tagmiss", false, 0);
    ctx.setUserTag(1, "AAA");
    ctx.setUserTag(5, "EEE");
    EXPECT_STREQ(ctx.getOrderTag(3), "");
    ctx.eraseOrderTag(2);
    EXPECT_STREQ(ctx.getOrderTag(5), "EEE");
    EXPECT_STREQ(ctx.getOrderTag(1), "AAA");
}

TEST_F(R15CtxTest, order_tags_keep_unordered_ids_and_replace_exact_duplicate)
{
    R15HftCtx ctx(&_eng, "c15_tag_order", false, 0);
    ctx.setUserTag(50, "fifty");
    ctx.setUserTag(10, "ten");
    ctx.setUserTag(30, "thirty");
    EXPECT_STREQ(ctx.getOrderTag(10), "ten");
    EXPECT_STREQ(ctx.getOrderTag(30), "thirty");
    EXPECT_STREQ(ctx.getOrderTag(50), "fifty");
    EXPECT_STREQ(ctx.getOrderTag(20), "");
    ctx.eraseOrderTag(20);
    EXPECT_STREQ(ctx.getOrderTag(30), "thirty");
    ctx.setUserTag(30, "updated");
    EXPECT_STREQ(ctx.getOrderTag(30), "updated");
    ctx.eraseOrderTag(30);
    EXPECT_STREQ(ctx.getOrderTag(30), "");
    EXPECT_STREQ(ctx.getOrderTag(10), "ten");
    EXPECT_STREQ(ctx.getOrderTag(50), "fifty");
}

TEST_F(R15CtxTest, order_tags_do_not_silently_evict_or_truncate)
{
    R15HftCtx ctx(&_eng, "c15_tag_many", false, 0);
    const std::string longTag(256, 'T');
    ctx.setUserTag(1, longTag.c_str());
    for (uint32_t id = 2; id <= 2048; ++id)
        ctx.setUserTag(id, "other");
    EXPECT_STREQ(ctx.getOrderTag(1), longTag.c_str());
    EXPECT_STREQ(ctx.getOrderTag(2048), "other");
    ctx.setUserTag(2049, nullptr);
    EXPECT_STREQ(ctx.getOrderTag(2049), "");
}

TEST_F(R15CtxTest, registered_order_tag_reaches_callbacks_and_remains_after_terminal_order)
{
    R15HftCtx ctx(&_eng, "c15_tag_callback", false, 0);
    R15SpyStra stra;
    ctx.set_strategy(&stra);
    ctx.on_init();
    ctx.setUserTag(17, "trade-tag");
    ctx.setUserTag(19, "other-tag");

    ctx.on_order(17, kAg, true, 2, 0, 7100.0, true);
    EXPECT_EQ(stra.n_order, 1u);
    EXPECT_STREQ(stra.last_tag.c_str(), "trade-tag");
    // Preserve the existing behavior: on_order does not decide when late
    // trades are impossible and therefore does not erase the registered tag.
    ctx.on_trade(17, kAg, true, 1, 7100.0);
    EXPECT_EQ(stra.n_trade, 1u);
    EXPECT_STREQ(stra.last_tag.c_str(), "trade-tag");
    EXPECT_STREQ(ctx.getOrderTag(19), "other-tag");
}

TEST_F(R15CtxTest, order_tags_survive_open_close_and_reverse_without_truncation)
{
    for (const size_t length : {size_t(31), size_t(32), size_t(256)})
    {
        const std::string name = "c15_tag_roundtrip_" + std::to_string(length);
        const std::string openTag(length, 'A');
        const std::string reverseTag(length, 'B');
        const std::string finalTag(length, 'C');
        {
            R15HftCtx ctx(&_eng, name.c_str(), true, 0);
            R15SpyStra stra;
            ctx.set_strategy(&stra);
            ctx.on_init();
            ctx.disable_signal_log_for_test();
            ctx.setUserTag(10, openTag.c_str());
            ctx.setUserTag(20, reverseTag.c_str());
            ctx.setUserTag(30, finalTag.c_str());
            ctx.on_trade(10, kAg, true, 1, 7100.0);
            EXPECT_STREQ(stra.last_tag.c_str(), openTag.c_str());
            ctx.on_trade(20, kAg, false, 2, 7110.0);
            EXPECT_STREQ(stra.last_tag.c_str(), reverseTag.c_str());
            ctx.on_trade(30, kAg, true, 1, 7105.0);
            EXPECT_STREQ(stra.last_tag.c_str(), finalTag.c_str());
        }
        const std::string closes = slurp(outDir(name.c_str()) + "/closes.csv");
        EXPECT_NE(closes.find("," + openTag + "," + reverseTag + "\n"), std::string::npos);
        EXPECT_NE(closes.find("," + reverseTag + "," + finalTag + "\n"), std::string::npos);
    }
}

TEST_F(R15CtxTest, unbound_trader_actions_fail_before_engine_or_mapping_side_effects)
{
    R15UnboundTraderCtx ctx(nullptr);
    // Also gives the original implementation a deterministic null red control.
    ctx.force_unbound_for_test();
    ctx.seed_code_mapping();
    ctx.setUserTag(77, "existing-tag");
    EXPECT_FALSE(ctx.stra_cancel(77));
    EXPECT_TRUE(ctx.stra_cancel(kAg, true, 1).empty());
    EXPECT_TRUE(ctx.stra_buy(kAg, 1, 1, "tag").empty());
    EXPECT_TRUE(ctx.stra_sell(kAg, 1, 1, "tag").empty());
    EXPECT_EQ(ctx.stra_enter_long(kAg, 1, 1, "tag"), 0u);
    EXPECT_EQ(ctx.stra_exit_long(kAg, 1, 1, "tag"), 0u);
    EXPECT_EQ(ctx.stra_enter_short(kAg, 1, 1, "tag"), 0u);
    EXPECT_EQ(ctx.stra_exit_short(kAg, 1, 1, "tag"), 0u);
    EXPECT_EQ(ctx.errors, 8u);
    EXPECT_NE(ctx.last_error.find("trader adapter is not bound"), std::string::npos);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    EXPECT_STREQ(ctx.getOrderTag(77), "existing-tag");
}

TEST_F(R15CtxTest, fresh_context_starts_without_a_trader)
{
    R15UnboundTraderCtx ctx(nullptr);
    EXPECT_FALSE(ctx.has_trader());
}

TEST_F(R15CtxTest, unbound_trader_reentrant_diagnostic_cannot_submit_actions)
{
    R15UnboundTraderCtx ctx(nullptr);
    ctx.force_unbound_for_test();
    ctx.reenter = true;
    EXPECT_TRUE(ctx.stra_buy(kAg, 1, 1, "tag").empty());
    EXPECT_FALSE(ctx.nested_cancel);
    EXPECT_EQ(ctx.errors, 2u);
    EXPECT_FALSE(ctx.has_trader());
}

TEST_F(R15CtxTest, bound_trader_still_handles_queries_and_unknown_cancel)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    ctx.setTrader(&trader);
    EXPECT_TRUE(ctx.has_trader());
    EXPECT_DOUBLE_EQ(ctx.stra_get_undone(kAg), 0.0);
    EXPECT_FALSE(ctx.stra_cancel(4321));
    EXPECT_EQ(ctx.errors, 0u);
}

TEST_F(R15CtxTest, bound_trader_sends_order_only_to_fake_api)
{
    const std::string id = "r15bind" + std::to_string(static_cast<long>(::getpid()));
    const std::filesystem::path output = std::filesystem::path(WtHelper::getBaseDir()) / "traders" / id;
    {
        p11fix::FakeTraderApi api;
        TraderAdapter trader;
        _ct->setAltCode("ag2512");
        ASSERT_TRUE(trader.initExt(id.c_str(), &api, &_bdm, nullptr));
        R15UnboundTraderCtx ctx(&_eng);
        ctx.setTrader(&trader);
        const uint32_t order = ctx.stra_enter_long(kAg, 7100.0, 1.0, "tag");
        EXPECT_NE(order, 0u);
        EXPECT_NE(order, UINT_MAX);
        EXPECT_EQ(api.n_insert_, 1u);
        EXPECT_EQ(api.last_code_, "ag2512");
        EXPECT_DOUBLE_EQ(api.last_price_, 7100.0);
        EXPECT_DOUBLE_EQ(api.last_vol_, 1.0);
        EXPECT_EQ(ctx.errors, 0u);
    }
    std::error_code ec;
    std::filesystem::remove_all(output, ec);
    EXPECT_FALSE(ec);
}

TEST_F(R15CtxTest, unbinding_a_previously_bound_trader_rejects_actions)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    ctx.setTrader(&trader);
    ctx.setTrader(nullptr);
    EXPECT_FALSE(ctx.stra_cancel(4321));
    EXPECT_EQ(ctx.errors, 1u);
}

TEST_F(R15CtxTest, reentrant_bind_during_unbound_diagnostic_does_not_submit_outer_order)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(nullptr);
    ctx.force_unbound_for_test();
    ctx.bind_on_error = &trader;
    // A bind from the diagnostic must not resume the already rejected order.
    EXPECT_TRUE(ctx.stra_buy(kAg, 1, 1, "tag").empty());
    EXPECT_TRUE(ctx.has_trader());
    EXPECT_EQ(ctx.errors, 1u);
}

TEST_F(R15CtxTest, trader_unbound_during_rule_callback_rejects_outer_order)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingHotMgr hot;
    hot.ctx = &ctx;
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_EQ(ctx.stra_enter_long("SHFE.ag.CUSTOM", 1, 1, "tag"), 0u);
    EXPECT_EQ(hot.calls, 1u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, trader_replaced_during_rule_callback_rejects_outer_order)
{
    TraderAdapter trader;
    TraderAdapter replacement;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingHotMgr hot;
    hot.ctx = &ctx;
    hot.replacement = &replacement;
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_EQ(ctx.stra_enter_long("SHFE.ag.CUSTOM", 1, 1, "tag"), 0u);
    EXPECT_EQ(hot.calls, 1u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, trader_unbound_then_rebound_during_rule_callback_rejects_outer_order)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingHotMgr hot;
    hot.ctx = &ctx;
    hot.restore = &trader;
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_EQ(ctx.stra_enter_long("SHFE.ag.CUSTOM", 1, 1, "tag"), 0u);
    EXPECT_EQ(hot.calls, 1u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.has_trader());
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, each_entry_and_exit_rejects_reentrant_unbind_from_rule_lookup)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingHotMgr hot;
    hot.ctx = &ctx;
    hot.rebind_on_call = 0; // Unbind during every call in this multi-action test.
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.seed_code_mapping();
    const char* code = "SHFE.ag.CUSTOM";
    ctx.setTrader(&trader);
    EXPECT_TRUE(ctx.stra_buy(code, 1, 1, "tag").empty());
    ctx.setTrader(&trader);
    EXPECT_TRUE(ctx.stra_sell(code, 1, 1, "tag").empty());
    ctx.setTrader(&trader);
    EXPECT_EQ(ctx.stra_enter_long(code, 1, 1, "tag"), 0u);
    ctx.setTrader(&trader);
    EXPECT_EQ(ctx.stra_exit_long(code, 1, 1, "tag"), 0u);
    ctx.setTrader(&trader);
    EXPECT_EQ(ctx.stra_enter_short(code, 1, 1, "tag"), 0u);
    ctx.setTrader(&trader);
    EXPECT_EQ(ctx.stra_exit_short(code, 1, 1, "tag"), 0u);
    EXPECT_EQ(hot.calls, 6u);
    EXPECT_EQ(ctx.errors, 6u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, trader_unbound_during_custom_code_lookup_rejects_outer_order)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingHotMgr hot;
    hot.ctx = &ctx;
    hot.rebind_in_custom = true;
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_EQ(ctx.stra_enter_long("SHFE.ag.CUSTOM", 1, 1, "tag"), 0u);
    EXPECT_EQ(hot.calls, 2u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, trader_unbound_during_contract_lookup_rejects_outer_buy)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingDataMgr data;
    data.delegate = &_bdm;
    data.ctx = &ctx;
    _eng.attach(&data, &_hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_TRUE(ctx.stra_buy(kAg, 1, 1, "tag").empty());
    EXPECT_EQ(data.calls, 1u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, trader_unbound_during_commodity_lookup_rejects_outer_sell)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingDataMgr data;
    data.delegate = &_bdm;
    data.ctx = &ctx;
    data.rebind_on_commodity = true;
    _eng.attach(&data, &_hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_TRUE(ctx.stra_sell(kAg, 1, 1, "tag").empty());
    EXPECT_EQ(data.calls, 1u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, trader_unbound_during_sell_position_lookup_rejects_outer_sell)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15RebindingHotMgr hot;
    hot.ctx = &ctx;
    hot.rebind_on_call = 2; // First lookup belongs to sell, second to its position query.
    _comm->setTradingMode(TM_Long);
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.setTrader(&trader);
    ctx.seed_code_mapping();
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.CUSTOM", 1, 1, "tag").empty());
    EXPECT_EQ(hot.calls, 2u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_TRUE(ctx.original_code_mapping_unchanged());
    _comm->setTradingMode(TM_Both);
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}

TEST_F(R15CtxTest, bound_long_only_sell_uses_existing_position_semantics)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    ctx.setTrader(&trader);
    _comm->setTradingMode(TM_Long);
    EXPECT_DOUBLE_EQ(trader.getPosition(kAg, true, 3), 0.0);
    EXPECT_TRUE(ctx.stra_sell(kAg, 1, 1, "tag").empty());
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_NE(ctx.last_error.find("No enough position"), std::string::npos);
    _comm->setTradingMode(TM_Both);
}

TEST_F(R15CtxTest, bound_long_only_sell_keeps_custom_position_mapping)
{
    TraderAdapter trader;
    R15UnboundTraderCtx ctx(&_eng);
    R15StableHotMgr hot;
    _comm->setTradingMode(TM_Long);
    _eng.attach(&_bdm, &hot, &_dtmgr);
    ctx.setTrader(&trader);
    EXPECT_DOUBLE_EQ(trader.getPosition("SHFE.ag.2512", true, 3), 0.0);
    EXPECT_TRUE(ctx.stra_sell("SHFE.ag.CUSTOM", 1, 1, "tag").empty());
    EXPECT_EQ(hot.rule_calls, 2u);
    EXPECT_EQ(hot.custom_calls, 1u);
    EXPECT_EQ(ctx.errors, 1u);
    EXPECT_NE(ctx.last_error.find("No enough position"), std::string::npos);
    EXPECT_STREQ(ctx.get_inner_code("SHFE.ag.2512"), "SHFE.ag.CUSTOM");
    _comm->setTradingMode(TM_Both);
    _eng.attach(&_bdm, &_hot, &_dtmgr);
}
