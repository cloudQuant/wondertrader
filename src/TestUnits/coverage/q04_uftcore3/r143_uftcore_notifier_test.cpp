/*!
 * \file r143_uftcore_notifier_test.cpp
 * \brief 第二十二波 R143 - WtUftCore/EventNotifier 成功挂载 MQ 后的 JSON 组装
 *
 * 覆盖生产通知逻辑（通过本地发布回调探针验证异步投递，不创建外部 MQ 消费者）:
 *  - EventNotifier.cpp:119-140  notify_log：tag/time/message 三字段 JSON + LOG 主题发布
 *  - EventNotifier.cpp:148-166  notify_event：time/message JSON + GRP_EVENT 主题发布
 *  - EventNotifier.cpp:174-194  notify(trader, message)：trader/time/message JSON
 *  - EventNotifier.cpp:202-211  notify(trader, localid, stdCode, WTSTradeInfo*)：
 *    retain -> post -> tradeToJson -> publish -> release
 *  - EventNotifier.cpp:219-227  notify(trader, localid, stdCode, WTSOrderInfo*)：
 *    retain -> post -> orderToJson -> publish -> release
 *  - EventNotifier.cpp:230-303  tradeToJson/orderToJson 的字段与 private 组装面
 *
 * 边界:
 *  - URL 固定为进程内 `inproc://`，nanomsg 不创建任何 socket/IPC 文件；用 pid+序号
 *    保证同进程多次 init 不撞名。
 *  - init() 起一个 asio worker；测试发布回调以 condition_variable 同步，析构负责
 *    排空已投递任务并 join worker，不使用固定睡眠。
 *  - WtMsgQue 插件路径从测试可执行文件所在目录解析，仅 init 期间切换 InstDir。
 *  - 本用例不连柜台、不访问网络、不写文件。
 *
 */
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <memory>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/document.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSObject.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/StdUtils.hpp"

#define private public
#include "WtUftCore/EventNotifier.h"
#undef private
#include "WtUftCore/WtHelper.h"

#include "Includes/WTSTradeDef.hpp"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

using namespace wtp;

namespace {

static_assert(std::is_same<FuncCreateMQServer, WtUInt32(*)(const char*, bool)>::value,
	"UFT create_server function pointer must match WtMsgQue ABI");
static_assert(std::is_same<FuncDestroyMQServer, void(*)(WtUInt32)>::value,
	"UFT destroy_server function pointer must match WtMsgQue ABI");
static_assert(std::is_same<FundPublishMessage,
	void(*)(WtUInt32, const char*, const char*, WtUInt32)>::value,
	"UFT publish_message function pointer must match WtMsgQue ABI");
static_assert(std::is_same<FuncLogCallback,
	void(PORTER_FLAG *)(WtUInt32, const char*, bool)>::value,
	"UFT MQ log callback must match WtMsgQue ABI");

std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	std::filesystem::path p = std::filesystem::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }
	InstDirGuard(const InstDirGuard&) = delete;
	InstDirGuard& operator=(const InstDirGuard&) = delete;

private:
	std::string _old;
};

std::string nextUrl()
{
	static std::atomic<uint32_t> seq(0);
	return "inproc://r143_uft_notifier_" + std::to_string((long)::getpid())
		+ "_" + std::to_string(++seq);
}

struct PublishedMessage
{
	std::string topic;
	std::string data;
};

std::mutex g_publishMutex;
std::condition_variable g_publishChanged;
std::vector<PublishedMessage> g_publishedMessages;

class BlockingPublishGate
{
public:
	void publish(const char* topic)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_topics.push_back(topic == nullptr ? "" : topic);
		if (_topics.size() == 1)
		{
			_firstEntered = true;
			_changed.notify_all();
			_changed.wait(lock, [this]() { return _releaseFirst; });
		}
		_changed.notify_all();
	}

	bool waitForFirst()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _changed.wait_for(lock, std::chrono::seconds(2), [this]() { return _firstEntered; });
	}

	void releaseFirst()
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_releaseFirst = true;
		}
		_changed.notify_all();
	}

	bool hasTopic(const std::string& topic)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		for (const std::string& published : _topics)
			if (published == topic)
				return true;
		return false;
	}

