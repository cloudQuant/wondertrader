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
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

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
static_assert(std::is_same<decltype(EventNotifier::_stopped), std::atomic<bool>>::value,
    "the worker stop flag must be atomic");

std::atomic<int> g_log{ 0 };
std::atomic<int> g_event{ 0 };
std::atomic<int> g_notify{ 0 };
std::atomic<int> g_trade{ 0 };
std::atomic<int> g_order{ 0 };
std::atomic<int> g_index{ 0 };
std::atomic<int> g_marker{ 0 };
std::atomic<int> g_stra{ 0 };
std::atomic<int> g_order_probe_destructions{ 0 };
std::atomic<int> g_trade_probe_destructions{ 0 };
std::atomic<int> g_throwing_publisher_calls{ 0 };
std::atomic<int> g_blocked_publisher_calls{ 0 };
std::mutex g_blocked_publisher_mutex;
std::condition_variable g_blocked_publisher_cv;
bool g_blocked_publisher_entered = false;
bool g_blocked_publisher_released = false;
std::mutex g_sequence_mutex;
std::condition_variable g_sequence_cv;
std::vector<std::string> g_sequence;
bool g_sequence_entered = false;
bool g_sequence_released = false;
std::atomic<int> g_exception_calls{0};
std::atomic<int> g_exception_successes{0};

void ordered_publisher(WtUInt32, const char*, const char* data, WtUInt32 len)
{
	std::unique_lock<std::mutex> lock(g_sequence_mutex);
	g_sequence.emplace_back(data, len);
	if (g_sequence.size() == 1) {
		g_sequence_entered = true;
		g_sequence_cv.notify_all();
		g_sequence_cv.wait(lock, [] { return g_sequence_released; });
	}
	g_sequence_cv.notify_all();
}

void throw_then_publish(WtUInt32, const char*, const char*, WtUInt32)
{
	if (g_exception_calls.fetch_add(1) == 0)
		throw std::runtime_error("injected publisher failure");
	++g_exception_successes;
}

class OrderInfoDestructionProbe final : public WTSOrderInfo
{
public:
	~OrderInfoDestructionProbe() noexcept override
	{
		++g_order_probe_destructions;
	}

	void release() override
	{
		// This probe is allocated with new, not WTSOrderInfo::create(). Route the
		// final reference through WTSObject::release() so delete invokes the
		// virtual probe destructor instead of the production object-pool release.
		WTSObject::release();
	}
};

class TradeInfoDestructionProbe final : public WTSTradeInfo
{
public:
	~TradeInfoDestructionProbe() noexcept override
	{
		++g_trade_probe_destructions;
	}

	void release() override
	{
		// This probe is allocated with new, not WTSTradeInfo::create(). Route the
		// final reference through WTSObject::release() so delete invokes the
		// virtual probe destructor instead of the production object-pool release.
		WTSObject::release();
	}
};

void throwing_trade_publisher(WtUInt32 /*id*/, const char* /*topic*/,
	const char* /*data*/, WtUInt32 /*dataLen*/)
{
	++g_throwing_publisher_calls;
	throw std::runtime_error("R160 trade publisher failure");
}

void blocked_order_publisher(WtUInt32 /*id*/, const char* /*topic*/,
	const char* /*data*/, WtUInt32 /*dataLen*/)
{
	std::unique_lock<std::mutex> lock(g_blocked_publisher_mutex);
	g_blocked_publisher_entered = true;
	g_blocked_publisher_cv.notify_all();
	g_blocked_publisher_cv.wait(lock, [] { return g_blocked_publisher_released; });
	++g_blocked_publisher_calls;
}

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
    const uint32_t tradeRefBefore = trd->retainCount();
    const uint32_t orderRefBefore = ord->retainCount();

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
        for (int i = 0; i < 500 && (!all_delivered()
            || trd->retainCount() != tradeRefBefore
            || ord->retainCount() != orderRefBefore); ++i)
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
        EXPECT_EQ(tradeRefBefore, trd->retainCount())
            << "成交通知 handler 完成后必须释放其 retain 引用";
        EXPECT_EQ(orderRefBefore, ord->retainCount())
            << "订单通知 handler 完成后必须释放其 retain 引用";

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

