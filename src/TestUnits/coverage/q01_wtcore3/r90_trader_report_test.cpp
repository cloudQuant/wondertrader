/**
 * r90_trader_report_test.cpp — lane R90 / wave17
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的公开回报分发、统计分支与通知守卫。
 * 复用 p11/r59 已有的进程内替身（只读），不接柜台、不起线程。
 *
 * 覆盖行段：
 *   - onRspOrders 买/卖 × 错单/普通撤单/自动撤单统计：TraderAdapter.cpp:1645-1693
 *   - onRspTrades 空头平今统计：:1786-1795
 *   - onPushOrder 空合约早退、stateToName 的 UnSmt/Error、空头平仓可平量、二次撤单回补：
 *     :1875-1880, :1860-1870, :2042-2085
 *   - onPushTrade 空头平今与净头寸四个方向：:2225-2235, :2246-2306
 *   - 非活跃 EventNotifier 的 5 个通知守卫 + closeLong/closeShort 市价单：
 *     :1434, :1504, :2145, :2332, :2343, :1372, :1390
 */
#include "../p11_wtcore_traderadapter/r59_trader_fixture.h"
#include "r90_env.h"

#include <WtCore/EventNotifier.h>

#include <initializer_list>
#include <memory>

namespace {

using namespace r59x;

template <typename T>
struct R90Release
{
	void operator()(T* value) const noexcept
	{
		if (value)
			value->release();
	}
};

template <typename T>
using R90Owner = std::unique_ptr<T, R90Release<T>>;

template <typename T>
R90Owner<WTSArray> makeOwnedArray(std::initializer_list<T*> items)
{
	WTSArray* array = makeArray(items);
	for (T* item : items)
	{
		if (item)
			item->release(); // Transfer each newly-created object's reference to the array.
	}
	return R90Owner<WTSArray>(array);
}

WTSOrderInfo* extOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    WTSOrderState st, double vol, double traded, const char* oid, WTSOrderFlag flag,
    bool isError)
{
    WTSOrderInfo* o = makeOrder(ct, dir, off, st, vol, traded, oid,
        (std::string("EXT.") + oid).c_str(), flag);
    if (isError)
        o->setError(WOEF_Normal);
    return o;
}

class R90TraderReport : public R59TraderTest
{
protected:
    wtp::TraderAdapter trader_;

    void buildPlain(wtp::TraderAdapter& trader)
    {
        build(trader, NULL);
    }
};

//////////////////////////////////////////////////////////////////
// onRspOrders 统计全分支 + stateToName 剩余两支 + 空合约早退
// src/WtCore/TraderAdapter.cpp:1645-1693, 1860-1880, 1786-1795
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderReport, OrderReportStatsAndStateNames)
{
    buildPlain(trader_);

    // 买 × 错单 / 普通撤单 / 自动撤单；卖 × 错单 / 普通撤单 / 自动撤单
    R90Owner<WTSArray> orderReports = makeOwnedArray({
        extOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 5, 2, "BE1", WOF_NOR, true),
        extOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 4, 1, "BC1", WOF_NOR, false),
        extOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Canceled, 3, 1, "BA1", WOF_FAK, false),
        extOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 6, 2, "SE1", WOF_NOR, true),
        extOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 5, 2, "SC1", WOF_NOR, false),
        extOrder(ct_, WDT_SHORT, WOT_CLOSE, WOS_Canceled, 2, 0, "SA1", WOF_FOK, false),
    });
    trader_.onRspOrders(orderReports.get());

    // 外部委托不进内部订单表
    wtp::OrderMap* tab = trader_.getOrders("");
    ASSERT_NE(nullptr, tab);
    EXPECT_EQ(0u, tab->size());
    tab->release();

    // 空头平今成交统计（onRspTrades 的 s_closetvol 分支）
    R90Owner<WTSArray> tradeReports = makeOwnedArray({
        makeTrade(ct_, WDT_SHORT, WOT_CLOSETODAY, 2, 7000, "TCT", "OCT", "EXT.TCT") });
    trader_.onRspTrades(tradeReports.get());

    // stateToName：Nottouched -> UnSmt；非法枚举 -> Error
    WTSOrderInfo* nt = makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Nottouched, 1, 0,
        "NT1", "EXT.NT1");
    trader_.onPushOrder(nt);
    nt->release();

    WTSOrderInfo* bad = makeOrder(ct_, WDT_LONG, WOT_OPEN,
        (WTSOrderState)0x7fffffff, 1, 0, "BD1", "EXT.BD1");
    trader_.onPushOrder(bad);
    bad->release();

    // 无合约信息的推送在入口早退
    WTSOrderInfo* bare = WTSOrderInfo::create();
    trader_.onPushOrder(bare);
    bare->release();

    EXPECT_EQ(wtp::TraderAdapter::AS_LOGINED, trader_.state());
}

