/*
 * R95 lane —— src/WtLatencyHFT/HftLatencyTool.cpp 的 init()/run()
 *
 * R14 只覆盖了「config.yaml 不存在 -> init() 返回 false」；本文件用自己写的
 * config.yaml 进入成功支（HftLatencyTool.cpp:360-419），并继续覆盖
 * initEngine(:446-453)、initModules(:461-478)、initStrategies(:426-438) 与 run(:484-506)。
 *
 * 关键前提（全部来自代码，不靠运行目录的巧合）：
 *   - basefiles 段必须存在（:361-362 不判空），子键可以缺；缺子键只会跳过对应的 load。
 *   - env 段必须是对象（WtHftEngine::init 在 WtHftEngine.cpp:59-63 无条件 _cfg->retain()）。
 *   - run() 的 ticker 需要时段：env.product.session=ALLDAY，basefiles.session 指向自写文件。
 *   - _act_mgr.init("actpolicy.yaml") 是硬编码相对路径；用例 chdir 到临时目录，文件缺失 -> false 被忽略。
 *   - TestParser 与 TestTrader 都成功处理 init(NULL)；HFT 工具持有 TestTrader 的所有权，
 *     测试通道不 dlopen 插件、不启动真实柜台连接。专门的 teardown 用例会让 fake trader
 *     异步推进到 READY，并在 sink callback 内用 gate 暂停，以核实 owner 排空顺序。
 *   - run() 只对 times=0/1 取样：times=0 走完 TestParser::run 的头尾（:97-104、:189-194）；
 *     times=1 且不加载合约，循环体在 :113-114 因 getContract 返回 NULL 立刻返回。
 *     不构造 tick，不触发策略回调与下单路径。
 *
 * 每个用例 chdir 到系统临时目录，产品写出的 generated/ 落到临时目录，用完删除。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include "Includes/ILogHandler.h"
#include "WTSTools/WTSLogger.h"
#include "WtLatencyHFT/HftLatencyTool.h"

/* 生产 TU HftLatencyTool.cpp:33 的自由函数（main.cpp:19 也是这样声明的） */
bool test_hft();
int wt_coverage_main_wtlatencyhft();
namespace hft { class TestParser; extern TestParser* theParser; }
namespace hft {
void armShutdownGateTest();
bool waitForShutdownGateCallback(int timeout_ms);
void releaseShutdownGateCallback();
bool shutdownGateSawContext();
bool shutdownGateSawTraderReleaseAfterCallback();
bool waitForTraderReleaseDuringCallback(int timeout_ms);
bool shutdownGateSawTraderReleaseDuringCallback();
void disarmShutdownGateTest();
void setTestTraderInitFailure(bool fail);
void setTestRunFailureForTesting(int stage, bool unknown);
void setTestTickCreateFailureForTesting(bool fail);
}

namespace {

namespace stdfs = std::filesystem;

constexpr int kRunFailureParser = 1;
constexpr int kRunFailureTrader = 2;
constexpr int kRunFailureEngine = 3;

class ScopedRunFailure
{
public:
	ScopedRunFailure(int stage, bool unknown) { hft::setTestRunFailureForTesting(stage, unknown); }
	~ScopedRunFailure() { hft::setTestRunFailureForTesting(0, false); }
};

class ScopedTickCreateFailure
{
public:
	ScopedTickCreateFailure() { hft::setTestTickCreateFailureForTesting(true); }
	~ScopedTickCreateFailure() { hft::setTestTickCreateFailureForTesting(false); }
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
		m_dir /= std::string("wt_r95_hft_") + tag + "_" + std::to_string(::getpid());

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

bool parse_latency_summary(const std::string& message, unsigned int& samples,
	double& total_ns, double& average_ns)
{
	return std::sscanf(message.c_str(),
		"%u ticks simulated in %lf ns, HftEngine Innner Latency: %lf ns",
		&samples, &total_ns, &average_ns) == 3;
}

/* 0~2400 单段：任何 hhmm 都在交易时段内，ticker 线程只做 10ms 轮询，stop 后立刻 join */
const char* kSessionsYaml =
	"ALLDAY:\n"
	"    name: AllDay\n"
	"    offset: 0\n"
	"    sections:\n"
	"        - from: 0\n"
	"          to: 0\n";	// 卡2 2b=A（用户裁定 2026-11-03）：全天时段只能写 0-0，0-2400 被拒（会话无时段）；迭代03 第五会话 int2 归因更新。

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

/* 成功支 A：string 形态的 commodity/contract + 缺失的 hot 文件 */
const char* kConfigA =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
	"    hot: hots_absent.yaml\n"
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
	"    hot: hots_absent.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 3\n"
	"core: 0\n";

/* 成功支 C：basefiles 只有一个无关子键；commodity/contract/hot 的 if 全为假 */
const char* kConfigC =
	"basefiles:\n"
	"    unused: 1\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：只加载 session，times=0 -> TestParser::run 头尾 */
const char* kConfigRunZero =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* 缺省 times：WTSVariant::getUInt32(name, defVal=0) 应回退为 0 */
const char* kConfigRunTimesOmitted =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"core: 0\n";

/* run() 用：只加载 session，times=1 且没有合约 -> 循环体在 getContract 处早退 */
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
 * 一、init() 成功支（HftLatencyTool.cpp:347-419）
 * ---------------------------------------------------------------------- */

TEST(r95_hft_init, string_basefiles_reach_all_success_stages)
{
	ScopedTempCwd guard("init_a");
	ASSERT_TRUE(guard.ok()) << "无法进入临时工作目录";
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigA));