TEST(R160EventNotifier, DiscardedOrderHandlerReleasesRetainedReference)
{
    WTSOrderInfo* ord = WTSOrderInfo::create();
    ASSERT_NE(nullptr, ord);
    const uint32_t orderRefBefore = ord->retainCount();

    {
        EventNotifier notifier;
        // Set only the guard needed to enqueue the order. No worker runs this
        // context, so destruction discards the queued handler deterministically.
        notifier._mq_sid = 1;
        notifier._accepting = true;
        notifier.notify("R160_TRADER", 12u, "SHFE.ag.2512", ord);
        EXPECT_EQ(orderRefBefore + 1, ord->retainCount());
        notifier._asyncio.stop();
    }

    EXPECT_EQ(orderRefBefore, ord->retainCount())
        << "被销毁的队列 handler 也必须释放订单通知引用";
    ord->release();
}

TEST(R160EventNotifier, AsyncOrderHandlerDestroysObjectOnceAfterPublisherReturns)
{
	g_order_probe_destructions = 0;
	g_blocked_publisher_calls = 0;
	{
		std::lock_guard<std::mutex> lock(g_blocked_publisher_mutex);
		g_blocked_publisher_entered = false;
		g_blocked_publisher_released = false;
	}

	EventNotifier notifier;
	notifier._mq_sid = 1;
	notifier._accepting = true;
	notifier._publisher = blocked_order_publisher;

	WTSOrderInfo* order = new OrderInfoDestructionProbe();
	ASSERT_NE(nullptr, order);
	order->setCode("ag2512");
	order->setExchange("SHFE");
	order->setDirection(WDT_SHORT);
	order->setOffsetType(WOT_OPEN);
	order->setOrderState(WOS_Canceled);
	order->setVolume(3);
	order->setVolTraded(1);
	order->setVolLeft(2);
	order->setPrice(3510.0);
	order->setStateMsg("canceled");

	notifier.notify("R160_TRADER", 12u, "SHFE.ag.2512", order);
	EXPECT_EQ(2u, order->retainCount());
	order->release(); // Drop the caller's reference; the queued handler owns the last one.

	std::thread handlerThread([&notifier] { notifier._asyncio.run_one(); });
	bool publisherEntered = false;
	{
		std::unique_lock<std::mutex> lock(g_blocked_publisher_mutex);
		publisherEntered = g_blocked_publisher_cv.wait_for(lock, std::chrono::seconds(2), [] {
			return g_blocked_publisher_entered;
		});
	}
	EXPECT_TRUE(publisherEntered) << "the queued order handler must reach the publisher";
	if (publisherEntered)
		EXPECT_EQ(0, g_order_probe_destructions.load())
			<< "the order remains alive until the asynchronous publisher returns";

	{
		std::lock_guard<std::mutex> lock(g_blocked_publisher_mutex);
		g_blocked_publisher_released = true;
	}
	g_blocked_publisher_cv.notify_all();
	handlerThread.join();

	EXPECT_EQ(1, g_blocked_publisher_calls.load());
	EXPECT_EQ(1, g_order_probe_destructions.load())
		<< "completion of the async handler must destroy its final order reference exactly once";
}

TEST(R160EventNotifier, DiscardedOrderHandlerDestroysLastReferenceOnce)
{
	g_order_probe_destructions = 0;
	{
		EventNotifier notifier;
		notifier._mq_sid = 1;
		notifier._accepting = true;
		auto* order = new OrderInfoDestructionProbe();
		notifier.notify("R160_TRADER", 12u, "SHFE.ag.2512", order);
		order->release();
		EXPECT_EQ(0, g_order_probe_destructions.load());
		notifier._asyncio.stop();
	}
	EXPECT_EQ(1, g_order_probe_destructions.load());
}

TEST(R160EventNotifier, ThrowingOrderPublisherDestroysLastReferenceOnce)
{
	g_order_probe_destructions = 0;
	g_throwing_publisher_calls = 0;
	EventNotifier notifier;
	notifier._mq_sid = 1;
	notifier._accepting = true;
	notifier._publisher = throwing_trade_publisher;
	auto* order = new OrderInfoDestructionProbe();
	notifier.notify("R160_TRADER", 12u, "SHFE.ag.2512", order);
	order->release();
	EXPECT_NO_THROW(notifier._asyncio.run_one());
	EXPECT_EQ(1, g_throwing_publisher_calls.load());
	EXPECT_EQ(1, g_order_probe_destructions.load());
}

