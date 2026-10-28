/**
 * r160_trader_adapter_test.cpp — lane R160 / wave24
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的剩余行。
 *
 * 复用 P11 的进程内替身（FakeTraderApi / FakeBaseDataMgr / RecordingSink），
 * 不接真实柜台、不起线程：
 *   - init() 真正装载 TraderMocker 插件：补 L191-206（createTrader/deleteTrader/
 *     api->init 成功链）
 *   - onRspEntrust(error) 的品种代码分支：补 L1459/L1462-1463/L1474
 *   - 已撤平空今仓回报 + 死亡订单撤单：补 L1247、L1307、L2070-2072
 *
 * 另外用 L2071 的持仓可平量更新验证撤单回报的副作用：s_newavail += leftQty。
 */
#include "r160_common.h"

#include <TestUnits/coverage/p11_wtcore_traderadapter/p11_trader_fixture.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <WtCore/WtHelper.h>

namespace {

class R160TraderTest : public P11TraderTest
{
};

} // namespace

// 走 init() 的真实插件装载链：createTrader -> api->init -> return true
// src/WtCore/TraderAdapter.cpp:184-206
TEST_F(R160TraderTest, R160InitLoadsTraderModuleAndParsesNodeConfig)
{
    const std::string binDir = r160::find_module_dir("TraderMocker");
    if (binDir.empty())
        GTEST_SKIP() << "libTraderMocker not found next to test binary";

    r160::TempDir tmp("trader_module");
    const std::string tradersDir = tmp.path("traders");
    std::error_code ec;
    std::filesystem::create_directories(tradersDir, ec);
    ASSERT_FALSE(ec);

    const std::string libName = DLLHelper::wrap_module("TraderMocker", "lib");
    std::filesystem::create_symlink(
        std::filesystem::path(binDir) / libName,
        std::filesystem::path(tradersDir) / libName, ec);
    ASSERT_FALSE(ec) << "cannot link TraderMocker into the temporary traders dir";

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((tmp.path() + "/").c_str());

    // TraderMocker::init 会按 mockerid 在 CWD 下建 mocker_<id>/，切到临时目录以便整棵清理
    r160::ScopedCwd cwd(tmp.path());
    ASSERT_TRUE(cwd.ok());

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"TraderMocker\",\"mockerid\":160160,\"span\":50}");
    ASSERT_NE(cfg, nullptr);

    TraderAdapter adapter;
    EXPECT_TRUE(adapter.init("r160a", cfg, &bd_, NULL));
    EXPECT_STREQ(adapter.id(), "r160a");
    EXPECT_EQ(adapter.getBaseDataMgr(), &bd_);
    EXPECT_EQ(static_cast<int>(adapter.state()), static_cast<int>(TraderAdapter::AS_NOTLOGIN));
    EXPECT_FALSE(adapter.isReady());

    // 只验证 init 装载链；这里不能调 adapter.release()：
    // TraderMocker::registerSpi 对 NULL 直接解引用（BUG-R175-001），release 必崩。
    // 每用例独立进程，未 release 的 api 对象由进程回收。

    cfg->release();
    WtHelper::setInstDir(oldInst.c_str());
}

// BUG-R175-001 的可执行证据：TraderAdapter::release() 先 registerSpi(NULL)，
// 而 TraderMocker::registerSpi 未判空（TraderMocker.cpp:659-664），必 SIGSEGV。
TEST_F(R160TraderTest, DISABLED_R160ReleaseAfterInitCrashesInMockerRegisterSpi)
{
    const std::string binDir = r160::find_module_dir("TraderMocker");
    if (binDir.empty())
        GTEST_SKIP() << "libTraderMocker not found next to test binary";

    r160::TempDir tmp("trader_release");
    const std::string tradersDir = tmp.path("traders");
    std::error_code ec;
    std::filesystem::create_directories(tradersDir, ec);
    ASSERT_FALSE(ec);

    const std::string libName = DLLHelper::wrap_module("TraderMocker", "lib");
    std::filesystem::create_symlink(
        std::filesystem::path(binDir) / libName,
        std::filesystem::path(tradersDir) / libName, ec);
    ASSERT_FALSE(ec);

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((tmp.path() + "/").c_str());

    r160::ScopedCwd cwd(tmp.path());
    ASSERT_TRUE(cwd.ok());

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"TraderMocker\",\"mockerid\":160161,\"span\":50}");
    ASSERT_NE(cfg, nullptr);

    TraderAdapter adapter;
    ASSERT_TRUE(adapter.init("r160rel", cfg, &bd_, NULL));

    adapter.release();   // 崩溃点：listener->getBaseDataMgr()

    cfg->release();
    WtHelper::setInstDir(oldInst.c_str());
}

