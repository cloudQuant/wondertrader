#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Includes/IParserApi.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "../q04_uftcore3/q04_trader_fixture.h"

#define private public
#include "WtUftRunner/WtUftRunner.h"
#undef private

using namespace wtp;

namespace
{
struct ParserStartStats
{
	std::atomic<uint32_t> _connect_count{0};
	std::atomic<uint32_t> _release_count{0};
	std::atomic<uint32_t> _destroy_count{0};
};

struct ParserTeardownStats
{
	std::atomic<uint32_t> _release_count{0};
	std::atomic<uint32_t> _remover_count{0};
	std::atomic<uint32_t> _destroy_count{0};
	std::atomic<bool> _provider_quiesced{false};
};

class UftRunnerBlockingParserApi : public IParserApi
{
public:
	explicit UftRunnerBlockingParserApi(std::shared_ptr<ParserStartStats> stats) : _stats(std::move(stats)) {}
	~UftRunnerBlockingParserApi() override { ++_stats->_destroy_count; }

	bool init(WTSVariant*) override { return true; }
	void release() override { ++_stats->_release_count; }
	bool connect() override
	{
		++_stats->_connect_count;
		if (_connect_hook)
			_connect_hook();
		if (_spi_callback && _spi != nullptr)
		{
			std::thread callback([this]() { _spi_callback(_spi); });
			callback.join();
		}
		std::unique_lock<std::mutex> lock(_mutex);
		_entered = true;
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _continue; });
		return true;
	}

	bool waitUntilConnectEntered()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, std::chrono::seconds(3), [this]() { return _entered; });
	}
	void allowConnectToReturn()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_continue = true;
		_condition.notify_all();
	}
	void setConnectHook(std::function<void()> hook) { _connect_hook = std::move(hook); }
	void setSpiCallback(std::function<void(IParserSpi*)> callback) { _spi_callback = std::move(callback); }
	void registerSpi(IParserSpi* spi) override { _spi = spi; }

private:
	std::shared_ptr<ParserStartStats> _stats;
	IParserSpi* _spi{nullptr};
	std::function<void()> _connect_hook;
	std::function<void(IParserSpi*)> _spi_callback;
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered{false};
	bool _continue{false};
};

class UftRunnerRetryableReleaseParserApi : public IParserApi
{
public:
	explicit UftRunnerRetryableReleaseParserApi(std::shared_ptr<ParserTeardownStats> stats) : _stats(std::move(stats)) {}
	~UftRunnerRetryableReleaseParserApi() override { ++_stats->_destroy_count; }
	bool init(WTSVariant*) override { return true; }
	void release() override { ++_stats->_release_count; }
	void registerSpi(IParserSpi*) override {}

	std::shared_ptr<ParserTeardownStats> _stats;
};

// Card 9 (BUG-R78-001): an external parser API is destroyed only with a
// provider confirmation; these quiescent fakes confirm through this remover.
void confirmedParserRemover(IParserApi*& api)
{
	IParserApi* original = api;
	api = nullptr;
	delete original;
}

void retryableParserRemover(IParserApi*& api)
{
	if (api == nullptr)
		return;
	auto* ownedApi = static_cast<UftRunnerRetryableReleaseParserApi*>(api);
	++ownedApi->_stats->_remover_count;
	if (!ownedApi->_stats->_provider_quiesced.load(std::memory_order_acquire))
		return;
	api = nullptr;
	delete ownedApi;
}

class UftRunnerShutdownQuoteStub : public IParserStub
{
public:
	explicit UftRunnerShutdownQuoteStub(WtUftRunner* runner) : _runner(runner) {}
	void handle_push_quote(WTSTickData*) override
	{
		const bool result = _runner->shutdown();
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_shutdown_result.store(result, std::memory_order_release);
			_called.store(true, std::memory_order_release);
		}
		_condition.notify_all();
	}
	bool waitUntilCalled()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, std::chrono::seconds(3), [this]() {
			return _called.load(std::memory_order_acquire);
		});
	}
	bool shutdownResult() const { return _shutdown_result.load(std::memory_order_acquire); }