TEST(R160EventNotifier, WorkerFinishesPendingOrderBeforeDestructorReturns)
{
	const std::string oldInst = WtHelper::getInstDir();
	struct RestoreInstDir {
		std::string value;
		~RestoreInstDir() { WtHelper::setInstDir(value.c_str()); }
	} restore{oldInst};
	WtHelper::setInstDir((std::string(WT_R160_FAKE_MQ_DIR) + "/").c_str());
	const std::string fakePath = std::string(WT_R160_FAKE_MQ_DIR) + "/" + DLLHelper::wrap_module("WtMsgQue", "lib");
	DllHandle inspector = DLLHelper::load_library(fakePath.c_str());
	ASSERT_NE(nullptr, inspector);
	using FakeReset = void(*)();
	using FakeCount = uint32_t(*)();
	const auto reset = (FakeReset)DLLHelper::get_symbol(inspector, "r160_fake_reset");
	const auto destroyCount = (FakeCount)DLLHelper::get_symbol(inspector, "r160_fake_destroy_count");
	ASSERT_NE(nullptr, reset);
	ASSERT_NE(nullptr, destroyCount);
	reset();
	WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"active\":true,\"url\":\"fake://r41_stop\"}");
	ASSERT_NE(nullptr, cfg);

	g_order_probe_destructions = 0;
	g_blocked_publisher_calls = 0;
	{
		std::lock_guard<std::mutex> lock(g_blocked_publisher_mutex);
		g_blocked_publisher_entered = false;
		g_blocked_publisher_released = false;
	}
	std::unique_ptr<EventNotifier> notifier(new EventNotifier());
	auto guard = boost::asio::make_work_guard(notifier->_asyncio);
	if (!notifier->init(cfg)) {
		guard.reset();
		cfg->release();
		FAIL() << "fake MQ initialization failed";
		return;
	}
	notifier->_publisher = blocked_order_publisher;
	auto* order = new OrderInfoDestructionProbe();
	notifier->notify("R160_TRADER", 12u, "SHFE.ag.2512", order);
	order->release();
	bool entered = false;
	{
		std::unique_lock<std::mutex> lock(g_blocked_publisher_mutex);
		entered = g_blocked_publisher_cv.wait_for(lock, std::chrono::seconds(2), [] {
			return g_blocked_publisher_entered;
		});
	}
	EXPECT_TRUE(entered);
	guard.reset();
	std::atomic<bool> destructorStarted{false}, destructorFinished{false};
	std::thread destroyer([&] {
		destructorStarted.store(true);
		notifier.reset();
		destructorFinished.store(true);
	});
	while (!destructorStarted.load()) std::this_thread::yield();
	if (entered) {
		EXPECT_FALSE(destructorFinished.load());
		EXPECT_EQ(0, g_order_probe_destructions.load());
		EXPECT_EQ(0u, destroyCount());
	}
	{
		std::lock_guard<std::mutex> lock(g_blocked_publisher_mutex);
		g_blocked_publisher_released = true;
	}
	g_blocked_publisher_cv.notify_all();
	destroyer.join();
	EXPECT_TRUE(destructorFinished.load());
	EXPECT_EQ(entered ? 1 : 0, g_blocked_publisher_calls.load());
	EXPECT_EQ(1, g_order_probe_destructions.load());
	EXPECT_EQ(1u, destroyCount());
	cfg->release();
	DLLHelper::free_library(inspector);
}

