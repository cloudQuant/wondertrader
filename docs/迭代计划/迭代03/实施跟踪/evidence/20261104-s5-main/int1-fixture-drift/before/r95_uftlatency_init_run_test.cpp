/*
 * R95 lane —— src/WtLatencyUFT/UftLatencyTool.cpp 的 init()/run()
 *
 * R14 只覆盖了「config.yaml 不存在 -> init() 返回 false」；本文件用自写 config.yaml
 * 进入成功支（UftLatencyTool.cpp:211-260），并覆盖 initEngine(:276-283)、
 * initModules(:286-303)、initStrategies(:262-274) 与 run(:305-327)。
 *
 * 关键前提：
 *   - 两个硬编码相对路径 "logcfg.yaml" / "config.yaml"（:200/:202-204）由用例 chdir 到
 *     系统临时目录后自己写 config.yaml 决定；目录里不写 logcfg.yaml，日志器保持未初始化。
 *   - basefiles 段必须存在（:212-213 不判空），子键可缺；env 段必须是对象
 *     （WtUftEngine::init 只在 _cfg 非空时 retain，但 run() 的 ticker 需要 env.product.session）。
 *   - env.product.session=ALLDAY，basefiles.session 指向自写时段文件；
 *     WtUftRtTicker 的 0~2400 单段让线程只做 10ms 轮询，析构 stop 时立刻 join。
 *   - TestParser 覆盖 init(NULL) 并返回 true，满足 ParserAdapter 的外部 API 所有权合同；
 *     TestTrader 是本地替身，按顺序发送成功的空查询并可到达真实 adapter READY 状态。
 *   - run() 只对 times=0/1 取样：times=0 覆盖 TestParser::run 头尾（:41-44、:115-117）；
 *     times=1 且不加载合约，循环体在 :50-52 因 getContract 返回 NULL 早退。
 *
 * 每个用例 chdir 到系统临时目录，产品读写的相对路径全部落在临时目录，用完删除。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include "Includes/ILogHandler.h"
#include "WTSTools/WTSLogger.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WTSTools/WTSBaseDataMgr.h"

#define private public
#include "WtLatencyUFT/UftLatencyTool.h"
#undef private

/* 生产 TU UftLatencyTool.cpp:28 的自由函数（main.cpp:19 也是这样声明的） */
bool test_uft();
int wt_coverage_main_wtlatencyuft();
namespace uft {
class TestParser;
void setTestRunFailureForTesting(int stage, bool unknown);
void setTestTickCreateFailureForTesting(bool fail);
void clearTestOrderInsertForTesting();
void getTestOrderInsertForTesting(std::string& exchange, std::string& code);
}

namespace {

namespace stdfs = std::filesystem;
using namespace wtp;

constexpr int kRunFailureParser = 1;
constexpr int kRunFailureTrader = 2;
constexpr int kRunFailureEngine = 3;

class ScopedRunFailure
{
public:
	ScopedRunFailure(int stage, bool unknown) { uft::setTestRunFailureForTesting(stage, unknown); }
	~ScopedRunFailure() { uft::setTestRunFailureForTesting(0, false); }
};

class ScopedTickCreateFailure
{
public:
	ScopedTickCreateFailure() { uft::setTestTickCreateFailureForTesting(true); }
	~ScopedTickCreateFailure() { uft::setTestTickCreateFailureForTesting(false); }
};

class UftLatencyShutdownContext : public UftStraContext
{
public:
	UftLatencyShutdownContext(WtUftEngine* engine) : UftStraContext(engine, "shutdown_gate"), _engine(engine) {}

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

class UftLatencyInitGateContext : public UftStraContext
{
public:
	UftLatencyInitGateContext(WtUftEngine* engine) : UftStraContext(engine, "run_init_gate") {}

	void on_init() override
	{
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

private:
	std::mutex _mutex;
	std::condition_variable _condition;
	bool _entered{false};
	bool _continue{false};
};

class UftLatencySelfShutdownContext : public UftStraContext
{
public:
	UftLatencySelfShutdownContext(WtUftEngine* engine, uft::UftLatencyTool* tool)
		: UftStraContext(engine, "self_shutdown"), _engine(engine), _tool(tool) {}