private:
	WtUftRunner* _runner;
	std::atomic<bool> _called{false};
	std::atomic<bool> _shutdown_result{true};
	std::mutex _mutex;
	std::condition_variable _condition;
};

#if !defined(_WIN32)
int runParserCallbackShutdownScenario()
{
	q04::FakeBaseDataMgr baseData;
	baseData.add("SHFE", "ag", "ag2406");
	WtUftRunner runner;
	UftRunnerShutdownQuoteStub stub(&runner);
	auto stats = std::make_shared<ParserStartStats>();
	UftRunnerBlockingParserApi* api = new UftRunnerBlockingParserApi(stats);
	api->setSpiCallback([](IParserSpi* spi) {
		WTSTickStruct tickStruct{};
		wt_strcpy(tickStruct.exchg, "SHFE");
		wt_strcpy(tickStruct.code, "ag2406");
		tickStruct.action_date = 20260430;
		tickStruct.trading_date = 20260430;
		tickStruct.price = 7000.5;
		WTSTickData* tick = WTSTickData::create(tickStruct);
		spi->handleQuote(tick, 0);
		tick->release();
	});
	ParserAdapterPtr adapter(new ParserAdapter());
	if (!adapter->initExt("cross_thread_callback", api, &stub, &baseData, confirmedParserRemover))
		return 1;
	if (!runner._parsers.addAdapter("cross_thread_callback", adapter))
		return 2;

	std::atomic<bool> start_result{false};
	std::thread starter([&]() {
		start_result.store(runner.startParsers(), std::memory_order_release);
	});
	if (!stub.waitUntilCalled())
	{
		// In the broken implementation the callback blocks in shutdown while
		// connect waits in callback.join(). Do not unwind a joinable thread here:
		// the parent test process will observe this bounded failure and reap us.
		::_exit(10);
	}
	if (stub.shutdownResult())
	{
		api->allowConnectToReturn();
		starter.join();
		return 3;
	}
	if (!api->waitUntilConnectEntered())
	{
		::_exit(11);
	}
	if (start_result.load(std::memory_order_acquire))
	{
		api->allowConnectToReturn();
		starter.join();
		return 4;
	}

	api->allowConnectToReturn();
	starter.join();
	if (!start_result.load(std::memory_order_acquire))
		return 5;
	if (!runner.shutdown())
		return 6;
	// 决策 K18=否（2026-11-04）：带 remover 的外部 parser 同无 remover：不调用 release/remover、不删除，隔离到进程退出；管理器将其作为常驻对象移出，关停可完成。
	if (stats->_release_count.load(std::memory_order_acquire) != 0u ||
		stats->_destroy_count.load(std::memory_order_acquire) != 0u)
		return 7;
	return 0;
}
#endif

class UftRunnerReadyApi : public ITraderApi
{
public:
	bool init(WTSVariant*) override { return true; }
	void registerSpi(ITraderSpi* spi) override { _spi = spi; }
	void release() override { ++_release_count; }
	void connect() override
	{
		if (_spi != nullptr)
			_spi->onLoginResult(true, "ok", 0);
	}
	int queryPositions() override
	{
		WTSArray* empty = WTSArray::create();
		_spi->onRspPosition(empty);
		empty->release();
		return 0;
	}
	int queryOrders() override
	{
		WTSArray* empty = WTSArray::create();
		_spi->onRspOrders(empty);
		empty->release();
		return 0;
	}
	int queryTrades() override
	{
		WTSArray* empty = WTSArray::create();
		_spi->onRspTrades(empty);
		empty->release();
		return 0;
	}
	int queryAccount() override
	{
		WTSArray* empty = WTSArray::create();
		_spi->onRspAccount(empty);
		empty->release();
		return 0;
	}

	std::atomic<uint32_t> _release_count{0};

private:
	ITraderSpi* _spi{nullptr};
};

class UftRunnerGatedContext : public UftStraContext
{
public:
	UftRunnerGatedContext(WtUftEngine* engine) : UftStraContext(engine, "shutdown_test"), _engine(engine) {}