private:
	std::mutex _mutex;
	std::condition_variable _changed;
	std::vector<std::string> _topics;
	bool _firstEntered = false;
	bool _releaseFirst = false;
};

BlockingPublishGate* g_blockingPublishGate = nullptr;

class ThrowingPublishGate
{
public:
	void throwOnOrder()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		_orderEntered = true;
		_changed.notify_all();
		_changed.wait(lock, [this]() { return _releaseOrder; });
		throw std::runtime_error("controlled order publish failure");
	}

	void markBarrier()
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_barrierSeen = true;
		}
		_changed.notify_all();
	}

	bool waitForOrder(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _changed.wait_for(lock, timeout, [this]() { return _orderEntered; });
	}

	void releaseOrder()
	{
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_releaseOrder = true;
		}
		_changed.notify_all();
	}

	bool waitForBarrier(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _changed.wait_for(lock, timeout, [this]() { return _barrierSeen; });
	}

private:
	std::mutex _mutex;
	std::condition_variable _changed;
	bool _orderEntered = false;
	bool _releaseOrder = false;
	bool _barrierSeen = false;
};

std::atomic<ThrowingPublishGate*> g_throwingPublishGate{ nullptr };

void capturePublish(uint32_t /*serverId*/, const char* topic, const char* data, uint32_t length)
{
	{
		std::lock_guard<std::mutex> lock(g_publishMutex);
		g_publishedMessages.push_back({ topic == nullptr ? "" : topic,
			data == nullptr ? "" : std::string(data, length) });
	}
	g_publishChanged.notify_all();
}

void blockFirstPublish(uint32_t /*serverId*/, const char* topic, const char* /*data*/, uint32_t /*length*/)
{
	if (g_blockingPublishGate != nullptr)
		g_blockingPublishGate->publish(topic);
}

void throwOnOrderPublish(uint32_t /*serverId*/, const char* topic, const char* /*data*/, uint32_t /*length*/)
{
	ThrowingPublishGate* gate = g_throwingPublishGate.load(std::memory_order_acquire);
	if (gate == nullptr || topic == nullptr)
		return;

	if (std::string(topic) == "TRD_ORDER")
		gate->throwOnOrder();
	else if (std::string(topic) == "TRD_NOTIFY")
		gate->markBarrier();
}

void resetPublishedMessages()
{
	std::lock_guard<std::mutex> lock(g_publishMutex);
	g_publishedMessages.clear();
}

bool waitForTopics(const std::vector<std::string>& topics)
{
	std::unique_lock<std::mutex> lock(g_publishMutex);
	return g_publishChanged.wait_for(lock, std::chrono::seconds(2), [&topics]() {
		for (const std::string& topic : topics)
		{
			bool found = false;
			for (const PublishedMessage& message : g_publishedMessages)
				found = found || message.topic == topic;
			if (!found)
				return false;
		}
		return true;
	});
}

std::string publishedData(const std::string& topic)
{
	std::lock_guard<std::mutex> lock(g_publishMutex);
	for (const PublishedMessage& message : g_publishedMessages)
		if (message.topic == topic)
			return message.data;
	return std::string();
}

bool makeNotifier(EventNotifier& notifier, WTSVariant*& cfg)
{
	cfg = WTSVariant::createObject();
	cfg->append("active", true);
	cfg->append("url", nextUrl().c_str());
	return notifier.init(cfg);
}

} // namespace