	void on_channel_ready(uint32_t) override
	{
		_shutdown_completed.store(_tool->shutdown(), std::memory_order_release);
		_parser_preserved.store(_tool->_test_parser != nullptr, std::memory_order_release);
		_context_preserved.store(_engine->getContext(id()).get() == this, std::memory_order_release);
		_called.store(true, std::memory_order_release);
	}

	bool wasCalled() const { return _called.load(std::memory_order_acquire); }
	bool shutdownCompleted() const { return _shutdown_completed.load(std::memory_order_acquire); }
	bool parserWasPreserved() const { return _parser_preserved.load(std::memory_order_acquire); }
	bool contextWasPreserved() const { return _context_preserved.load(std::memory_order_acquire); }

private:
	WtUftEngine* _engine;
	uft::UftLatencyTool* _tool;
	std::atomic<bool> _called{false};
	std::atomic<bool> _shutdown_completed{false};
	std::atomic<bool> _parser_preserved{false};
	std::atomic<bool> _context_preserved{false};
};

class ScopedTempCwd
{
public:
	explicit ScopedTempCwd(const char* tag)
	{
		std::error_code ec;
		m_old = stdfs::current_path(ec);
		if (ec)
			return;

		m_dir = stdfs::temp_directory_path(ec);
		if (ec)
			return;
		m_dir /= std::string("wt_r95_ufttool_") + tag + "_" + std::to_string(::getpid());

		stdfs::remove_all(m_dir, ec);
		ec.clear();
		stdfs::create_directories(m_dir, ec);
		if (ec)
			return;

		stdfs::current_path(m_dir, ec);
		if (ec)
			return;

		m_ok = true;
	}

	~ScopedTempCwd()
	{
		std::error_code ec;
		if (m_ok)
			stdfs::current_path(m_old, ec);
		if (!m_dir.empty())
			stdfs::remove_all(m_dir, ec);
	}

	bool ok() const { return m_ok; }

	ScopedTempCwd(const ScopedTempCwd&) = delete;
	ScopedTempCwd& operator=(const ScopedTempCwd&) = delete;

private:
	stdfs::path	m_old;
	stdfs::path	m_dir;
	bool		m_ok = false;
};

class TestLogHandler : public ILogHandler
{
public:
	void handleLogAppend(WTSLogLevel level, const char* msg) override
	{
		if (level != LL_WARN && level != LL_ERROR)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		m_messages.emplace_back(msg != nullptr ? msg : "");
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_messages.clear();
	}

	std::vector<std::string> messages()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_messages;
	}

private:
	std::vector<std::string> m_messages;
	std::mutex m_mutex;
};

TestLogHandler g_warning_log_handler;

class ScopedWarningCapture
{
public:
	ScopedWarningCapture() { WTSLogger::registerHandler(&g_warning_log_handler); }
	~ScopedWarningCapture() { WTSLogger::registerHandler(nullptr); }
};

const char* kLogConfig =
	"{\"root\":{\"level\":\"warn\",\"async\":false,\"sinks\":["
	"{\"type\":\"basic_file_sink\",\"filename\":\"latency.log\","
	"\"truncate\":true,\"pattern\":\"%v\"}]}}";

bool write_text(const char* name, const std::string& text)
{
	std::ofstream out(name, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << text;
	out.close();
	return !out.fail();
}

const char* kSessionsYaml =
	"ALLDAY:\n"
	"    name: AllDay\n"
	"    offset: 0\n"
	"    sections:\n"
	"        - from: 0\n"
	"          to: 2400\n";

const char* kCommoditiesYaml =
	"SHFE:\n"
	"    rb:\n"
	"        covermode: 0\n"
	"        pricemode: 0\n"
	"        category: 1\n"
	"        precision: 1\n"
	"        pricetick: 1\n"
	"        volscale: 10\n"
	"        name: rebar\n"
	"        exchg: SHFE\n"
	"        session: ALLDAY\n";

const char* kContractsYaml =
	"SHFE:\n"
	"    rb2205:\n"
	"        name: rb2205\n"
	"        code: rb2205\n"
	"        exchg: SHFE\n"
	"        product: rb\n";

bool prepare_base()
{
	return write_text("sessions.yaml", kSessionsYaml)
		&& write_text("commodities.yaml", kCommoditiesYaml)
		&& write_text("contracts.yaml", kContractsYaml);
}

/* 成功支 A：string 形态的 commodity/contract */
const char* kConfigA =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* 成功支 B：array 形态（第二个文件缺失，覆盖循环的失败迭代） */
const char* kConfigB =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity:\n"
	"        - commodities.yaml\n"
	"        - commodities_absent.yaml\n"
	"    contract:\n"
	"        - contracts.yaml\n"
	"        - contracts_absent.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* 成功支 C：basefiles 只有无关子键；session/commodity/contract 的 if 全为假 */
const char* kConfigC =
	"basefiles:\n"
	"    unused: 1\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：只加载 session，times=0 */
const char* kConfigRunZero =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：省略 times，验证 WTSVariant::getUInt32 的缺省 0 */
const char* kConfigRunDefaultTimes =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"core: 0\n";

/* run() 用：只加载 session，times=1 且无合约 */
const char* kConfigRunOneWithoutContract =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 1\n"
	"core: 0\n";

const char* kConfigRunOneWithValidContract =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 1\n"
	"core: 0\n";

} // namespace

/* -------------------------------------------------------------------------
 * 一、init() 成功支（UftLatencyTool.cpp:198-260）
 * ---------------------------------------------------------------------- */

TEST(r95_ufttool_init, string_basefiles_reach_all_success_stages)
{
	ScopedTempCwd guard("init_a");
	ASSERT_TRUE(guard.ok()) << "无法进入临时工作目录";
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigA));