	void on_channel_ready(uint32_t) override
	{
		_owner_visible.store(_engine->getContext(id()).get() == this, std::memory_order_release);
		std::unique_lock<std::mutex> lock(_mutex);
		_entered = true;
		_condition.notify_all();
		_condition.wait(lock, [this]() { return _continue; });
	}

	bool waitUntilEntered()
	{
		std::unique_lock<std::mutex> lock(_mutex);
		return _condition.wait_for(lock, std::chrono::seconds(3), [this]() { return _entered; });
	}

	void allowCallbackToReturn()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_continue = true;
		_condition.notify_all();
	}

	bool ownerWasVisible() const { return _owner_visible.load(std::memory_order_acquire); }

private:
	WtUftEngine* _engine;
	std::atomic<bool> _owner_visible{false};
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered{false};
	bool _continue{false};
};
}

TEST(WtUftRunnerSinkShutdown, drainsReadyCallbackBeforeReleasingTraderAndContext)
{
	UftRunnerReadyApi api;
	WtUftRunner runner;
	TraderAdapterPtr adapter(new TraderAdapter());
	ASSERT_TRUE(adapter->initExt("shutdown", &api, nullptr, nullptr));
	ASSERT_TRUE(runner._traders.addAdapter("shutdown", adapter));

	std::shared_ptr<UftRunnerGatedContext> context(new UftRunnerGatedContext(&runner._uft_engine));
	context->setTrader(adapter.get());
	runner._uft_engine.addContext(UftContextPtr(context));
	adapter->addSink(context.get());

	std::thread traderThread([&]() { EXPECT_TRUE(adapter->run()); });
	const bool callbackEntered = context->waitUntilEntered();
	if (!callbackEntered)
	{
		context->allowCallbackToReturn();
		traderThread.join();
		FAIL() << "fake trader did not reach a READY sink callback";
	}
	EXPECT_TRUE(context->ownerWasVisible());

	std::atomic<bool> shutdown_done{false};
	std::thread shutdownThread([&]() {
		EXPECT_TRUE(runner.shutdown());
		shutdown_done.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	EXPECT_FALSE(shutdown_done.load(std::memory_order_acquire));
	EXPECT_EQ(api._release_count.load(std::memory_order_acquire), 0u);
	EXPECT_NE(runner._uft_engine.getContext(context->id()).get(), nullptr);

	context->allowCallbackToReturn();
	traderThread.join();
	shutdownThread.join();
	EXPECT_TRUE(shutdown_done.load(std::memory_order_acquire));
	EXPECT_EQ(api._release_count.load(std::memory_order_acquire), 1u);
	EXPECT_NE(runner._uft_engine.getContext(context->id()).get(), nullptr);
	std::signal(SIGINT, SIG_DFL);
}

TEST(WtUftRunnerParserOwnership, duplicateRegistrationKeepsInitializedCandidateQuarantined)
{
	q04::FakeBaseDataMgr baseData;
	WtUftRunner runner;
	ParserAdapterPtr registered(new ParserAdapter());
	runner._parsers._adapters["duplicate"] = registered;

	auto stats = std::make_shared<ParserStartStats>();
	UftRunnerBlockingParserApi* api = new UftRunnerBlockingParserApi(stats);
	ParserAdapterPtr candidate(new ParserAdapter());
	ASSERT_TRUE(candidate->initExt("duplicate", api, nullptr, &baseData, confirmedParserRemover));
	runner._quarantined_parsers.push_back(candidate);

	EXPECT_FALSE(runner.registerParserCandidate("duplicate", candidate));
	EXPECT_TRUE(candidate->isInitialized());
	EXPECT_TRUE(candidate->hasApi());
	EXPECT_EQ(runner._quarantined_parsers.size(), 1u);
	EXPECT_TRUE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
	EXPECT_EQ(runner._parsers.getAdapter("duplicate").get(), registered.get());
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);

	// This fake never starts a worker or schedules a callback; the test's
	// synchronous scope is its provider-specific quiescence barrier.
	candidate->release();
	runner._quarantined_parsers.clear();
	runner._parser_quarantine_pending.store(false, std::memory_order_release);
	// 决策 K18=否（2026-11-04）：带 remover 的外部 parser 同无 remover：不调用 release/remover、不删除，隔离到进程退出；管理器将其作为常驻对象移出，关停可完成。
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);
	EXPECT_EQ(stats->_destroy_count.load(std::memory_order_acquire), 0u);
	EXPECT_TRUE(runner.shutdown());
}

