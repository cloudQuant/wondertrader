/**
 * r160_event_notifier_test.cpp — lane R160 / wave24
 *
 * 目标：src/WtCore/EventNotifier.cpp 的发布体。
 *
 * 现有 R41/R120 用例已经 post 了全部通知，但基线报告里 :121-140、:149-166、
 * :176-194、:205-211、:222-227、:315-335、:346-366、:377-399 仍是 0：
 * init() 里的 work_guard 是局部对象，返回即析构；worker 线程第一次
 * run_one() 发现队列为空就进入 stopped 状态，之后 post 的 lambda 永不执行
 * （BUG-R120-001）。本用例用测试侧的 work_guard 把 io_context 的
 * outstanding work 保持到通知全部入队，让 worker 的 run_one() 阻塞等待而不是
 * 空转停机，从而确定性地执行 9 个发布 lambda。
 *
 * 另订阅本地 inproc 端点，断言 LOG / TRD_TRADE 等主题确实收到消息，
 * 避免“只覆盖不验证”。WtMsgQue 缺失时 GTEST_SKIP。
 */
#include "r160_common.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <Includes/WTSMarcos.h>
#include <Includes/WTSObject.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/StdUtils.hpp>

#define private public
#include <WtCore/EventNotifier.h>
#undef private

#include <WtCore/WtHelper.h>
#include <WTSUtils/WTSCfgLoader.h>

namespace {

static_assert(std::is_same<FuncCreateMQServer, WtUInt32(*)(const char*, bool)>::value,
    "create_server function pointer must match WtMsgQue ABI");
static_assert(std::is_same<FuncDestroyMQServer, void(*)(WtUInt32)>::value,
    "destroy_server function pointer must match WtMsgQue ABI");
static_assert(std::is_same<FundPublishMessage,
    void(*)(WtUInt32, const char*, const char*, WtUInt32)>::value,
    "publish_message function pointer must match WtMsgQue ABI");
static_assert(std::is_same<FuncLogCallback,
    void(PORTER_FLAG *)(WtUInt32, const char*, bool)>::value,
    "MQ log callback must match WtMsgQue ABI");

std::atomic<int> g_log{ 0 };
std::atomic<int> g_event{ 0 };
std::atomic<int> g_notify{ 0 };
std::atomic<int> g_trade{ 0 };
std::atomic<int> g_order{ 0 };
std::atomic<int> g_index{ 0 };
std::atomic<int> g_marker{ 0 };
std::atomic<int> g_stra{ 0 };

void PORTER_FLAG on_mq_message(uint32_t /*id*/, const char* topic, const char* /*data*/, uint32_t /*len*/)
{
    const std::string t = topic == NULL ? "" : topic;
    if (t == "LOG") ++g_log;
    else if (t == "GRP_EVENT") ++g_event;
    else if (t == "TRD_NOTIFY") ++g_notify;
    else if (t == "TRD_TRADE") ++g_trade;
    else if (t == "TRD_ORDER") ++g_order;
    else if (t == "CHART_INDEX") ++g_index;
    else if (t == "CHART_MARKER") ++g_marker;
    else if (t == "STRA_TRADE") ++g_stra;
}

TEST(R160EventNotifier, FakeModuleObservesExactServerAndPublishAbi)
{
    const std::string fakeDir = WT_R160_FAKE_MQ_DIR;
    const std::string oldInst = WtHelper::getInstDir();
    struct RestoreInstDir
    {
        std::string value;
        ~RestoreInstDir() { WtHelper::setInstDir(value.c_str()); }
    } restore{ oldInst };
    WtHelper::setInstDir((fakeDir + "/").c_str());

    const std::string fakePath = fakeDir + "/" + DLLHelper::wrap_module("WtMsgQue", "lib");
    DllHandle inspector = DLLHelper::load_library(fakePath.c_str());
    ASSERT_NE(inspector, nullptr);
    using Reset = void(*)();
    using Count = uint32_t(*)();
    using Id = WtUInt32(*)();
    using Length = WtUInt32(*)();
    using Size = uint64_t(*)();
    using Flag = bool(*)();
    using Text = const char*(*)();
    const auto reset = (Reset)DLLHelper::get_symbol(inspector, "r160_fake_reset");
    const auto createCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_create_count");
    const auto destroyCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_destroy_count");
    const auto publishCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_publish_count");
    const auto registerCount = (Count)DLLHelper::get_symbol(inspector, "r160_fake_register_count");
    const auto serverId = (Id)DLLHelper::get_symbol(inspector, "r160_fake_server_id");
    const auto destroyedId = (Id)DLLHelper::get_symbol(inspector, "r160_fake_destroyed_id");
    const auto publishedId = (Id)DLLHelper::get_symbol(inspector, "r160_fake_published_id");
    const auto dataLen = (Length)DLLHelper::get_symbol(inspector, "r160_fake_data_len");
    const auto dataSize = (Size)DLLHelper::get_symbol(inspector, "r160_fake_data_size");
    const auto confirm = (Flag)DLLHelper::get_symbol(inspector, "r160_fake_confirm");
    const auto url = (Text)DLLHelper::get_symbol(inspector, "r160_fake_url");
    const auto topic = (Text)DLLHelper::get_symbol(inspector, "r160_fake_topic");
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(createCount, nullptr);
    ASSERT_NE(destroyCount, nullptr);
    ASSERT_NE(publishCount, nullptr);
    ASSERT_NE(registerCount, nullptr);
    ASSERT_NE(serverId, nullptr);
    ASSERT_NE(destroyedId, nullptr);
    ASSERT_NE(publishedId, nullptr);
    ASSERT_NE(dataLen, nullptr);
    ASSERT_NE(dataSize, nullptr);
    ASSERT_NE(confirm, nullptr);
    ASSERT_NE(url, nullptr);
    ASSERT_NE(topic, nullptr);
    reset();

    constexpr WtUInt32 expectedId = 0xF1234567u;
    const std::string expectedUrl = "fake://r160_abi";
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        ("{\"active\":true,\"url\":\"" + expectedUrl + "\"}").c_str());
    ASSERT_NE(cfg, nullptr);