	uft::UftLatencyTool tool;
	// :213-214 session 有效；:216-230 commodity 走 VT_String；:232-246 contract 走 VT_String
	// :248-252 times/core；:254 initEngine(env) -> WtUftEngine::init 只存指针并 retain
	// :255 initModules()：TestParser/TestTrader 只注册到内存适配器
	// :256 initStrategies()：UftStraContext 绑定 "trader" 并加入引擎
	// :258 释放根配置后返回 true
	EXPECT_TRUE(tool.init());
}

TEST(r95_ufttool_init, array_basefiles_for_commodity_and_contract_are_walked)
{
	ScopedTempCwd guard("init_b");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigB));

	uft::UftLatencyTool tool;
	// :223-229 commodity 是 VT_Array：两次 loadCommodities（第二个文件不存在）
	// :239-245 contract 是 VT_Array：同上
	EXPECT_TRUE(tool.init());
}

TEST(r95_ufttool_init, absent_optional_basefiles_children_are_skipped)
{
	ScopedTempCwd guard("init_c");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("config.yaml", kConfigC));

	uft::UftLatencyTool tool;
	// :213/:217/:233 三个 if 全假：不加载 session/commodity/contract
	// :254-256 仍要建引擎、适配器与策略上下文，:260 返回 true
	EXPECT_TRUE(tool.init());
}

/* -------------------------------------------------------------------------
 * 二、run()（UftLatencyTool.cpp:305-327）
 * ---------------------------------------------------------------------- */

TEST(r95_ufttool_run, zero_ticks_after_successful_init_exercises_engine_and_parser_tail)
{
	ScopedTempCwd guard("run_zero");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	// :307-313 core==0 -> 不绑定 CPU
	// :317/:318 parsers/traders 各一个内存适配器，connect 是接口默认空实现
	// :320 _engine.run()：策略 on_init、启动 UftRtTicker（析构时 stop/join）
	WTSLogger::init("logcfg.yaml");
	g_warning_log_handler.clear();
	bool init_ok = false;
	bool repeated_init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			uft::UftLatencyTool tool;
			try
			{
				init_ok = tool.init();
				if (init_ok)
				{
					repeated_init_ok = tool.init();
					tool.run();
					run_failed = tool.runFailed();
				}
			}
			catch (...)
			{
				run_threw = true;
			}
		}
	}
	EXPECT_TRUE(init_ok);
	EXPECT_FALSE(repeated_init_ok) << "init() is a one-shot operation";
	EXPECT_FALSE(run_threw);
	EXPECT_FALSE(run_failed);
	bool saw_no_samples = false;
	for (const std::string& warning : g_warning_log_handler.messages())
		saw_no_samples = saw_no_samples || warning.find("average latency unavailable (no samples)") != std::string::npos;
	EXPECT_TRUE(saw_no_samples);
}