TEST(WtUftRunnerParserOwnership, duplicateIdIsRejectedBeforePluginInitialization)
{
	WtUftRunner runner;
	ParserAdapterPtr registered(new ParserAdapter());
	runner._parsers._adapters["duplicate"] = registered;

	WTSVariant* parsers = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", "duplicate");
	item->append("module", "no_such_parser_module");
	ASSERT_TRUE(parsers->append(item, false));

	EXPECT_FALSE(runner.initParsers(parsers));
	EXPECT_EQ(runner._parsers.getAdapter("duplicate").get(), registered.get());
	EXPECT_TRUE(runner._quarantined_parsers.empty());
	EXPECT_FALSE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
	parsers->release();
}

TEST(WtUftRunnerParserStart, shutdownWaitsForParserStartAndDoesNotHoldGateLockInPlugin)
{
	q04::FakeBaseDataMgr baseData;
	WtUftRunner runner;
	auto stats = std::make_shared<ParserStartStats>();
	UftRunnerBlockingParserApi* api = new UftRunnerBlockingParserApi(stats);
	ParserAdapterPtr adapter(new ParserAdapter());
	ASSERT_TRUE(adapter->initExt("blocking", api, nullptr, &baseData, confirmedParserRemover));
	ASSERT_TRUE(runner._parsers.addAdapter("blocking", adapter));

	std::atomic<bool> start_result{false};
	std::thread starter([&]() {
		start_result.store(runner.startParsers(), std::memory_order_release);
	});
	if (!api->waitUntilConnectEntered())
	{
		api->allowConnectToReturn();
		starter.join();
		FAIL() << "fake parser did not enter connect";
	}

	std::atomic<bool> shutdown_done{false};
	std::atomic<bool> shutdown_result{false};
	std::thread stopper([&]() {
		shutdown_result.store(runner.shutdown(), std::memory_order_release);
		shutdown_done.store(true, std::memory_order_release);
	});
	bool admissionClosed = false;
	{
		std::unique_lock<std::mutex> lock(runner._parser_start_mutex);
		admissionClosed = runner._parser_start_cv.wait_for(lock, std::chrono::seconds(3), [&]() {
			return runner._parser_start_closed;
		});
		if (admissionClosed)
		{
			EXPECT_TRUE(runner._parser_start_active);
			EXPECT_EQ(runner._parser_start_owner, starter.get_id());
		}
	}
	if (!admissionClosed)
	{
		api->allowConnectToReturn();
		starter.join();
		stopper.join();
		FAIL() << "shutdown did not close parser-start admission";
	}
	EXPECT_FALSE(shutdown_done.load(std::memory_order_acquire));
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);
	const bool startGateLockAvailable = runner._parser_start_mutex.try_lock();
	EXPECT_TRUE(startGateLockAvailable);
	if (startGateLockAvailable)
		runner._parser_start_mutex.unlock();

	api->allowConnectToReturn();
	starter.join();
	stopper.join();
	EXPECT_TRUE(start_result.load(std::memory_order_acquire));
	EXPECT_TRUE(shutdown_done.load(std::memory_order_acquire));
	EXPECT_TRUE(shutdown_result.load(std::memory_order_acquire));
	// 决策 K18=否（2026-11-04）：带 remover 的外部 parser 同无 remover：不调用 release/remover、不删除，隔离到进程退出；管理器将其作为常驻对象移出，关停可完成。
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);
	EXPECT_EQ(stats->_destroy_count.load(std::memory_order_acquire), 0u);
}