//////////////////////////////////////////////////////////////////
// 空头二次推送补齐可平量 + 空头平今成交 + 净头寸四方向
// src/WtCore/TraderAdapter.cpp:2042-2085, 2225-2235, 2246-2306
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderReport, ShortCloseCancelAndNetTrades)
{
    buildPlain(trader_);
    seedPos(trader_, {
        makePos(ct_, WDT_LONG, 0, 0, 5, 5),
        makePos(ct_, WDT_SHORT, 2, 2, 2, 2) });

    // 首次推送：空头平昨 3 手 -> 先扣昨仓可用，再扣今仓可用
    R90Owner<WTSOrderInfo> submitting = R90Owner<WTSOrderInfo>(makeOrder(ct_, WDT_SHORT, WOT_CLOSE,
        WOS_Submitting, 3, 0, "SC1", "otp.p11a.9001"));
    trader_.onPushOrder(submitting.get());
    EXPECT_DOUBLE_EQ(-1.0, trader_.getPosition(kStdCode, true, 2));

    // 二次推送且撤单：回补 3 手，昨仓上限 2，溢出 1 手回到今仓
    R90Owner<WTSOrderInfo> canceled = R90Owner<WTSOrderInfo>(makeOrder(ct_, WDT_SHORT, WOT_CLOSE,
        WOS_Canceled, 3, 0, "SC1", "otp.p11a.9001"));
    trader_.onPushOrder(canceled.get());
    EXPECT_DOUBLE_EQ(-4.0, trader_.getPosition(kStdCode, true, 2));

    // 空头平今成交：今仓 2 -> 0，昨仓不变
    R90Owner<WTSTradeInfo> closeTrade = R90Owner<WTSTradeInfo>(makeTrade(ct_, WDT_SHORT,
        WOT_CLOSETODAY, 2, 7000, "ST1", "SO1", "otp.p11a.9002"));
    trader_.onPushTrade(closeTrade.get());
    EXPECT_DOUBLE_EQ(-2.0, trader_.getPosition(kStdCode, false, 2));

    // 净头寸卖出：多头今仓 3 - 2 -> 1（不触发反手）
    {
        wtp::TraderAdapter t2;
        buildPlain(t2);
        seedPos(t2, { makePos(ct_, WDT_LONG, 0, 0, 3, 3) });
        R90Owner<WTSTradeInfo> t = R90Owner<WTSTradeInfo>(
            makeTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000, "N1", "N1", ""));
        t->setNetDirection(false);
        t2.onPushTrade(t.get());
        EXPECT_DOUBLE_EQ(1.0, t2.getPosition(kStdCode, false, 1));
        EXPECT_DOUBLE_EQ(0.0, t2.getPosition(kStdCode, false, 2));
    }

    // 净头寸卖出：无多头 -> 开空 2 手
    {
        wtp::TraderAdapter t3;
        buildPlain(t3);
        R90Owner<WTSTradeInfo> t = R90Owner<WTSTradeInfo>(
            makeTrade(ct_, WDT_SHORT, WOT_OPEN, 2, 7000, "N2", "N2", ""));
        t->setNetDirection(false);
        t3.onPushTrade(t.get());
        EXPECT_DOUBLE_EQ(-2.0, t3.getPosition(kStdCode, false, 2));
    }

    // 净头寸买入：先平空昨仓 1 手
    {
        wtp::TraderAdapter t4;
        buildPlain(t4);
        seedPos(t4, { makePos(ct_, WDT_SHORT, 2, 2, 0, 0) });
        R90Owner<WTSTradeInfo> t = R90Owner<WTSTradeInfo>(
            makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "N3", "N3", ""));
        t->setNetDirection(true);
        t4.onPushTrade(t.get());
        EXPECT_DOUBLE_EQ(-1.0, t4.getPosition(kStdCode, false, 2));
    }

    // 净头寸买入：无空头 -> 开多 2 手
    {
        wtp::TraderAdapter t5;
        buildPlain(t5);
        R90Owner<WTSTradeInfo> t = R90Owner<WTSTradeInfo>(
            makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "N4", "N4", ""));
        t->setNetDirection(true);
        t5.onPushTrade(t.get());
        EXPECT_DOUBLE_EQ(2.0, t5.getPosition(kStdCode, false, 1));
    }
}