TEST(r95_ufttool_run, omitted_times_defaults_to_zero_without_average_latency)
{
	ScopedTempCwd guard("run_default_times");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunDefaultTimes));
	ASSERT_EQ(std::string::npos, std::string(kConfigRunDefaultTimes).find("times:"));

	WTSLogger::init("logcfg.yaml");
	g_warning_log_handler.clear();
	bool init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			uft::UftLatencyTool tool;
			try
			{
				init_ok = tool.init();
				if (init_ok)
				{
					tool.run();
					run_failed = tool.runFailed();
				}
			}
			catch (...)
			{
				run_threw = true;
			}
		}
	}
	EXPECT_TRUE(init_ok);
	EXPECT_FALSE(run_threw);
	EXPECT_FALSE(run_failed);
	bool saw_no_samples = false;
	bool saw_average_latency = false;
	for (const std::string& warning : g_warning_log_handler.messages())
	{
		saw_no_samples = saw_no_samples || warning.find("average latency unavailable (no samples)") != std::string::npos;
		saw_average_latency = saw_average_latency || warning.find("UftEngine Innner Latency:") != std::string::npos;
	}
	EXPECT_TRUE(saw_no_samples);
	EXPECT_FALSE(saw_average_latency);
}

TEST(r95_ufttool_run, run_after_failed_init_does_not_use_parser)
{
	ScopedTempCwd guard("run_failed_init");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	WTSLogger::init("logcfg.yaml");

	g_warning_log_handler.clear();
	bool init_ok = true;
	{
		ScopedWarningCapture capture;
		{
			uft::UftLatencyTool tool;
			init_ok = tool.init();
			tool.run();
		}
	}
	EXPECT_FALSE(init_ok);
	bool saw_not_initialized = false;
	for (const std::string& message : g_warning_log_handler.messages())
		saw_not_initialized = saw_not_initialized || message.find("before parser initialization succeeds") != std::string::npos;
	EXPECT_TRUE(saw_not_initialized);
}

TEST(r95_ufttool_run, shutdown_after_run_stops_engine_before_releasing_parser)
{
	ScopedTempCwd guard("run_destroy");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	{
		uft::UftLatencyTool tool;
		ASSERT_TRUE(tool.init());
		EXPECT_NO_THROW(tool.run());
		EXPECT_NE(tool._test_parser, nullptr);
		EXPECT_TRUE(tool.shutdown());
		EXPECT_EQ(tool._test_parser, nullptr);
	}
}

TEST(r95_ufttool_run, shutdown_waits_for_real_ready_callback_before_releasing_context)
{
	ScopedTempCwd guard("run_sink_shutdown");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));
	WTSLogger::init("logcfg.yaml");

	uft::UftLatencyTool tool;
	ASSERT_TRUE(tool.init());
	TraderAdapterPtr adapter = tool._traders.getAdapter("trader");
	ASSERT_NE(adapter.get(), nullptr);
	std::shared_ptr<UftLatencyShutdownContext> context(new UftLatencyShutdownContext(&tool._engine));
	context->setTrader(adapter.get());
	tool._engine.addContext(UftContextPtr(context));
	adapter->addSink(context.get());

	std::thread runThread([&]() { tool.run(); });
	const bool callbackEntered = context->waitUntilEntered();
	if (!callbackEntered)
	{
		context->allowCallbackToReturn();
		runThread.join();
		FAIL() << "test trader did not reach a READY sink callback";
	}
	EXPECT_TRUE(context->ownerWasVisible());

	std::atomic<bool> shutdown_done{false};
	std::thread shutdownThread([&]() {
		EXPECT_TRUE(tool.shutdown());
		shutdown_done.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	EXPECT_FALSE(shutdown_done.load(std::memory_order_acquire));
	EXPECT_NE(tool._engine.getContext(context->id()).get(), nullptr);

	context->allowCallbackToReturn();
	runThread.join();
	shutdownThread.join();
	EXPECT_TRUE(shutdown_done.load(std::memory_order_acquire));
	EXPECT_NE(tool._engine.getContext(context->id()).get(), nullptr);
	EXPECT_EQ(tool._test_parser, nullptr);
}

TEST(r95_ufttool_run, shutdown_waits_for_run_operation_before_releasing_parser)
{
	ScopedTempCwd guard("run_operation_shutdown");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));
	WTSLogger::init("logcfg.yaml");

	uft::UftLatencyTool tool;
	ASSERT_TRUE(tool.init());
	std::shared_ptr<UftLatencyInitGateContext> context(new UftLatencyInitGateContext(&tool._engine));
	tool._engine.addContext(UftContextPtr(context));

	std::thread runThread([&]() { tool.run(); });
	const bool operationEntered = context->waitUntilEntered();
	if (!operationEntered)
	{
		context->allowCallbackToReturn();
		runThread.join();
		FAIL() << "UFT run did not enter the gated strategy initialization callback";
	}

	std::atomic<bool> shutdownDone{false};
	std::thread shutdownThread([&]() {
		EXPECT_TRUE(tool.shutdown());
		shutdownDone.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(25));
	EXPECT_FALSE(shutdownDone.load(std::memory_order_acquire));
	EXPECT_NE(tool._test_parser, nullptr);
	EXPECT_FALSE(tool._shutdown_complete.load(std::memory_order_acquire));

	context->allowCallbackToReturn();
	runThread.join();
	shutdownThread.join();
	EXPECT_TRUE(shutdownDone.load(std::memory_order_acquire));
	EXPECT_EQ(tool._test_parser, nullptr);
	EXPECT_TRUE(tool._shutdown_complete.load(std::memory_order_acquire));
}