TEST(WtUftRunnerParserStart, sameThreadShutdownReentryDoesNotWaitOnItself)
{
	q04::FakeBaseDataMgr baseData;
	WtUftRunner runner;
	auto stats = std::make_shared<ParserStartStats>();
	UftRunnerBlockingParserApi* api = new UftRunnerBlockingParserApi(stats);
	api->allowConnectToReturn();
	ParserAdapterPtr adapter(new ParserAdapter());
	ASSERT_TRUE(adapter->initExt("reentrant", api, nullptr, &baseData, confirmedParserRemover));
	ASSERT_TRUE(runner._parsers.addAdapter("reentrant", adapter));

	bool reentrant_shutdown_result = true;
	api->setConnectHook([&]() { reentrant_shutdown_result = runner.shutdown(); });
	EXPECT_TRUE(runner.startParsers());
	EXPECT_FALSE(reentrant_shutdown_result);
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);
	EXPECT_TRUE(runner.shutdown());
	// 决策 K18=否（2026-11-04）：带 remover 的外部 parser 同无 remover：不调用 release/remover、不删除，隔离到进程退出；管理器将其作为常驻对象移出，关停可完成。
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);
	EXPECT_EQ(stats->_destroy_count.load(std::memory_order_acquire), 0u);
}

TEST(WtUftRunnerParserStart, parserCallbackShutdownDoesNotWaitForConnectThread)
{
#if defined(_WIN32)
	GTEST_SKIP() << "POSIX fork/waitpid regression harness is unavailable on Windows; this scenario is unverified there";
#else
	const pid_t child = ::fork();
	ASSERT_GE(child, 0) << "fork failed: " << std::strerror(errno);
	if (child == 0)
		::_exit(runParserCallbackShutdownScenario());

	int status = 0;
	bool completed = false;
	bool wait_failed = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline)
	{
		const pid_t waited = ::waitpid(child, &status, WNOHANG);
		if (waited == child)
		{
			completed = true;
			break;
		}
		if (waited < 0)
		{
			if (errno == EINTR)
				continue;
			wait_failed = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (!completed)
	{
		::kill(child, SIGKILL);
		while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
		{
		}
	}
	ASSERT_FALSE(wait_failed) << "waitpid failed: " << std::strerror(errno);
	ASSERT_TRUE(completed) << "cross-thread callback shutdown scenario exceeded 10 seconds";
	ASSERT_TRUE(WIFEXITED(status)) << "child terminated by signal " << WTERMSIG(status);
	EXPECT_EQ(WEXITSTATUS(status), 0) << "scenario exit code identifies the failed checkpoint";
#endif
}

// 原用例 retainedManagerAdapterBlocksCompletionAndCanRetry（remover 未确认则关停未完成、可重试）按 K18 改写。
TEST(WtUftRunnerParserShutdown, externalParserWithRemoverIsResidentAndShutdownCompletes)
{
	// 决策 K18=否（2026-11-04）：带 remover 的外部 parser 同无 remover：不调用 release/remover、不删除，隔离到进程退出；管理器将其作为常驻对象移出，关停可完成。
	q04::FakeBaseDataMgr baseData;
	WtUftRunner runner;
	auto stats = std::make_shared<ParserTeardownStats>();
	UftRunnerRetryableReleaseParserApi* api = new UftRunnerRetryableReleaseParserApi(stats);
	ParserAdapterPtr adapter(new ParserAdapter());
	ASSERT_TRUE(adapter->initExt("retryable_teardown", api, nullptr, &baseData, retryableParserRemover));
	ASSERT_TRUE(runner._parsers.addAdapter("retryable_teardown", adapter));

	EXPECT_TRUE(runner.shutdown());
	EXPECT_TRUE(runner._shutdown_complete.load(std::memory_order_acquire));
	EXPECT_TRUE(runner._inputs_stopped.load(std::memory_order_acquire));
	EXPECT_FALSE(runner._parsers.hasRetainedAdapters());
	EXPECT_TRUE(adapter->hasApi());
#ifdef WT_UFT_PARSER_K18_RESIDENT
	EXPECT_TRUE(adapter->isResidentQuarantined());
#endif
	EXPECT_EQ(stats->_release_count.load(std::memory_order_acquire), 0u);
	EXPECT_EQ(stats->_remover_count.load(std::memory_order_acquire), 0u);
	EXPECT_EQ(stats->_destroy_count.load(std::memory_order_acquire), 0u);
}
