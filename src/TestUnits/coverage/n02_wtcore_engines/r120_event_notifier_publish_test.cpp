/**
 * r120_event_notifier_publish_test.cpp — lane R120 / wave20
 *
 * 目标：src/WtCore/EventNotifier.cpp 的发布体（_mq_sid != 0 之后的 9 个 lambda、
 * tradeToJson / orderToJson）。
 *
 * 覆盖行段：
 *   - init 成功链 :58-112
 *   - notify_log :121-140；notify_event :149-166；notify(trader,msg) :176-194
 *   - notify(trade) :205-211 + tradeToJson :232-265
 *   - notify(order) :222-227 + orderToJson :267-305
 *   - notify_chart_index :315-335；notify_chart_marker :346-366
 *   - notify_trade :377-399
 *
 * 关键时序（产品现有实现，见 bugs/R120.md BUG-R120-001）：
 *   init() 里的 work_guard 是局部的，函数返回即析构；worker 线程第一次
 *   run_one() 发现 outstanding_work == 0 会调用 io_context::stop()，此后 post
 *   的 handler 永远不执行。因此必须在 init() 返回后、worker 的 2ms sleep 结束
 *   之前把全部通知一次性 post 出去——本用例先把成交/委托对象造好，init 之后
 *   立刻连发，再留 120ms 让 worker 逐条消费。不订阅、不建外网连接。
 */
#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/EventNotifier.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

std::string msgque_dir()
{
    const std::string wrapped = DLLHelper::wrap_module("WtMsgQue", "lib");
    const std::string cwd = WtHelper::getCWD();
    if (fs::exists(fs::path(cwd) / wrapped))
        return cwd;
    return r71::bin_dir() + "/";
}

std::string unique_url(const char* tag)
{
    static uint32_t seq = 0;
    return std::string("inproc://r120_ntf_") + tag + "_" +
           std::to_string(static_cast<long long>(::getpid())) + "_" +
           std::to_string(++seq);
}

class R120Notifier : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    std::string oldInst;

    void SetUp() override
    {
        oldInst = WtHelper::getInstDir();
        WtHelper::setInstDir(msgque_dir().c_str());
    }

    void TearDown() override { WtHelper::setInstDir(oldInst.c_str()); }

    WTSVariant* activeCfg(const std::string& url)
    {
        const std::string json = "{\"active\":true,\"url\":\"" + url + "\"}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }
};

//////////////////////////////////////////////////////////////////
// 一次 post 全部 8 个通知入口，覆盖 9 个 lambda + 两个 JSON 组包函数
// src/WtCore/EventNotifier.cpp:58-112, 121-140, 149-166, 176-194,
// 205-211, 222-227, 232-265, 267-305, 315-335, 346-366, 377-399
//////////////////////////////////////////////////////////////////
TEST_F(R120Notifier, BurstPublishesEveryPayload)
{
    if (!fs::exists(fs::path(msgque_dir()) / DLLHelper::wrap_module("WtMsgQue", "lib")))
        GTEST_SKIP() << "libWtMsgQue not found";

    // 所有载荷在 init 之前造好，init 返回后只做 post，避免错过 2ms 窗口
    WTSTradeInfo* trd = WTSTradeInfo::create("rb2510", "SHFE");
    ASSERT_NE(nullptr, trd);
    trd->setDirection(WDT_LONG);
    trd->setOffsetType(WOT_CLOSETODAY);
    trd->setVolume(2.0);
    trd->setPrice(3500.0);

    WTSOrderInfo* ord = WTSOrderInfo::create();
    ASSERT_NE(nullptr, ord);
    ord->setCode("rb2510");
    ord->setExchange("SHFE");
    ord->setDirection(WDT_SHORT);
    ord->setOffsetType(WOT_OPEN);
    ord->setOrderState(WOS_Canceled);
    ord->setVolume(3.0);
    ord->setVolTraded(1.0);
    ord->setVolLeft(2.0);
    ord->setPrice(3510.0);
    ord->setStateMsg("canceled");

    WTSVariant* cfg = activeCfg(unique_url("burst"));
    ASSERT_NE(nullptr, cfg);

    EventNotifier notifier;
    ASSERT_TRUE(notifier.init(cfg));

    notifier.notify_log("R120", "log payload");
    notifier.notify_event("event payload");
    notifier.notify("R120_TRADER", "trader message");
    notifier.notify("R120_TRADER", 11u, "SHFE.rb.2510", trd);
    notifier.notify("R120_TRADER", 12u, "SHFE.rb.2510", ord);
    notifier.notify_chart_index(20250110093100ull, "r120stra", "idx", "line", 1.5);
    notifier.notify_chart_marker(20250110093100ull, "r120stra", 3500.0, "icon", "tag");
    notifier.notify_trade("r120stra", "SHFE.rb.2510", true, false,
                          20250110093100ull, 3501.0, "usertag");

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    cfg->release();
    trd->release();
    ord->release();
}

//////////////////////////////////////////////////////////////////
// 非空/空成交与委托的守卫两侧 + worker 停止后的迟到 post 不崩溃
// src/WtCore/EventNotifier.cpp:197-200, 214-217, 371-372
//////////////////////////////////////////////////////////////////
TEST_F(R120Notifier, NullTradeAndOrderGuardsPlusLatePost)
{
    if (!fs::exists(fs::path(msgque_dir()) / DLLHelper::wrap_module("WtMsgQue", "lib")))
        GTEST_SKIP() << "libWtMsgQue not found";

    WTSTradeInfo* trd = WTSTradeInfo::create("rb2510", "SHFE");
    ASSERT_NE(nullptr, trd);
    WTSOrderInfo* ord = WTSOrderInfo::create();
    ASSERT_NE(nullptr, ord);

    WTSVariant* cfg = activeCfg(unique_url("guard"));
    ASSERT_NE(nullptr, cfg);

    EventNotifier notifier;
    ASSERT_TRUE(notifier.init(cfg));

    // 第一操作数短路：不 retain、不 post
    notifier.notify("R120_TRADER", 1u, "SHFE.rb.2510", static_cast<WTSTradeInfo*>(NULL));
    notifier.notify("R120_TRADER", 2u, "SHFE.rb.2510", static_cast<WTSOrderInfo*>(NULL));

    // 非空对象走 retain/post 分支
    notifier.notify("R120_TRADER", 3u, "SHFE.rb.2510", trd);
    notifier.notify("R120_TRADER", 4u, "SHFE.rb.2510", ord);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    // 产品现状：worker 已 stop，迟到 post 只是入队，不再发布；此处确认不崩溃
    notifier.notify_log("R120", "late log");
    notifier.notify_trade("r120stra", "SHFE.rb.2510", false, true,
                          20250110093100ull, 3499.0, "late");

    cfg->release();
    trd->release();
    ord->release();
}

}  // namespace