TEST(R160EventNotifier, DiscardedTradeHandlerReleasesRetainedReference)
{
	g_trade_probe_destructions = 0;
	auto* trade = new TradeInfoDestructionProbe();
	trade->setCode("ag2512");
	trade->setDirection(WDT_LONG);
	trade->setOffsetType(WOT_CLOSETODAY);
	trade->setVolume(2);
	trade->setPrice(3500.0);

	{
		EventNotifier notifier;
		notifier._mq_sid = 1;
		notifier._accepting = true;
		notifier.notify("R160_TRADER", 11u, "SHFE.ag.2512", trade);
		EXPECT_EQ(2u, trade->retainCount());
		notifier._asyncio.stop();
	}

	const uint32_t refsAfterDiscard = trade->retainCount();
	EXPECT_EQ(1u, refsAfterDiscard)
		<< "discarding the queued trade handler must release its retained reference";
	// Clean up the old implementation's leaked test reference after recording
	// the failure, while keeping the test process free of an intentional leak.
	if (refsAfterDiscard > 1)
		trade->release();
	trade->release();
	EXPECT_EQ(1, g_trade_probe_destructions.load());
}

TEST(R160EventNotifier, ThrowingTradePublisherReleasesRetainedReference)
{
	g_trade_probe_destructions = 0;
	g_throwing_publisher_calls = 0;
	auto* trade = new TradeInfoDestructionProbe();
	trade->setCode("ag2512");
	trade->setDirection(WDT_LONG);
	trade->setOffsetType(WOT_CLOSETODAY);
	trade->setVolume(2);
	trade->setPrice(3500.0);

	EventNotifier notifier;
	notifier._mq_sid = 1;
	notifier._accepting = true;
	notifier._publisher = throwing_trade_publisher;
	notifier.notify("R160_TRADER", 11u, "SHFE.ag.2512", trade);
	EXPECT_EQ(2u, trade->retainCount());

	bool publisherThrew = false;
	try
	{
		notifier._asyncio.run_one();
	}
	catch (const std::runtime_error&)
	{
		publisherThrew = true;
	}
	EXPECT_FALSE(publisherThrew) << "the notifier must isolate publisher exceptions";
	EXPECT_EQ(1, g_throwing_publisher_calls.load());

	const uint32_t refsAfterHandler = trade->retainCount();
	EXPECT_EQ(1u, refsAfterHandler)
		<< "handler unwinding after a publisher exception must release the trade reference";
	if (refsAfterHandler > 1)
		trade->release();
	trade->release();
	EXPECT_EQ(1, g_trade_probe_destructions.load());
}

TEST(R160EventNotifier, DeliversAfterIdleWithoutExternalWorkGuard)
{
	const std::string oldInst = WtHelper::getInstDir();
	struct Restore { std::string dir; ~Restore() { WtHelper::setInstDir(dir.c_str()); } } restore{oldInst};
	WtHelper::setInstDir((std::string(WT_R160_FAKE_MQ_DIR) + "/").c_str());
	WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"active\":true,\"url\":\"fake://r41_idle\"}");
	ASSERT_NE(nullptr, cfg);
	{
		std::lock_guard<std::mutex> lock(g_sequence_mutex);
		g_sequence.clear(); g_sequence_entered = false; g_sequence_released = true;
	}
	{
		EventNotifier notifier;
		ASSERT_TRUE(notifier.init(cfg));
		notifier._publisher = ordered_publisher;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		EXPECT_FALSE(notifier._asyncio.stopped());
		notifier.notify_event("after idle");
		std::unique_lock<std::mutex> lock(g_sequence_mutex);
		const bool delivered = g_sequence_cv.wait_for(lock, std::chrono::seconds(2), [] {
			return !g_sequence.empty();
		});
		EXPECT_TRUE(delivered);
		if (delivered) EXPECT_NE(std::string::npos, g_sequence.front().find("after idle"));
	}
	cfg->release();
}