//////////////////////////////////////////////////////////////////
// 非活跃通知器守卫 + closeLong/closeShort 市价 + 错单回报
// src/WtCore/TraderAdapter.cpp:1434, 1504, 2145, 2332, 2343, 1372, 1390
//////////////////////////////////////////////////////////////////
TEST_F(R90TraderReport, InactiveNotifierGuardsAndMarketCloses)
{
    wtp::EventNotifier notifier;  // 未 init -> _mq_sid==0，全部 notify 短路
    wtp::TraderAdapter trader(&notifier);
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api2_, &bd_, NULL));
    trader.addSink(&sink_);
    ASSERT_TRUE(trader.run());

    // 登录失败与通道错误：_notifier 非空但未激活
    trader.onLoginResult(false, "r90 login failed", 0);
    EXPECT_EQ(wtp::TraderAdapter::AS_LOGINFAILED, trader.state());
    WTSError* err = WTSError::create(WEC_ORDERINSERT, "boom");
    trader.onTraderError(err, NULL);
    err->release();

    // 建立未完成量，供错单回报走 updateUndone + notify 分支
    R90Owner<WTSArray> orderReports = makeOwnedArray({
        makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 2, 0,
            "G1", "otp.p11a.7001") });
    trader.onRspOrders(orderReports.get());

    WTSEntrust* ent = WTSEntrust::create("ag2512", 1, 7000.0);
    ent->setContractInfo(ct_);
    ent->setDirection(WDT_LONG);
    ent->setOffsetType(WOT_OPEN);
    ent->setUserTag("otp.p11a.7002");
    ent->setEntrustID("R90E1");
    err = WTSError::create(WEC_ORDERINSERT, "rejected");
    trader.onRspEntrust(ent, err);
    err->release();
    ent->release();

    // 委托/成交推送：_notifier 守卫
    R90Owner<WTSOrderInfo> pushedOrder = R90Owner<WTSOrderInfo>(
        makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_Submitting, 1, 0, "G2", "otp.p11a.7003"));
    trader.onPushOrder(pushedOrder.get());
    R90Owner<WTSTradeInfo> pushedTrade = R90Owner<WTSTradeInfo>(
        makeTrade(ct_, WDT_LONG, WOT_OPEN, 1, 7000, "G3", "G2", "otp.p11a.7003"));
    trader.onPushTrade(pushedTrade.get());

    // 平多/平空价格 0 -> WPT_ANYPRICE
    uint32_t before = api2_.n_insert_;
    EXPECT_NE(UINT_MAX, trader.closeLong(kStdCode, 0.0, 1, false, 0));
    EXPECT_EQ(WPT_ANYPRICE, api2_.last_pxtype_);
    EXPECT_NE(UINT_MAX, trader.closeShort(kStdCode, 0.0, 1, false, 0));
    EXPECT_EQ(WPT_ANYPRICE, api2_.last_pxtype_);
    EXPECT_EQ(before + 2, api2_.n_insert_);
}

TEST_F(R90TraderReport, NormalInitWithoutRiskmonAllowsSuccessfulOrder)
{
    namespace fs = std::filesystem;
    const std::string moduleDir = r90::find_bin_dir();
    if (moduleDir.empty())
        GTEST_SKIP() << "build bin dir with TraderMocker not found";

    const std::string instRoot = r90::temp_root() + "r90_no_riskmon/";
    const fs::path tradersDir = fs::path(instRoot) / "traders";
    std::error_code ec;
    fs::create_directories(tradersDir, ec);
    ASSERT_FALSE(ec);

    const std::string moduleName = DLLHelper::wrap_module("TraderMocker", "lib");
    const fs::path modulePath = fs::path(moduleDir) / moduleName;
    if (!fs::exists(modulePath))
        GTEST_SKIP() << "TraderMocker plugin not found: " << modulePath.string();
    fs::copy_file(modulePath, tradersDir / moduleName, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "cannot copy TraderMocker into the temporary traders dir: " << ec.message();

    struct ScopedInstDir
    {
        explicit ScopedInstDir(const std::string& path) : previous(WtHelper::getInstDir())
        {
            WtHelper::setInstDir(path.c_str());
        }
        ~ScopedInstDir() { WtHelper::setInstDir(previous.c_str()); }
        std::string previous;
    } instDir(instRoot);

    const uint32_t mockerId = static_cast<uint32_t>(::getpid()) * 100u + 909u;
    struct ScopedMockerDir
    {
        explicit ScopedMockerDir(uint32_t id) : mockerId(id) {}
        ~ScopedMockerDir()
        {
            std::error_code ec;
            fs::remove_all(fs::path("mocker_" + std::to_string(mockerId)), ec);
        }
        uint32_t mockerId;
    } mockerDir(mockerId);

    WTSVariant* params = WTSVariant::createObject();
    params->append("module", "TraderMocker");
    params->append("mockerid", mockerId);
    params->append("span", 50u);
    params->append("udp_port", 0u);
    params->append("savedata", false);
    struct ScopedVariant
    {
        explicit ScopedVariant(WTSVariant* value) : params(value) {}
        ~ScopedVariant() { if (params != nullptr) params->release(); }
        WTSVariant* params;
    } paramsOwner(params);

    wtp::TraderAdapter trader;
    ASSERT_TRUE(trader.init(id_.c_str(), params, &bd_, nullptr));
    ASSERT_TRUE(trader.run());

    EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    trader.release();
}

}  // namespace