    {
        EventNotifier notifier;
        auto guard = boost::asio::make_work_guard(notifier._asyncio);
        ASSERT_TRUE(notifier.init(cfg));
        EXPECT_EQ(expectedId, notifier._mq_sid);
        EXPECT_EQ(1u, createCount());
        EXPECT_EQ(1u, registerCount());
        EXPECT_STREQ(expectedUrl.c_str(), url());
        // The notifier config has no confirmation option; it explicitly uses
        // the MQ module's non-confirming default.
        EXPECT_FALSE(confirm());
        EXPECT_EQ(expectedId, serverId());

        notifier.notify_event("ABI payload");
        for (int i = 0; i < 500 && publishCount() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ASSERT_EQ(1u, publishCount());
        EXPECT_EQ(expectedId, publishedId());
        EXPECT_STREQ("GRP_EVENT", topic());
        EXPECT_EQ(dataSize(), static_cast<uint64_t>(dataLen()));
        EXPECT_GT(dataLen(), 0u);
        guard.reset();
    }

    EXPECT_EQ(1u, destroyCount());
    EXPECT_EQ(expectedId, destroyedId());
    cfg->release();
    DLLHelper::free_library(inspector);
}

void reset_counts()
{
    g_log = g_event = g_notify = g_trade = 0;
    g_order = g_index = g_marker = g_stra = 0;
}

} // namespace