	hft::HftLatencyTool tool;
	// :363-364 session 有效；:366-380 commodity 走 VT_String；:382-396 contract 走 VT_String
	// :398-402 hot 文件缺失 -> loadHots 内部记日志（WTSHotMgr.cpp:117-122），仍继续
	// :405 actpolicy.yaml 不存在 -> init 返回 false 后被忽略
	// :407-411 times/core 读入；:413 initEngine(env 对象) -> WtHftEngine::init
	// :414 initModules()：TestParser/TestTrader 的 init(NULL) 取默认 false，不 dlopen 插件
	// :415 initStrategies()：HftStraContext 绑定 "trader" 适配器并加入引擎
	// :417 释放根配置后返回 true
	ASSERT_TRUE(tool.init());

	// 成功支的可见副作用：WtEngine::init 的 load_datas/init_outputs 与 TraderAdapter::initSaveData
	EXPECT_TRUE(stdfs::exists("generated/portfolio/trades.csv"));
	EXPECT_TRUE(stdfs::exists("generated/traders/trader/orders.csv"));
}

TEST(r95_hft_init, array_basefiles_for_commodity_and_contract_are_walked)
{
	ScopedTempCwd guard("init_b");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigB));

	hft::HftLatencyTool tool;
	// :373-379 commodity 是 VT_Array：两次 loadCommodities（第二个文件不存在）
	// :389-395 contract 是 VT_Array：同上
	EXPECT_TRUE(tool.init());
}

TEST(r95_hft_init, absent_optional_basefiles_children_are_skipped)
{
	ScopedTempCwd guard("init_c");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("config.yaml", kConfigC));

	hft::HftLatencyTool tool;
	// :363/:367/:383/:398 四个 if 全假：session/commodity/contract/hot 都不加载
	// :413-415 仍要建引擎、适配器与策略上下文，:417 返回 true
	EXPECT_TRUE(tool.init());
}

/* -------------------------------------------------------------------------
 * 二、run()（HftLatencyTool.cpp:484-506）
 * ---------------------------------------------------------------------- */

TEST(r95_hft_run, zero_ticks_after_successful_init_exercises_engine_and_parser_tail)
{
	ScopedTempCwd guard("run_zero");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	// :486-492 core==0 -> 不绑定 CPU
	// :496/:497 parsers/traders 各只有一个内存适配器，connect 是接口默认空实现
	// :499 _engine.run()：策略 on_init、写 marker.json、启动 HftRtTicker（析构时 stop/join）
	WTSLogger::init("logcfg.yaml");
	g_warning_log_handler.clear();
	bool init_ok = false;
	bool repeated_init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			hft::HftLatencyTool tool;
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
	bool saw_latency_summary = false;
	for (const std::string& warning : g_warning_log_handler.messages())
	{
		unsigned int samples = 0;
		double total_ns = 0.0;
		double average_ns = 0.0;
		saw_latency_summary = saw_latency_summary ||
			parse_latency_summary(warning, samples, total_ns, average_ns);
	}
	EXPECT_FALSE(saw_latency_summary) << "zero samples must not emit an average latency";

	EXPECT_TRUE(stdfs::exists("generated/marker.json")) << "engine.run() 没有走到写 marker 的段";
}

TEST(r95_hft_run, missing_times_uses_zero_default_and_reports_no_samples)
{
	ScopedTempCwd guard("run_times_omitted");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunTimesOmitted));
	WTSLogger::init("logcfg.yaml");

	g_warning_log_handler.clear();
	bool init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			hft::HftLatencyTool tool;
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

	bool saw_zero_default = false;
	bool saw_no_samples = false;
	bool saw_latency_summary = false;
	for (const std::string& warning : g_warning_log_handler.messages())
	{
		saw_zero_default = saw_zero_default || warning.find("0 ticks will be simulated") != std::string::npos;
		saw_no_samples = saw_no_samples || warning.find("average latency unavailable (no samples)") != std::string::npos;
		unsigned int samples = 0;
		double total_ns = 0.0;
		double average_ns = 0.0;
		saw_latency_summary = saw_latency_summary ||
			parse_latency_summary(warning, samples, total_ns, average_ns);
	}
	EXPECT_TRUE(saw_zero_default);
	EXPECT_TRUE(saw_no_samples);
	EXPECT_FALSE(saw_latency_summary);
}

TEST(r95_hft_run, run_after_failed_init_does_not_use_parser)
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
			hft::HftLatencyTool tool;
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

