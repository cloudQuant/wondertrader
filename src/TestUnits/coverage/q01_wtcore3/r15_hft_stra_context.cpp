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
#include <WtCore/WtHftEngine.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

static const char* kAg = "SHFE.ag.2512";

/** 裸高频引擎：只接替身基础数据，不 init/run/起线程（与 R10 同法） */
class R15HftEngine : public WtHftEngine
{
public:
    R15HftEngine() {}
    void attach(IBaseDataMgr* bd, IHotMgr* hot)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
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

    void sub_tick_direct(const char* stdCode) { _tick_subs.insert(stdCode); }
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
        _eng.attach(&_bdm, &_hot);
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

    // 默认构造上下文仍应保存已登记的订单标签；当前产品容器 capacity 为 0，
    // 正确契约断言会暴露标签未保存的问题（BUG-R15-001）。
    ctx.setUserTag(1, "AAA");
    EXPECT_STREQ(ctx.getOrderTag(1), "AAA");
}

TEST_F(R15CtxTest, DISABLED_order_tag_set_get_erase_with_initialized_capacity)
{
    R15HftCtx ctx(&_eng, "c15_tag_capacity", false, 0);

    // 完整标签链需要非零 _orders 容量；该成员为 private，当前没有安全初始化 seam。
    // 保留正确的回读、精确删除和非匹配查询断言，待生产提供 seam 后再启用。
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
    EXPECT_STREQ(ctx.getOrderTag(9), "");       // 大于所有 id ⇒ lower_bound 命中 end
}

TEST_F(R15CtxTest, DISABLED_order_tag_lookup_miss_does_not_match_or_erase_neighbor)
{
    // 契约按精确 localid 匹配：缺失的中间 id 查询为空，删除缺失 id 不影响后继标签。
    // _orders 是 private 且默认容量为零，当前测试没有安全 seam 构造非空标签集合。
    R15HftCtx ctx(&_eng, "c15_tagmiss", false, 0);
    ctx.setUserTag(1, "AAA");
    ctx.setUserTag(5, "EEE");
    EXPECT_STREQ(ctx.getOrderTag(3), "");
    ctx.eraseOrderTag(2);
    EXPECT_STREQ(ctx.getOrderTag(5), "EEE");
    EXPECT_STREQ(ctx.getOrderTag(1), "AAA");
}