TEST(R160UftNotifier, FakeMqReceivesDefaultConfirmAndIsDestroyed)
{
	const std::string fakeDir = WT_UFT_R160_FAKE_MQ_DIR;
	const std::string moduleName = DLLHelper::wrap_module("WtMsgQue", "lib");
	const std::string fakePath = fakeDir + "/" + moduleName;
	InstDirGuard instdir(fakeDir + "/");

	DllHandle inspector = DLLHelper::load_library(fakePath.c_str());
	ASSERT_NE(nullptr, inspector);
	struct InspectorGuard
	{
		DllHandle handle;
		~InspectorGuard() { DLLHelper::free_library(handle); }
	} inspectorGuard{ inspector };

	using Reset = void(*)();
	using Count = uint32_t(*)();
	using ServerId = WtUInt32(*)();
	using Flag = bool(*)();
	using Text = const char*(*)();
	const auto reset = reinterpret_cast<Reset>(DLLHelper::get_symbol(inspector, "r160_fake_reset"));
	const auto createCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector, "r160_fake_create_count"));
	const auto destroyCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector, "r160_fake_destroy_count"));
	const auto serverId = reinterpret_cast<ServerId>(DLLHelper::get_symbol(inspector, "r160_fake_server_id"));
	const auto destroyedId = reinterpret_cast<ServerId>(DLLHelper::get_symbol(inspector, "r160_fake_destroyed_id"));
	const auto confirm = reinterpret_cast<Flag>(DLLHelper::get_symbol(inspector, "r160_fake_confirm"));
	const auto observedUrl = reinterpret_cast<Text>(DLLHelper::get_symbol(inspector, "r160_fake_url"));
	ASSERT_NE(nullptr, reset);
	ASSERT_NE(nullptr, createCount);
	ASSERT_NE(nullptr, destroyCount);
	ASSERT_NE(nullptr, serverId);
	ASSERT_NE(nullptr, destroyedId);
	ASSERT_NE(nullptr, confirm);
	ASSERT_NE(nullptr, observedUrl);
	reset();

	constexpr WtUInt32 expectedId = 0xF1234567u;
	const std::string url = "fake://r160_uft_abi";
	struct VariantGuard
	{
		WTSVariant* value;
		~VariantGuard() { if (value != nullptr) value->release(); }
	} cfg{ WTSVariant::createObject() };
	ASSERT_NE(nullptr, cfg.value);
	cfg.value->append("active", true);
	cfg.value->append("url", url.c_str());

	{
		EventNotifier notifier;
		ASSERT_TRUE(notifier.init(cfg.value));
		EXPECT_EQ(expectedId, notifier._mq_sid);
		EXPECT_EQ(1u, createCount());
		EXPECT_EQ(expectedId, serverId());
		EXPECT_STREQ(url.c_str(), observedUrl());
		// The UFT notifier config has no confirm option; it must pass the MQ default explicitly.
		EXPECT_FALSE(confirm());
	}

	EXPECT_EQ(1u, destroyCount());
	EXPECT_EQ(expectedId, destroyedId());
}

// EventNotifier.cpp:119-140 + :148-166 + :174-194
TEST(R143UftNotifier, postsTextLogEventAndTraderMessages)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	InstDirGuard instdir(bin);
	EventNotifier notifier;
	WTSVariant* cfg = nullptr;
	ASSERT_TRUE(makeNotifier(notifier, cfg));
	cfg->release();
	resetPublishedMessages();
	notifier._publisher = &capturePublish;

	notifier.notify_log("R143", "log line");
	notifier.notify_event("event line");
	notifier.notify("uft1", "trader text line");
	EXPECT_TRUE(waitForTopics({ "LOG", "GRP_EVENT", "TRD_NOTIFY" }));
	EXPECT_NE(std::string::npos, publishedData("LOG").find("log line"));
	EXPECT_NE(std::string::npos, publishedData("GRP_EVENT").find("event line"));
	EXPECT_NE(std::string::npos, publishedData("TRD_NOTIFY").find("trader text line"));
}