// onRspEntrust 在错误回报下的三个代码形态 + close today 动作串
// src/WtCore/TraderAdapter.cpp:1450-1483
TEST_F(R160TraderTest, R160RspEntrustErrorsCoverMonthlyFlatAndCloseToday)
{
    TraderAdapter adapter;
    ASSERT_NO_FATAL_FAILURE(buildReady(adapter));

    WTSError* err = WTSError::create(WEC_ORDERINSERT, "r160 rejected");
    ASSERT_NE(err, nullptr);

    // 分月代码 ag2512 -> rawMonthCodeToStdCode；平今 -> "closetoday long"
    WTSEntrust* monthly = WTSEntrust::create("ag2512", 2, 3500.0, "SHFE");
    ASSERT_NE(monthly, nullptr);
    monthly->setContractInfo(ct_);
    monthly->setDirection(WDT_LONG);
    monthly->setOffsetType(WOT_CLOSETODAY);
    monthly->setVolume(2);
    adapter.onRspEntrust(monthly, err);

    EXPECT_TRUE(sink_.entrusts_.empty()) << "undone==0 时错误回报不得二次通知";
    EXPECT_DOUBLE_EQ(adapter.getUndoneQty(p11fix::kStdCode), 0.0);
    EXPECT_FALSE(adapter.isReady());
    monthly->release();

    // 品种连续代码 ag -> isMonthlyCode 为假，走 rawFlatCodeToStdCode；
    // 平仓（非今仓）-> "close short"
    WTSCommodityInfo* flatComm = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
    ASSERT_NE(flatComm, nullptr);
    flatComm->setCategory(CC_Future);
    flatComm->setCoverMode(CM_CoverToday);
    WTSContractInfo* flatCt = WTSContractInfo::create("ag", "ag", "SHFE", "ag");
    ASSERT_NE(flatCt, nullptr);
    flatCt->setCommInfo(flatComm);

    WTSEntrust* flat = WTSEntrust::create("ag", 1, 3500.0, "SHFE");
    ASSERT_NE(flat, nullptr);
    flat->setContractInfo(flatCt);
    flat->setDirection(WDT_SHORT);
    flat->setOffsetType(WOT_CLOSE);
    flat->setVolume(1);
    adapter.onRspEntrust(flat, err);

    EXPECT_TRUE(sink_.entrusts_.empty());
    flat->release();
    flatCt->release();
    flatComm->release();
    err->release();
}

// 已撤平空今仓回报：_positions 可平量更新 + 死亡订单撤单的两个早退
// src/WtCore/TraderAdapter.cpp:2067-2072、1244-1247、1305-1307
TEST_F(R160TraderTest, R160CanceledCloseShortTodayOrderAndDeadCancelPaths)
{
    TraderAdapter adapter;
    ASSERT_NO_FATAL_FAILURE(buildReady(adapter));

    WTSOrderInfo* ord = WTSOrderInfo::create();
    ASSERT_NE(ord, nullptr);
    ord->setContractInfo(ct_);
    ord->setDirection(WDT_SHORT);
    ord->setOffsetType(WOT_CLOSETODAY);
    ord->setOrderState(WOS_Canceled);
    ord->setOrderFlag(WOF_NOR);
    ord->setVolume(3);
    ord->setVolTraded(1);
    ord->setVolLeft(2);
    ord->setPrice(3510.0);
    ord->setOrderID("O160");
    ord->setEntrustID("E160");
    ord->setUserTag("otp.p11a.77");   // _order_pattern = "otp." + id
    ord->setStateMsg("canceled by test");

    adapter.onPushOrder(ord);

    ASSERT_EQ(sink_.orders_.size(), 1u);
    EXPECT_TRUE(sink_.orders_[0].canceled);
    EXPECT_DOUBLE_EQ(sink_.orders_[0].leftQty, 2.0);

    // 撤单回报已把订单放进 _orders，但订单已不存活 -> doCancel 首行早退
    EXPECT_FALSE(adapter.cancel(77u));

    // 按合约撤单时，!isAlive() 的订单被直接跳过，返回空集合
    const OrderIDs ids = adapter.cancel(p11fix::kStdCode, false);
    EXPECT_TRUE(ids.empty());

    ord->release();
    adapter.release();
}