TEST(r95_ufttool_run, shutdown_called_from_ready_callback_defers_without_self_wait)
{
	ScopedTempCwd guard("run_callback_shutdown");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));
	WTSLogger::init("logcfg.yaml");

	uft::UftLatencyTool tool;
	ASSERT_TRUE(tool.init());
	TraderAdapterPtr adapter = tool._traders.getAdapter("trader");
	ASSERT_NE(adapter.get(), nullptr);
	std::shared_ptr<UftLatencySelfShutdownContext> context(
		new UftLatencySelfShutdownContext(&tool._engine, &tool));
	context->setTrader(adapter.get());
	tool._engine.addContext(UftContextPtr(context));
	adapter->addSink(context.get());

	EXPECT_NO_THROW(tool.run());
	EXPECT_TRUE(context->wasCalled());
	EXPECT_FALSE(context->shutdownCompleted());
	EXPECT_TRUE(context->parserWasPreserved());
	EXPECT_TRUE(context->contextWasPreserved());
	EXPECT_TRUE(tool._shutdown_complete.load(std::memory_order_acquire));
	EXPECT_EQ(tool._test_parser, nullptr);
}

TEST(r95_ufttool_run, one_tick_without_contract_reports_and_returns)
{
	ScopedTempCwd guard("run_one");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunOneWithoutContract));
	WTSLogger::init("logcfg.yaml");

	g_warning_log_handler.clear();
	bool init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			uft::UftLatencyTool tool;
			init_ok = tool.init();
			if (init_ok)
			{
				try
				{
					tool.run();
					run_failed = tool.runFailed();
				}
				catch (...)
				{
					run_threw = true;
				}
			}
		}
	}
	EXPECT_TRUE(init_ok);
	EXPECT_FALSE(run_threw);
	EXPECT_FALSE(run_failed);
	bool saw_missing_contract = false;
	for (const std::string& warning : g_warning_log_handler.messages())
		saw_missing_contract = saw_missing_contract || warning.find("contract SHFE.rb2205 not found") != std::string::npos;
	EXPECT_TRUE(saw_missing_contract);
}

TEST(r95_ufttool_run, one_tick_with_contract_and_commodity_runs)
{
	ScopedTempCwd guard("run_valid_one");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunOneWithValidContract));
	WTSLogger::init("logcfg.yaml");

	uft::clearTestOrderInsertForTesting();
	g_warning_log_handler.clear();
	bool init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			uft::UftLatencyTool tool;
			init_ok = tool.init();
			if (init_ok)
			{
				try
				{
					tool.run();
					run_failed = tool.runFailed();
				}
				catch (...)
				{
					run_threw = true;
				}
			}
		}
	}
	EXPECT_TRUE(init_ok);
	EXPECT_FALSE(run_threw);
	EXPECT_FALSE(run_failed);
	std::string submitted_exchange;
	std::string submitted_code;
	uft::getTestOrderInsertForTesting(submitted_exchange, submitted_code);
	EXPECT_EQ("SHFE", submitted_exchange);
	EXPECT_EQ("rb2205", submitted_code);
	bool saw_completed_tick = false;
	bool parsed_finite_average = false;
	const std::string latency_label = "UftEngine Innner Latency: ";
	for (const std::string& warning : g_warning_log_handler.messages())
	{
		const size_t summary = warning.find("1 ticks simulated in ");
		if (summary == std::string::npos)
			continue;
		saw_completed_tick = true;

		const size_t value_start = warning.find(latency_label, summary);
		ASSERT_NE(std::string::npos, value_start) << "missing latency field in summary: " << warning;
		const size_t value_begin = value_start + latency_label.size();
		const size_t value_end = warning.find(" ns", value_begin);
		ASSERT_NE(std::string::npos, value_end) << "missing latency unit in summary: " << warning;

		std::istringstream value_stream(warning.substr(value_begin, value_end - value_begin));
		double average_latency = 0.0;
		value_stream >> average_latency;
		const bool finite_value = !value_stream.fail() && std::isfinite(average_latency);
		value_stream >> std::ws;
		const bool consumed_value = value_stream.eof();
		EXPECT_TRUE(finite_value && consumed_value)
			<< "latency summary must contain a finite numeric mean: " << warning;
		parsed_finite_average = parsed_finite_average || (finite_value && consumed_value);
	}
	EXPECT_TRUE(saw_completed_tick);
	EXPECT_TRUE(parsed_finite_average) << "the positive-tick summary did not produce a finite latency mean";
}