// EventNotifier.cpp:202-211 + :230-263
TEST(R143UftNotifier, postsTradeJsonAndReleasesRetainedPayload)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	trd->setDirection(WDT_LONG);
	trd->setOffsetType(WOT_CLOSETODAY);
	trd->setVolume(3.0);
	trd->setPrice(5123.5);
	ASSERT_EQ(1u, trd->retainCount());

	bool published = false;
	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();
		resetPublishedMessages();
		notifier._publisher = &capturePublish;

		notifier.notify("uft1", 4001, "SHFE.ag2406", trd);
		published = waitForTopics({ "TRD_TRADE" });
	}

	EXPECT_TRUE(published);
	EXPECT_NE(std::string::npos, publishedData("TRD_TRADE").find("SHFE.ag2406"));
	// worker join 后，异步通知持有的引用必须已经释放。
	EXPECT_EQ(1u, trd->retainCount());
	trd->release();
}

// EventNotifier.cpp:219-227 + :265-303
TEST(R143UftNotifier, postsOrderJsonWithStateAndVolumeFields)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	ord->setDirection(WDT_SHORT);
	ord->setOffsetType(WOT_OPEN);
	ord->setOrderState(WOS_Canceled);
	ord->setStateMsg("cancelled");
	ord->setVolume(6.0);
	ord->setVolLeft(2.0);
	ord->setVolTraded(4.0);
	ord->setPrice(4988.0);
	ASSERT_EQ(1u, ord->retainCount());

	bool published = false;
	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();
		resetPublishedMessages();
		notifier._publisher = &capturePublish;

		notifier.notify("uft1", 4002, "SHFE.ag2406", ord);
		published = waitForTopics({ "TRD_ORDER" });
	}

	EXPECT_TRUE(published);
	const std::string payload = publishedData("TRD_ORDER");
	EXPECT_NE(std::string::npos, payload.find("cancelled"));
	EXPECT_NE(std::string::npos, payload.find("total"));
	EXPECT_NE(std::string::npos, payload.find("left"));
	EXPECT_NE(std::string::npos, payload.find("traded"));
	EXPECT_EQ(1u, ord->retainCount()) << "TRD_ORDER handler must release its retained order after worker join";
	ord->release();
}

TEST(R143UftNotifier, releasesRetainedOrderWhenPublisherIsMissing)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	ASSERT_EQ(1u, ord->retainCount());

	std::promise<void> drained;
	std::future<void> drainedFuture = drained.get_future();
	bool orderHandlerDrained = false;
	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();
		notifier._publisher = nullptr;

		notifier.notify("uft1", 4006, "SHFE.ag2406", ord);
		// The single worker runs this barrier after the earlier order handler.
		boost::asio::post(notifier._asyncio, [&drained]() { drained.set_value(); });
		orderHandlerDrained = drainedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		// stopWorker joins after the barrier and drains any remaining posted work.
		notifier.stopWorker();
	}

	EXPECT_TRUE(orderHandlerDrained);
	EXPECT_EQ(1u, ord->retainCount());
	ord->release();
}

TEST(R143UftNotifier, releasesRetainedOrderWhenJsonWriterRejectsNonFinitePrice)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	const double nonFinitePrice = std::numeric_limits<double>::quiet_NaN();
	rapidjson::StringBuffer serialized;
	rapidjson::Writer<rapidjson::StringBuffer> writer(serialized);
	ASSERT_FALSE(writer.Double(nonFinitePrice)) << "the default RapidJSON writer must reject NaN";

	auto releaseOrder = [](WTSOrderInfo* order) {
		if (order != nullptr)
			order->release();
	};
	std::unique_ptr<WTSOrderInfo, decltype(releaseOrder)> ord(WTSOrderInfo::create(), releaseOrder);
	ASSERT_NE(nullptr, ord.get());
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	ord->setPrice(nonFinitePrice);
	ASSERT_EQ(1u, ord->retainCount());

	std::promise<void> drained;
	std::future<void> drainedFuture = drained.get_future();
	bool orderHandlerDrained = false;
	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();
		// The serialization path still runs, but no partial output is observed or
		// made part of this ownership regression's expected behavior.
		notifier._publisher = nullptr;

		notifier.notify("uft1", 4008, "SHFE.ag2406", ord.get());
		boost::asio::post(notifier._asyncio, [&drained]() { drained.set_value(); });
		orderHandlerDrained = drainedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		notifier.stopWorker();
	}

	EXPECT_TRUE(orderHandlerDrained);
	EXPECT_EQ(1u, ord->retainCount());
}

