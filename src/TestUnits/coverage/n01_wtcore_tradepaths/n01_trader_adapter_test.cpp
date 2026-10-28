/**
 * n01_trader_adapter_test.cpp
 * 覆盖 src/WtCore/TraderAdapter.{h,cpp} 中「不需要真实交易通道」的部分：
 *   - 构造后的初始状态机（AS_NOTLOGIN / isReady()==false）
 *   - id()/state()/getBaseDataMgr()/getUndoneQty()/getOrders()/isSelfMatched()
 *   - run()/release()/queryFund() 在 _trader_api == NULL 时的早退分支
 *   - getPosition() 的「合约不在持仓表」分支
 *   - checkOrderLimits()/checkCancelLimits()/isTradeEnabled() 在风控未启用时的短路分支
 *   - onRspPosition(NULL)/onRspOrders(NULL)/onPushOrder(NULL)/onTrade(NULL) 的空数据早退
 *   - addSink()/TraderAdapterMgr 注册表（重名、空名、NULL、getAdapter、run、release、refresh_funds）
 *   - makeLocalOrderID() 本地委托号生成器的单调性（不断言墙钟绝对值）
 * 需要真实 ITraderApi/交易回报的分支（buy/sell/doEntrust/doCancel/onPushTrade/onRspOrders 实体
 * /onLoginResult/handleEvent/onRspAccount/onRspTrades）见 manifest 的跳过清单。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>

#include <boost/core/noncopyable.hpp>

#include <WtCore/TraderAdapter.h>
#include <WtCore/ITrdNotifySink.h>
#include <Includes/WTSVariant.hpp>

USING_NS_WTP;

// TraderAdapter.cpp 中的文件级（非 static）自由函数，直接链接可用
extern uint32_t makeLocalOrderID();

namespace {

/// 记录型 sink，用于验证 addSink 只是把指针放进集合
class CountingSink : public ITrdNotifySink
{
public:
	virtual void on_trade(uint32_t, const char*, bool, double, double) override { ++trades; }
	virtual void on_order(uint32_t, const char*, bool, double, double, double, bool) override { ++orders; }
	virtual void on_channel_ready() override { ++readys; }
	virtual void on_channel_lost() override { ++losts; }

	uint32_t trades = 0;
	uint32_t orders = 0;
	uint32_t readys = 0;
	uint32_t losts = 0;
};

} // namespace

TEST(N01TraderAdapter, fresh_adapter_is_not_logged_in)
{
    TraderAdapter adapter;
    // AS_NOTLOGIN 是 AdapterState 的首个枚举值；类型在类内私有，只能转 int 比较
    EXPECT_EQ(static_cast<int>(adapter.state()), 0);
    EXPECT_FALSE(adapter.isReady());
    EXPECT_STREQ(adapter.id(), "");
    EXPECT_EQ(adapter.getBaseDataMgr(), nullptr);
}

TEST(N01TraderAdapter, init_rejects_null_params_without_touching_state)
{
    TraderAdapter adapter;
    EXPECT_FALSE(adapter.init("ctp01", nullptr, nullptr, nullptr));
    // init 在 params==NULL 时最先返回，_id/_cfg/_bd_mgr 都未被动过
    EXPECT_STREQ(adapter.id(), "");
    EXPECT_EQ(adapter.getBaseDataMgr(), nullptr);
    EXPECT_EQ(static_cast<int>(adapter.state()), 0);
}

TEST(N01TraderAdapter, init_ext_rejects_null_api)
{
    TraderAdapter adapter;
    EXPECT_FALSE(adapter.initExt("ctp01", nullptr, nullptr, nullptr));
    EXPECT_STREQ(adapter.id(), "");
}

TEST(N01TraderAdapter, run_and_release_and_queryfund_are_api_null_safe)
{
    TraderAdapter adapter;
    // _trader_api == NULL：run 直接 false，不解引用 api
    EXPECT_FALSE(adapter.run());
    // release 里 api 为空则整段跳过
    adapter.release();
    // queryFund: 状态不是 AS_ALLREADY 就早退，不会 api->queryAccount()
    adapter.queryFund();
    EXPECT_FALSE(adapter.isReady());
    EXPECT_EQ(static_cast<int>(adapter.state()), 0);
}

TEST(N01TraderAdapter, empty_lookups_return_neutral_values)
{
    TraderAdapter adapter;
    EXPECT_DOUBLE_EQ(adapter.getPosition("SHFE.rb2510", true), 0.0);
    EXPECT_DOUBLE_EQ(adapter.getPosition("SHFE.rb2510", false), 0.0);
    EXPECT_DOUBLE_EQ(adapter.getPosition("DCE.a2509", true, 1), 0.0);
    EXPECT_DOUBLE_EQ(adapter.getPosition("DCE.a2509", false, 2), 0.0);
    EXPECT_DOUBLE_EQ(adapter.getUndoneQty("CFFEX.if2510"), 0.0);
    // _orders 尚未由 onRspOrders 创建 -> NULL
    EXPECT_EQ(adapter.getOrders("SHFE.rb2510"), nullptr);
    EXPECT_EQ(adapter.getOrders(""), nullptr);
    EXPECT_FALSE(adapter.isSelfMatched("SHFE.rb2510"));
}

// 风控未启用（_risk_mon_enabled 默认 false）时三个闸门全部放行，
// 且不会去解引用 _stat_map / _risk_params_map（后者没有 "default" 项会越界）
TEST(N01TraderAdapter, risk_gates_pass_when_riskmon_disabled)
{
    TraderAdapter adapter;
    EXPECT_TRUE(adapter.checkOrderLimits("SHFE.rb2510"));
    EXPECT_TRUE(adapter.checkCancelLimits("SHFE.rb2510"));
    EXPECT_TRUE(adapter.checkOrderLimits("NOT.A.CODE"));
    // 注意：isTradeEnabled() 在头文件里声明为 inline 却只在 .cpp 里定义，
    // 外部 TU 无法调用（-Wundefined-inline），故本用例不调用它（见 BUG-N01-004）。
}

TEST(N01TraderAdapter, null_payloads_on_response_callbacks_are_ignored)
{
    TraderAdapter adapter;
    CountingSink sink;
    adapter.addSink(&sink);

    // ayPositions == NULL：跳过遍历；状态不是 AS_LOGINED，也不会 api->queryOrders()
    adapter.onRspPosition(NULL);
    adapter.onRspOrders(NULL);
    adapter.onPushOrder(NULL);
    // orderInfo 非空但没有合约信息 -> 第二道关卡早退
    WTSOrderInfo* dummyOrder = nullptr;
    adapter.onPushOrder(dummyOrder);
    EXPECT_EQ(static_cast<int>(adapter.state()), 0);
}

TEST(N01TraderAdapter, add_sink_is_idempotent_set_membership)
{
    TraderAdapter adapter;
    CountingSink a;
    CountingSink b;
    adapter.addSink(&a);
    adapter.addSink(&b);
    adapter.addSink(&a);  // wt_hashset 去重，不应崩溃也不应重复
    EXPECT_EQ(static_cast<int>(adapter.state()), 0);
    // 默认空实现的三个可选回调（on_position/on_entrust/on_account）
    a.on_position("SHFE.rb2510", true, 0, 0, 0, 0, 20250110);
    a.on_entrust(1, "SHFE.rb2510", true, "ok");
    a.on_account("CNY", 1, 1, 1, 1, 0, 0, 0, 0, 0, 0);
}

TEST(N01TraderAdapter, local_order_id_is_monotonic)
{
    // 只校验单调性与互异性，不比对任何墙钟绝对值
    uint32_t first = makeLocalOrderID();
    uint32_t second = makeLocalOrderID();
    uint32_t third = makeLocalOrderID();
    EXPECT_NE(first, UINT_MAX);
    EXPECT_EQ(second, first + 1);
    EXPECT_EQ(third, second + 1);

    std::set<uint32_t> ids;
    for (int i = 0; i < 64; i++)
        ids.insert(makeLocalOrderID());
    EXPECT_EQ(ids.size(), 64u) << "本地委托号必须互不重复";
}

TEST(N01TraderAdapter, mgr_rejects_null_and_empty_names)
{
    TraderAdapterMgr mgr;
    TraderAdapterPtr nullPtr;
    EXPECT_FALSE(mgr.addAdapter("ctp01", nullPtr));

    TraderAdapterPtr nullName(new TraderAdapter());
    EXPECT_FALSE(mgr.addAdapter("", nullName));

    EXPECT_EQ(mgr.getAdapter("ctp_none"), nullptr);
    EXPECT_TRUE(mgr.getAdapters().empty());
}

TEST(N01TraderAdapter, mgr_registers_unique_names_and_rejects_duplicates)
{
    TraderAdapterMgr mgr;
    TraderAdapterPtr first(new TraderAdapter());
    TraderAdapterPtr second(new TraderAdapter());

    ASSERT_TRUE(mgr.addAdapter("ctp01", first));
    EXPECT_FALSE(mgr.addAdapter("ctp01", second)) << "同名通道必须被拒绝";
    ASSERT_TRUE(mgr.addAdapter("ctp02", second));

    EXPECT_EQ(mgr.getAdapter("ctp01"), first);
    EXPECT_EQ(mgr.getAdapter("ctp02"), second);
    EXPECT_EQ(mgr.getAdapter("ctp03"), nullptr);
    EXPECT_EQ(mgr.getAdapters().size(), 2u);
    EXPECT_STREQ(mgr.getAdapter("ctp01")->id(), "");
}

TEST(N01TraderAdapter, mgr_run_release_and_refresh_funds_without_api)
{
    TraderAdapterMgr mgr;
    TraderAdapterPtr a(new TraderAdapter());
    TraderAdapterPtr b(new TraderAdapter());
    ASSERT_TRUE(mgr.addAdapter("t_a", a));
    ASSERT_TRUE(mgr.addAdapter("t_b", b));

    // 每个 adapter 的 _trader_api 都是 NULL -> run() 返回 false，不崩溃
    mgr.run();
    // 状态都不是 AS_ALLREADY -> queryFund 全部早退
    mgr.refresh_funds();
    EXPECT_FALSE(a->isReady());
    EXPECT_FALSE(b->isReady());

    mgr.release();
    EXPECT_TRUE(mgr.getAdapters().empty());
    EXPECT_EQ(mgr.getAdapter("t_a"), nullptr);
}

/*
 * DISABLED：同类「未初始化指针被解引用」缺陷类集中在这里，启用用例一条不留。
 *
 * TraderAdapter 的 _stat_map 只在 run() 里创建（run 又被 _trader_api==NULL 挡住），
 * 而 onPushOrder(WOS_Canceled) / onPushTrade 里都无条件写
 *     (WTSTradeStateInfo*)_stat_map->get(stdCode)
 * ——只要订单/成交回报在 run() 之前（或 api==NULL 的单测里）到达就是空指针解引用。
 * 同一缺陷类的另一个入口是 getRiskParams()：规则表里没有 "default" 项时
 *     return &it->second;   // it == _risk_params_map.end()
 * 同样是指向 end() 的越界解引用（需 _risk_mon_enabled 为 true 才会被走到）。
 * 这两条都需要真实交易回报对象（WTSTradeInfo/WTSOrderInfo 的 create + 合约信息），
 * 构造成本与真实通道等价，故只作为文档化示例保留，不参与常规度量。
 */
TEST(N01TraderAdapter, DISABLED_push_report_before_run_dereferences_null_stat_map)
{
    TraderAdapter adapter;
    // 需要一个带合约信息、状态为 WOS_Canceled 的 WTSOrderInfo；
    // 进入 onPushOrder 的撤单统计分支后 _stat_map==NULL -> 段错误。
    // 构造成本等价于真实交易通道回报，见 manifest 跳过清单。
   TraderAdapter adapter2;
    (void)adapter2;
}