TEST(R160EventNotifier, ThrowingPublisherIsReportedAndLaterItemRuns)
{
	const std::string oldInst = WtHelper::getInstDir();
	struct Restore { std::string dir; ~Restore() { WtHelper::setInstDir(dir.c_str()); } } restore{oldInst};
	WtHelper::setInstDir((std::string(WT_R160_FAKE_MQ_DIR) + "/").c_str());
	WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"active\":true,\"url\":\"fake://r41_throw\"}");
	ASSERT_NE(nullptr, cfg);
	g_exception_calls = 0; g_exception_successes = 0;
	std::unique_ptr<EventNotifier> notifier(new EventNotifier());
	ASSERT_TRUE(notifier->init(cfg));
	notifier->_publisher = throw_then_publish;
	testing::internal::CaptureStderr();
	notifier->notify_event("throws");
	notifier->notify_event("after failure");
	notifier.reset(); // Drain both accepted items before the fake server is destroyed.
	const std::string diagnostic = testing::internal::GetCapturedStderr();
	EXPECT_EQ(2, g_exception_calls.load());
	EXPECT_EQ(1, g_exception_successes.load());
	EXPECT_NE(std::string::npos, diagnostic.find("GRP_EVENT handler failed: injected publisher failure"));
	cfg->release();
}

TEST(R160EventNotifier, ConcurrentStopDrainsAcceptedItemsAndRejectsLaterItems)
{
	const std::string oldInst = WtHelper::getInstDir();
	struct Restore { std::string dir; ~Restore() { WtHelper::setInstDir(dir.c_str()); } } restore{oldInst};
	WtHelper::setInstDir((std::string(WT_R160_FAKE_MQ_DIR) + "/").c_str());
	WTSVariant* cfg = WTSCfgLoader::load_from_content("{\"active\":true,\"url\":\"fake://r41_drain\"}");
	ASSERT_NE(nullptr, cfg);
	{
		std::lock_guard<std::mutex> lock(g_sequence_mutex);
		g_sequence.clear(); g_sequence_entered = false; g_sequence_released = false;
	}
	std::unique_ptr<EventNotifier> notifier(new EventNotifier());
	ASSERT_TRUE(notifier->init(cfg));
	notifier->_publisher = ordered_publisher;
	notifier->notify_event("hold");
	bool entered;
	{
		std::unique_lock<std::mutex> lock(g_sequence_mutex);
		entered = g_sequence_cv.wait_for(lock, std::chrono::seconds(2), [] { return g_sequence_entered; });
	}
	if (!entered) {
		{ std::lock_guard<std::mutex> lock(g_sequence_mutex); g_sequence_released = true; }
		g_sequence_cv.notify_all(); notifier.reset(); cfg->release();
		FAIL() << "first handler did not reach the fake publisher";
		return;
	}
	constexpr int count = 200;
	std::atomic<bool> ready{false}, go{false};
	testing::internal::CaptureStderr();
	std::thread producer([&] {
		for (int i = 0; i < count; ++i) {
			if (i == 10) { ready.store(true); while (!go.load()) std::this_thread::yield(); }
			notifier->notify_event((std::string("seq-") + std::to_string(i)).c_str());
		}
	});
	while (!ready.load()) std::this_thread::yield();
	std::thread stopper([&] { while (!go.load()) std::this_thread::yield(); notifier->stopAccepting(); });
	go.store(true);
	producer.join(); stopper.join();
	notifier->notify_event("late"); // Explicit post-stop rejection.
	{
		std::lock_guard<std::mutex> lock(g_sequence_mutex);
		g_sequence_released = true;
	}
	g_sequence_cv.notify_all();
	notifier.reset();
	const std::string diagnostic = testing::internal::GetCapturedStderr();
	const std::string rejectedText = "EventNotifier rejected GRP_EVENT: stopping";
	std::size_t rejected = 0, pos = 0;
	while ((pos = diagnostic.find(rejectedText, pos)) != std::string::npos) { ++rejected; pos += rejectedText.size(); }
	std::lock_guard<std::mutex> lock(g_sequence_mutex);
	ASSERT_FALSE(g_sequence.empty());
	EXPECT_NE(std::string::npos, g_sequence.front().find("hold"));
	const std::size_t accepted = g_sequence.size() - 1;
	EXPECT_GE(accepted, 10u);
	EXPECT_GE(rejected, 1u);
	EXPECT_EQ(static_cast<std::size_t>(count + 1), accepted + rejected);
	for (std::size_t i = 0; i < accepted; ++i) {
		const std::size_t marker = g_sequence[i + 1].find("seq-");
		ASSERT_NE(std::string::npos, marker);
		EXPECT_EQ(static_cast<int>(i), std::stoi(g_sequence[i + 1].substr(marker + 4)));
	}
	cfg->release();
}