TEST(r95_ufttool_run, tick_creation_failure_is_reported_and_fails_run)
{
	ScopedTempCwd guard("run_tick_create_failure");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunOneWithValidContract));
	WTSLogger::init("logcfg.yaml");

	g_warning_log_handler.clear();
	bool init_ok = false;
	bool run_threw = false;
	bool run_failed = false;
	{
		ScopedWarningCapture capture;
		{
			uft::UftLatencyTool tool;
			init_ok = tool.init();
			if (init_ok)
			{
				ScopedTickCreateFailure injected;
				try
				{
					tool.run();
					run_failed = tool.runFailed();
				}
				catch (...)
				{
					run_threw = true;
				}
			}
		}
	}
	EXPECT_TRUE(init_ok);
	EXPECT_FALSE(run_threw);
	EXPECT_TRUE(run_failed);
	bool saw_tick_create_failure = false;
	for (const std::string& message : g_warning_log_handler.messages())
		saw_tick_create_failure = saw_tick_create_failure ||
			message.find("UFT latency test failed to create tick SHFE.rb2205") != std::string::npos;
	EXPECT_TRUE(saw_tick_create_failure);
}

TEST(r95_ufttool_run, reports_parser_trader_and_engine_exceptions)
{
	ScopedTempCwd guard("run_exceptions");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));
	WTSLogger::init("logcfg.yaml");

	struct FailureCase { int stage; bool unknown; const char* diagnostic; };
	const FailureCase cases[] = {
		{ kRunFailureParser, false, "injected parser exception" },
		{ kRunFailureTrader, false, "injected trader exception" },
		{ kRunFailureEngine, true, "unknown exception" },
	};
	ScopedWarningCapture capture;
	for (const FailureCase& failure : cases)
	{
		g_warning_log_handler.clear();
		{
			ScopedRunFailure injected(failure.stage, failure.unknown);
			uft::UftLatencyTool tool;
			ASSERT_TRUE(tool.init());
			EXPECT_NO_THROW(tool.run());
			EXPECT_TRUE(tool.runFailed());
		}
		bool saw_diagnostic = false;
		for (const std::string& message : g_warning_log_handler.messages())
			saw_diagnostic = saw_diagnostic || message.find(failure.diagnostic) != std::string::npos;
		EXPECT_TRUE(saw_diagnostic) << "missing diagnostic: " << failure.diagnostic;
	}
}

/* -------------------------------------------------------------------------
 * 三、入口包装 test_uft()（UftLatencyTool.cpp:28-34）
 * ---------------------------------------------------------------------- */
TEST(r95_ufttool_entry, free_function_runs_init_and_run_with_valid_config)
{
	ScopedTempCwd guard("entry");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	// :30 栈上构造 UftLatencyTool；:31 init()（配置有效 -> true）；
	// :33 run()（times=0，引擎/解析器/交易器同上）；出作用域走工具析构，ticker stop/join。
	EXPECT_TRUE(test_uft());
}

TEST(r95_ufttool_entry, injected_run_failure_reaches_executable_status)
{
	ScopedTempCwd guard("entry_run_failure");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));
	WTSLogger::init("logcfg.yaml");

	ScopedRunFailure injected(kRunFailureParser, false);
	EXPECT_EQ(1, wt_coverage_main_wtlatencyuft());
}