TEST(r95_hft_init, failed_test_trader_init_is_reported_and_cleaned_up)
{
	ScopedTempCwd guard("init_trader_failure");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	hft::setTestTraderInitFailure(true);
	bool init_ok = true;
	{
		hft::HftLatencyTool tool;
		init_ok = tool.init();
		tool.run();
	}
	hft::setTestTraderInitFailure(false);

	EXPECT_FALSE(init_ok);
	EXPECT_EQ(hft::theParser, nullptr);
}

TEST(r95_hft_run, shutdown_waits_for_active_channel_callback_before_releasing_trader_and_context)
{
	ScopedTempCwd guard("run_destroy");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	hft::armShutdownGateTest();
	bool init_ok = false;
	bool callback_entered = false;
	std::thread release_gate;
	{
		hft::HftLatencyTool tool;
		init_ok = tool.init();
		if (init_ok)
		{
			EXPECT_NO_THROW(tool.run());
			callback_entered = hft::waitForShutdownGateCallback(2000);
		}

		if (callback_entered)
		{
			release_gate = std::thread([]() {
				// A premature API release signals immediately; otherwise let teardown
				// reach the adapter drain wait before opening the callback gate.
				hft::waitForTraderReleaseDuringCallback(200);
				hft::releaseShutdownGateCallback();
			});
		}
		else
		{
			hft::releaseShutdownGateCallback();
		}
	}
	if (release_gate.joinable())
		release_gate.join();

	EXPECT_TRUE(init_ok);
	EXPECT_TRUE(callback_entered) << "the fake trader did not reach the channel-ready sink callback";
	EXPECT_TRUE(hft::shutdownGateSawContext()) << "the engine context must remain available through the gated callback";
	EXPECT_FALSE(hft::shutdownGateSawTraderReleaseDuringCallback()) << "trader release began before the active sink callback drained";
	EXPECT_TRUE(hft::shutdownGateSawTraderReleaseAfterCallback());
	EXPECT_EQ(hft::theParser, nullptr);
	hft::disarmShutdownGateTest();
}

TEST(r95_hft_run, one_tick_without_contract_reports_and_returns)
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
			hft::HftLatencyTool tool;
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

	EXPECT_TRUE(stdfs::exists("generated/marker.json"));
}

TEST(r95_hft_run, one_tick_with_contract_and_commodity_runs)
{
	ScopedTempCwd guard("run_valid_one");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunOneWithValidContract));
	WTSLogger::init("logcfg.yaml");

	g_warning_log_handler.clear();
	bool init_ok = false;
	bool run_threw = false;
	bool run_failed = true;
	{
		ScopedWarningCapture capture;
		{
			hft::HftLatencyTool tool;
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
	unsigned int summaries = 0;
	unsigned int observed_samples = 0;
	double observed_total_ns = 0.0;
	double observed_average_ns = 0.0;
	for (const std::string& warning : g_warning_log_handler.messages())
	{
		unsigned int samples = 0;
		double total_ns = 0.0;
		double average_ns = 0.0;
		if (parse_latency_summary(warning, samples, total_ns, average_ns))
		{
			++summaries;
			observed_samples = samples;
			observed_total_ns = total_ns;
			observed_average_ns = average_ns;
		}
	}
	EXPECT_EQ(1u, summaries);
	EXPECT_EQ(1u, observed_samples) << "the configured positive sample count must be reported";
	EXPECT_TRUE(std::isfinite(observed_total_ns));
	EXPECT_TRUE(std::isfinite(observed_average_ns));
	EXPECT_GE(observed_total_ns, 0.0);
	EXPECT_GE(observed_average_ns, 0.0);
}

TEST(r95_hft_run, tick_creation_failure_is_reported_and_fails_run)
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
			hft::HftLatencyTool tool;
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
			message.find("HFT latency test failed to create tick SHFE.rb2205") != std::string::npos;
	EXPECT_TRUE(saw_tick_create_failure);
}

TEST(r95_hft_run, reports_parser_trader_and_engine_exceptions)
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
			hft::HftLatencyTool tool;
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
 * 三、入口包装 test_hft()（HftLatencyTool.cpp:33-42）
 * ---------------------------------------------------------------------- */
TEST(r95_hft_entry, free_function_runs_init_and_run_with_valid_config)
{
	ScopedTempCwd guard("entry");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	// :36 栈上构造 HftLatencyTool；:38 init()（配置有效 -> true）；
	// :41 run()（times=0，引擎/解析器/交易器同上）；出作用域走工具析构，ticker stop/join。
	EXPECT_TRUE(test_hft());
	EXPECT_TRUE(stdfs::exists("generated/marker.json"));
}

TEST(r95_hft_entry, injected_run_failure_reaches_executable_status)
{
	ScopedTempCwd guard("entry_run_failure");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));
	WTSLogger::init("logcfg.yaml");

	ScopedRunFailure injected(kRunFailureParser, false);
	EXPECT_EQ(1, wt_coverage_main_wtlatencyhft());
}