TEST(R143UftNotifier, PublishesOnlyCompleteJsonForFiniteTradeAndOrderValues)
{
	resetPublishedMessages();
	EventNotifier notifier;
	notifier._mq_sid = 42;
	notifier._publisher = &capturePublish;

	const double nonFinite = std::numeric_limits<double>::quiet_NaN();
	WTSTradeInfo* finiteTrade = WTSTradeInfo::create("ag2406", "SHFE");
	WTSTradeInfo* invalidTrade = WTSTradeInfo::create("ag2406", "SHFE");
	WTSOrderInfo* finiteOrder = WTSOrderInfo::create();
	WTSOrderInfo* invalidOrder = WTSOrderInfo::create();
	ASSERT_NE(nullptr, finiteTrade);
	ASSERT_NE(nullptr, invalidTrade);
	ASSERT_NE(nullptr, finiteOrder);
	ASSERT_NE(nullptr, invalidOrder);

	finiteTrade->setVolume(2.0);
	finiteTrade->setPrice(3500.25);
	invalidTrade->setVolume(2.0);
	invalidTrade->setPrice(nonFinite);
	finiteOrder->setCode("ag2406");
	finiteOrder->setExchange("SHFE");
	finiteOrder->setVolume(3.0);
	finiteOrder->setPrice(3510.75);
	invalidOrder->setCode("ag2406");
	invalidOrder->setExchange("SHFE");
	invalidOrder->setVolume(3.0);
	invalidOrder->setPrice(nonFinite);

	notifier.notify("r143", 1, "SHFE.ag2406", finiteTrade);
	notifier.notify("r143", 2, "SHFE.ag2406", invalidTrade);
	notifier.notify("r143", 3, "SHFE.ag2406", finiteOrder);
	notifier.notify("r143", 4, "SHFE.ag2406", invalidOrder);
	notifier._asyncio.poll();

	ASSERT_EQ(2u, g_publishedMessages.size()) << "failed writer output must not reach the publisher";
	EXPECT_EQ("TRD_TRADE", g_publishedMessages[0].topic);
	EXPECT_EQ("TRD_ORDER", g_publishedMessages[1].topic);
	for (const PublishedMessage& message : g_publishedMessages)
	{
		rapidjson::Document parsed;
		parsed.Parse(message.data.c_str());
		ASSERT_FALSE(parsed.HasParseError()) << message.data;
		ASSERT_TRUE(parsed.IsObject()) << message.data;
		ASSERT_TRUE(parsed.HasMember("price")) << message.data;
		EXPECT_TRUE(std::isfinite(parsed["price"].GetDouble()));
		if (message.topic == "TRD_TRADE")
			EXPECT_DOUBLE_EQ(3500.25, parsed["price"].GetDouble());
		else
			EXPECT_DOUBLE_EQ(3510.75, parsed["price"].GetDouble());
	}
	EXPECT_EQ(1u, finiteTrade->retainCount());
	EXPECT_EQ(1u, invalidTrade->retainCount());
	EXPECT_EQ(1u, finiteOrder->retainCount());
	EXPECT_EQ(1u, invalidOrder->retainCount());

	finiteTrade->release();
	invalidTrade->release();
	finiteOrder->release();
	invalidOrder->release();
}

TEST(R143UftNotifier, releasesRetainedOrderWhenPublisherThrows)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	ASSERT_EQ(1u, ord->retainCount());

	ThrowingPublishGate gate;
	bool orderPublisherEntered = false;
	bool barrierPassed = false;
	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();
		notifier._publisher = &throwOnOrderPublish;
		g_throwingPublishGate.store(&gate, std::memory_order_release);

		notifier.notify("uft1", 4007, "SHFE.ag2406", ord);
		orderPublisherEntered = gate.waitForOrder(std::chrono::seconds(2));
		// Queue a barrier behind the throwing order handler so the worker must
		// recover from the exception and finish destroying the captured owner.
		notifier.notify("uft1", "exception recovery barrier");
		gate.releaseOrder();
		barrierPassed = gate.waitForBarrier(std::chrono::seconds(2));
		notifier.stopWorker();
	}
	g_throwingPublishGate.store(nullptr, std::memory_order_release);

	EXPECT_TRUE(orderPublisherEntered);
	EXPECT_TRUE(barrierPassed);
	EXPECT_EQ(1u, ord->retainCount());
	ord->release();
}