TEST(R160EventNotifier, WorkGuardKeepsWorkerAliveForEveryPublishLambda)
{
    const std::string libDir = r160::find_module_dir("WtMsgQue");
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found next to test binary";

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((libDir + "/").c_str());

    reset_counts();

    static std::atomic<uint32_t> seq{ 0 };
    const std::string url = "inproc://r160_ntf_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(++seq);

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        ("{\"active\":true,\"url\":\"" + url + "\"}").c_str());
    ASSERT_NE(cfg, nullptr);

    // 全部载荷先造好，避免在 worker 消费窗口里做额外工作
    WTSTradeInfo* trd = WTSTradeInfo::create("ag2512", "SHFE");
    ASSERT_NE(trd, nullptr);
    trd->setDirection(WDT_LONG);
    trd->setOffsetType(WOT_CLOSETODAY);
    trd->setVolume(2);
    trd->setPrice(3500.0);

    WTSOrderInfo* ord = WTSOrderInfo::create();
    ASSERT_NE(ord, nullptr);
    ord->setCode("ag2512");
    ord->setExchange("SHFE");
    ord->setDirection(WDT_SHORT);
    ord->setOffsetType(WOT_OPEN);
    ord->setOrderState(WOS_Canceled);
    ord->setVolume(3);
    ord->setVolTraded(1);
    ord->setVolLeft(2);
    ord->setPrice(3510.0);
    ord->setStateMsg("canceled");

    // 用显式路径加载与 EventNotifier 同一份 WtMsgQue，避免测试二进制里另有一份
    // 静态副本时客户端/服务端落在两个 MQManager 上。
    const std::string mqPath = libDir + "/" + DLLHelper::wrap_module("WtMsgQue", "lib");
    DllHandle mqHandle = DLLHelper::load_library(mqPath.c_str());
    ASSERT_NE(mqHandle, nullptr);
    using MqCallback = void(PORTER_FLAG*)(uint32_t, const char*, const char*, uint32_t);
    auto createClient = (uint32_t(*)(const char*, MqCallback))DLLHelper::get_symbol(mqHandle, "create_client");
    auto destroyClient = (void(*)(uint32_t))DLLHelper::get_symbol(mqHandle, "destroy_client");
    auto subscribeTopic = (void(*)(uint32_t, const char*))DLLHelper::get_symbol(mqHandle, "subscribe_topic");
    auto startClient = (void(*)(uint32_t))DLLHelper::get_symbol(mqHandle, "start_client");
    ASSERT_NE(createClient, nullptr);
    ASSERT_NE(destroyClient, nullptr);
    ASSERT_NE(subscribeTopic, nullptr);
    ASSERT_NE(startClient, nullptr);

    uint32_t clientId = 0;
    {
        EventNotifier notifier;

        // 在 init() 之前挂上 work_guard：init 返回后仍有一个 outstanding work，
        // worker 的 run_one() 会阻塞等待 post，而不是立即把 io_context 置为 stopped。
        auto guard = boost::asio::make_work_guard(notifier._asyncio);

        ASSERT_TRUE(notifier.init(cfg));

        clientId = createClient(url.c_str(), on_mq_message);
        ASSERT_GT(clientId, 0u);
        subscribeTopic(clientId, "LOG");
        subscribeTopic(clientId, "GRP_EVENT");
        subscribeTopic(clientId, "TRD_NOTIFY");
        subscribeTopic(clientId, "TRD_TRADE");
        subscribeTopic(clientId, "TRD_ORDER");
        subscribeTopic(clientId, "CHART_INDEX");
        subscribeTopic(clientId, "CHART_MARKER");
        subscribeTopic(clientId, "STRA_TRADE");
        startClient(clientId);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        notifier.notify_log("R160", "log payload");
        notifier.notify_event("event payload");
        notifier.notify("R160_TRADER", "trader message");
        notifier.notify("R160_TRADER", 11u, "SHFE.ag.2512", trd);
        notifier.notify("R160_TRADER", 12u, "SHFE.ag.2512", ord);
        notifier.notify_chart_index(20250110093100ull, "r160stra", "idx", "line", 1.5);
        notifier.notify_chart_marker(20250110093100ull, "r160stra", 3500.0, "icon", "tag");
        notifier.notify_trade("r160stra", "SHFE.ag.2512", true, false,
                              20250110093100ull, 3501.0, "usertag");

        // worker 每 2ms 才 run_one 一次，9 条通知按 post 顺序串行消费；
        // 只等 LOG/TRD_TRADE 会在 STRA_TRADE（最后一条）之前结束，必须等全部主题。
        auto all_delivered = []() {
            return g_log.load() >= 1 && g_event.load() >= 1 && g_notify.load() >= 1
                && g_trade.load() >= 1 && g_order.load() >= 1 && g_index.load() >= 1
                && g_marker.load() >= 1 && g_stra.load() >= 1;
        };
        for (int i = 0; i < 500 && !all_delivered(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // 9 个入口必须各自真正发布（而非仅仅 post）
        EXPECT_GE(g_log.load(), 1) << "LOG 主题未收到消息";
        EXPECT_GE(g_event.load(), 1) << "GRP_EVENT 主题未收到消息";
        EXPECT_GE(g_notify.load(), 1) << "TRD_NOTIFY 主题未收到消息";
        EXPECT_GE(g_trade.load(), 1) << "TRD_TRADE 主题未收到消息";
        EXPECT_GE(g_order.load(), 1) << "TRD_ORDER 主题未收到消息";
        EXPECT_GE(g_index.load(), 1) << "CHART_INDEX 主题未收到消息";
        EXPECT_GE(g_marker.load(), 1) << "CHART_MARKER 主题未收到消息";
        EXPECT_GE(g_stra.load(), 1) << "STRA_TRADE 主题未收到消息";

        guard.reset();
    }

    if (clientId != 0)
        destroyClient(clientId);
    cfg->release();
    trd->release();
    ord->release();
    DLLHelper::free_library(mqHandle);
    WtHelper::setInstDir(oldInst.c_str());
}
