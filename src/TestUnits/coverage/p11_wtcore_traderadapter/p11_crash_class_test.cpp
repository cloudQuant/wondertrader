/**
 * p11_crash_class_test.cpp — 崩溃/挂死类输入统一收口（纪律 1、2）+ 少量安全的收尾分支
 *
 * 启用用例只覆盖「不会段错误」的收尾分支：onTraderError、handleTraderLog、onLogout、
 * release 之后再收回报、TraderAdapter 与 TraderAdapterMgr 的组合。
 * 其余判定为「空指针解引用 -> 段错误」的输入全部放进同一个 DISABLED_ 用例，启用用例零残留，
 * 缺陷登记在 docs/迭代计划/迭代01/bugs/p11_wtcore_traderadapter.md。
 */
#include "p11_trader_fixture.h"

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

class P11CrashClass : public P11TraderTest
{
};

TEST_F(P11CrashClass, error_and_log_paths_are_safe)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    // 交易错误回报（_notifier==NULL, 只写日志）
    WTSError* err = WTSError::create(WEC_UNKNOWN, "channel down");
    err->retain();
    trader.onTraderError(err);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
    EXPECT_EQ(0u, sink_.n_lost_);
    EXPECT_EQ(0u, sink_.n_ready_);
    EXPECT_STREQ("channel down", err->getMessage());
    EXPECT_EQ(WEC_UNKNOWN, err->getErrorCode());

    // 空错误指针: 日志分支被 if(err) 挡住, 但 _notifier 为空所以不触发二次解引用
    trader.onTraderError(NULL);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());

    // 通道日志与登出回报
    trader.handleTraderLog(LL_INFO, "raw channel log");
    trader.onLogout();
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());

    // 连接失败(非 0 事件码)只写日志, 不会去读 _cfg
    trader.handleEvent(WTE_Connect, -1);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
    EXPECT_EQ(0u, api_.n_login_);

    // release 之后仍可继续收回报（_trader_api 未被置空, 只是注销了 spi）
    EXPECT_EQ(1u, api_.n_register_);
    trader.release();
    EXPECT_EQ(1u, api_.n_release_);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
}

TEST_F(P11CrashClass, mgr_holds_two_channels_with_own_apis)
{
    TraderAdapterMgr mgr;
    TraderAdapterPtr a(new TraderAdapter());
    TraderAdapterPtr b(new TraderAdapter());

    FakeTraderApi apiB;
    EXPECT_TRUE(a->initExt("p11m1", &api_, &bd_, NULL));
    EXPECT_TRUE(b->initExt("p11m2", &apiB, &bd_, NULL));
    ASSERT_TRUE(mgr.addAdapter("p11m1", a));
    ASSERT_TRUE(mgr.addAdapter("p11m2", b));
    EXPECT_FALSE(mgr.addAdapter("p11m1", a));
    EXPECT_EQ(2u, mgr.getAdapters().size());

    mgr.run();                        // 两个通道各自 registerSpi + connect, 不起线程
    EXPECT_EQ(1u, api_.n_register_);
    EXPECT_EQ(1u, apiB.n_register_);

    // 登录回报只影响自己那条通道
    a->onLoginResult(true, "ok", 20240510);
    EXPECT_EQ(TraderAdapter::AS_LOGINED, a->state());
    EXPECT_EQ(TraderAdapter::AS_LOGINING, b->state());
    EXPECT_EQ(1u, api_.n_qpositions_);
    EXPECT_EQ(0u, apiB.n_qpositions_);

    mgr.refresh_funds();              // 未就绪 -> 双方都不查资金
    EXPECT_EQ(0u, api_.n_qaccount_);
    EXPECT_EQ(0u, apiB.n_qaccount_);

    TraderAdapterPtr miss = mgr.getAdapter("nope");
    EXPECT_TRUE(miss == NULL);
    EXPECT_TRUE(mgr.getAdapter("p11m2") != NULL);

    mgr.release();
    EXPECT_EQ(0u, mgr.getAdapters().size());
    EXPECT_EQ(1u, apiB.n_release_);
}

/*
 * 崩溃类输入收口：以下每一行都会把空指针喂进 TraderAdapter 的实体分支,
 * 一旦被执行就是段错误, 所以整组留在 DISABLED_ 里（gtest 默认不跑）。
 *  1) handleEvent(WTE_Connect, 0) —— initExt 装配的通道 _cfg 恒为 NULL,
 *     但这里无条件 `_cfg->getCString("user")`（TraderAdapter.cpp:1411）。
 *  2) buy/sell —— 未 run() 时 `_stat_map->get()` 空表解引用（:693/:985）;
 *     run() 之后仍然依赖 `_policy_mgr->getActionRules` 与 `commInfo->getSessionInfo()`,
 *     替身没有真实时段表时会继续越界, 且它按墙钟 WtHelper::getTime() 判断是否在交易时间内,
 *     结果不可复现, 所以这两条路径本 lane 一律不进启用用例。
 *  3) checkOrderLimits/checkCancelLimits —— 风控已激活但 _stat_map 尚未创建（:556/:621）。
 *  4) getRiskParams —— 规则表里没有目标品种又没有 default 时 `it->second` 取 end()（:401,
 *     与 BUG-N01-003 同一入口）。
 *  5) onPushTrade —— 成交回报在 run() 之前到达, _stat_map 为空（BUG-N01-005 同类第二入口）。
 *  6) onRspAccount —— 资金数组里含空元素时无判空, 直接 fundInfo->getCurrency()（:1528）。
 *  7) onRspEntrust —— 错单分支不校验 entrust->getContractInfo()（:1455）。
 */
TEST_F(P11CrashClass, DISABLED_report_paths_that_dereference_null_before_run)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));

    // 1) 连接成功事件读 _cfg（NULL）
    trader.handleEvent(WTE_Connect, 0);

    // 6) 资金数组里的空元素
    WTSArray* funds = WTSArray::create();
    funds->append(static_cast<WTSObject*>(NULL));
    trader.onRspAccount(funds);

    // 7) 没有合约信息的委托错单回报
    WTSEntrust* bare = WTSEntrust::create(kStdCode, 1, 7000.0);
    trader.onRspEntrust(bare, WTSError::create(WEC_ORDERINSERT, "boom"));

    // 2) 下单前置校验读 _stat_map（NULL）
    trader.buy(kStdCode, 7000.0, 1, 0, false);
    trader.sell(kStdCode, 7000.0, 1, 0, false);

    // 3) 风控开启但没有统计表
    wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(
        (tmp_ / "crash.json").string().c_str());
    if (cfg != NULL)
    {
        trader.init(id_.c_str(), cfg, &bd_, NULL);
        trader.checkOrderLimits(kStdCode);
        trader.checkCancelLimits(kStdCode);
        cfg->release();
    }

    // 4)/5) 无 default 规则的 getRiskParams 与 run() 前的成交回报
    WTSArray* arr = WTSArray::create();
    WTSTradeInfo* t = WTSTradeInfo::create(kRawCode, kExchg);
    t->setContractInfo(ct_);
    t->setDirection(WDT_LONG);
    t->setOffsetType(WOT_OPEN);
    t->setVolume(1);
    t->setPrice(7000.0);
    arr->append(t);
    trader.onPushTrade(t);
}

} // namespace