TEST(R143UftNotifier, ShutdownWaitsForInflightPublishAndDrainsQueuedOrder)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	BlockingPublishGate gate;
	g_blockingPublishGate = &gate;
	bool initSucceeded = false;
	bool firstHandlerEntered = false;
	bool shutdownWaitedForInflightHandler = false;
	bool shutdownCompleted = false;
	bool queuedOrderPublished = false;
	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		initSucceeded = makeNotifier(notifier, cfg);
		if (cfg != nullptr)
			cfg->release();
		if (initSucceeded)
		{
			notifier._publisher = &blockFirstPublish;
			notifier.notify("uft1", "hold the worker in publish");
			firstHandlerEntered = gate.waitForFirst();
			notifier.notify("uft1", 4005, "SHFE.ag2406", ord);

			std::promise<void> shutdownStarted;
			std::promise<void> shutdownFinished;
			std::future<void> shutdownStartedFuture = shutdownStarted.get_future();
			std::future<void> shutdownFinishedFuture = shutdownFinished.get_future();
			std::thread shutdown([&]() {
				shutdownStarted.set_value();
				notifier.stopWorker();
				shutdownFinished.set_value();
			});
			shutdownStartedFuture.wait();
			shutdownWaitedForInflightHandler =
				shutdownFinishedFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
			gate.releaseFirst();
			shutdownCompleted = shutdownFinishedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
			if (!shutdownCompleted)
				notifier._asyncio.stop(); // bounded cleanup if a regression leaves run() blocked
			shutdown.join();
			queuedOrderPublished = gate.hasTopic("TRD_ORDER");
		}
	}
	g_blockingPublishGate = nullptr;

	EXPECT_TRUE(initSucceeded);
	EXPECT_TRUE(firstHandlerEntered);
	EXPECT_TRUE(shutdownWaitedForInflightHandler);
	EXPECT_TRUE(shutdownCompleted);
	EXPECT_TRUE(queuedOrderPublished) << "shutdown must drain a queued notifier callback before join completes";
	EXPECT_EQ(1u, ord->retainCount());
	ord->release();
}

// EventNotifier.cpp:197-200 + :214-217：MQ 已挂载时空载荷仍必须在 retain/post 之前返回
TEST(R143UftNotifier, nullPayloadIsDroppedBeforeRetainWhenMqIsActive)
{
	const std::string bin = binDir();
	if (bin.empty() || !std::filesystem::exists(bin + "libWtMsgQue.dylib"))
		GTEST_SKIP() << "libWtMsgQue.dylib not found in this build";

	WTSTradeInfo* trd = WTSTradeInfo::create("ag2406", "SHFE");
	ASSERT_NE(nullptr, trd);
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(nullptr, ord);
	const uint32_t trdRefs = trd->retainCount();
	const uint32_t ordRefs = ord->retainCount();

	{
		InstDirGuard instdir(bin);
		EventNotifier notifier;
		WTSVariant* cfg = nullptr;
		ASSERT_TRUE(makeNotifier(notifier, cfg));
		cfg->release();

		notifier.notify("uft1", 4003, "SHFE.ag2406", (WTSTradeInfo*)nullptr);
		notifier.notify("uft1", 4004, "SHFE.ag2406", (WTSOrderInfo*)nullptr);
		EXPECT_EQ(trdRefs, trd->retainCount());
		EXPECT_EQ(ordRefs, ord->retainCount());
	}

	trd->release();
	ord->release();
}
