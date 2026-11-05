/*
 * R95 lane —— src/WtUftRunner/WtUftRunner.cpp 的 config()/run()
 *
 * R14 只走了 config() 的两条早退支（文件不存在 / basefiles 只有占位键且 env、data 缺失）。
 * 本文件把 runner 配置与启动的成功/失败支拆成可控输入：
 *   - basefiles 四类子键（session / commodity / contract / holiday）的 string 与 array 两种形态；
 *   - parser 数组或独立 yaml 中的 active/inactive 缺模块分支，以及真实 loadable fake parser 正控；
 *   - trader 配置形态与 R85 fake trader init=false / 成功正控；
 *   - run() 用「先 raise(SIGINT) 让 _to_exit=true」跳过退出轮询；无 share_domain 时
 *     不会创建共享域 watcher。
 *
 * 每个用例 chdir 到自己的系统临时目录，产品读写的相对路径（生成的日志/工厂目录）
 * 都落在临时目录内，用例退出时恢复并删除。日志不落仓库。
 */
#include <gtest/gtest.h>
#include <gtest/internal/gtest-port.h>

#include <csignal>
#include <chrono>
#include <cerrno>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <mutex>
#include <spawn.h>
#include <queue>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// Load the dependencies first so the access shim below only exposes
// WtUftRunner's test seam, not private members in transitive headers.
#include "Includes/ILogHandler.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Includes/IUftStraCtx.h"
#include "Includes/RiskMonDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSCollection.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/SpinMutex.hpp"
#include "Share/StdUtils.hpp"
#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WTSUtils/WTSCfgLoader.h"

#define private public
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftRunner/WtUftRunner.h"
#undef private

#ifndef WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH
#error "CoverageTests_m_WtUftRunner must wire the UFT lifecycle fake plugin"
#endif
#ifndef WT_R95_PARSER_PLUGIN_PATH
#error "CoverageTests_m_WtUftRunner must wire the R95 parser lifecycle fixture"
#endif
#ifndef WT_R95_UFT_STRATEGY_PLUGIN_PATH
#error "CoverageTests_m_WtUftRunner must wire the R95 dynamic UFT strategy fixture"
#endif

namespace {

namespace fs = std::filesystem;

fs::path& r85TestExecutablePath()
{
	static fs::path path;
	return path;
}

void cacheTestExecutablePathBeforeChangingCwd()
{
	fs::path& path = r85TestExecutablePath();
	if (!path.empty())
		return;
	const std::vector<std::string> argvs = testing::internal::GetArgvs();
	if (!argvs.empty() && !argvs.front().empty())
		path = fs::absolute(argvs.front()).lexically_normal();
}

/* 进入 <temp>/wt_r95_uft_<tag>_<pid>，析构时恢复 cwd 并整目录删除 */
class ScopedTempCwd
{
public:
	explicit ScopedTempCwd(const char* tag)
	{
		// Cache argv[0] while it is still relative to the process launch CWD;
		// runner tests below change CWD before spawning the isolated worker.
		cacheTestExecutablePathBeforeChangingCwd();
		std::error_code ec;
		m_old = fs::current_path(ec);
		if (ec)
			return;

		m_dir = fs::temp_directory_path(ec);
		if (ec)
			return;
		m_dir /= std::string("wt_r95_uft_") + tag + "_" + std::to_string(::getpid());

		fs::remove_all(m_dir, ec);
		ec.clear();
		fs::create_directories(m_dir, ec);
		if (ec)
			return;

		fs::current_path(m_dir, ec);
		if (ec)
			return;

		m_ok = true;
	}

	~ScopedTempCwd()
	{
		std::error_code ec;
		if (m_ok)
			fs::current_path(m_old, ec);
		if (!m_dir.empty())
			fs::remove_all(m_dir, ec);
	}

	bool ok() const { return m_ok; }

	ScopedTempCwd(const ScopedTempCwd&) = delete;
	ScopedTempCwd& operator=(const ScopedTempCwd&) = delete;

private:
	fs::path	m_old;
	fs::path	m_dir;
	bool		m_ok = false;
};

bool write_text(const char* name, const std::string& text)
{
	std::ofstream out(name, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << text;
	out.close();
	return !out.fail();
}

class ScopedInstDir
{
public:
	explicit ScopedInstDir(const fs::path& root) : _old(WtHelper::getInstDir())
	{
		std::string path = root.string();
		if (path.empty() || path.back() != fs::path::preferred_separator)
			path.push_back(fs::path::preferred_separator);
		WtHelper::setInstDir(path.c_str());
	}
	~ScopedInstDir() { WtHelper::setInstDir(_old.c_str()); }

	ScopedInstDir(const ScopedInstDir&) = delete;
	ScopedInstDir& operator=(const ScopedInstDir&) = delete;

private:
	std::string _old;
};

bool stageLifecycleTrader()
{
	const fs::path source(WT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH);
	std::error_code ec;
	fs::create_directories("traders", ec);
	if (ec)
		return false;
	fs::copy_file(source, fs::path("traders") / source.filename(), fs::copy_options::overwrite_existing, ec);
	return !ec;
}

class ScopedEnvVar
{
public:
	ScopedEnvVar(const char* name, const std::string& value) : _name(name)
	{
		const char* current = std::getenv(name);
		_hadOldValue = current != nullptr;
		if (_hadOldValue)
			_oldValue = current;
#if defined(_WIN32)
		_ok = _putenv_s(name, value.c_str()) == 0;
#else
		_ok = ::setenv(name, value.c_str(), 1) == 0;
#endif
	}
	~ScopedEnvVar()
	{
#if defined(_WIN32)
		_putenv_s(_name.c_str(), _hadOldValue ? _oldValue.c_str() : "");
#else
		if (_hadOldValue)
			::setenv(_name.c_str(), _oldValue.c_str(), 1);
		else
			::unsetenv(_name.c_str());
#endif
	}

	bool ok() const { return _ok; }
	ScopedEnvVar(const ScopedEnvVar&) = delete;
	ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
	std::string _name;
	std::string _oldValue;
	bool _hadOldValue{false};
	bool _ok{false};
};

class R85LogCapture final : public ILogHandler
{
public:
	void handleLogAppend(WTSLogLevel, const char* message) override
	{
		_messages.emplace_back(message == nullptr ? "" : message);
	}

	bool contains(const std::string& needle) const
	{
		for (const std::string& message : _messages)
			if (message.find(needle) != std::string::npos)
				return true;
		return false;
	}

	std::string joinedMessages() const
	{
		std::string result;
		for (const std::string& message : _messages)
			result += message + "\n";
		return result;
	}

private:
	std::vector<std::string> _messages;
};

class ScopedR85LogRegistration
{
public:
	ScopedR85LogRegistration(R85LogCapture& capture, const fs::path& outputPath)
		: _capture(capture), _outputPath(outputPath) { WTSLogger::registerHandler(&_capture); }
	~ScopedR85LogRegistration()
	{
		WTSLogger::registerHandler(nullptr);
		const std::string output = _outputPath.string();
		write_text(output.c_str(), _capture.joinedMessages());
	}

	ScopedR85LogRegistration(const ScopedR85LogRegistration&) = delete;
	ScopedR85LogRegistration& operator=(const ScopedR85LogRegistration&) = delete;

private:
	R85LogCapture& _capture;
	fs::path _outputPath;
};

bool spawnIsolatedLogProbe(const char* mode, const fs::path& outputPath)
{
	const fs::path& executablePath = r85TestExecutablePath();
	if (executablePath.empty())
		return false;
	const std::string executable = executablePath.string();
	std::string filterArg = "--gtest_filter=r85_uft_runner_child.DISABLED_log_capture_probe";
	std::string disabledArg = "--gtest_also_run_disabled_tests";
	std::vector<std::string> argStorage{executable, filterArg, disabledArg};
	std::vector<char*> childArgv;
	childArgv.reserve(argStorage.size() + 1);
	for (std::string& arg : argStorage)
		childArgv.push_back(arg.data());
	childArgv.push_back(nullptr);

	const std::string output = outputPath.string();
	ScopedEnvVar childMode("WT_R85_CHILD_MODE", mode);
	ScopedEnvVar childOutput("WT_R85_CAPTURE_LOG", output);
	if (!childMode.ok() || !childOutput.ok())
		return false;

	pid_t child = -1;
	const int spawnError = ::posix_spawn(&child, executable.c_str(), nullptr, nullptr, childArgv.data(), environ);
	if (spawnError != 0)
		return false;

	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	for (;;)
	{
		const pid_t waited = ::waitpid(child, &status, WNOHANG);
		if (waited == child)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (waited < 0 && errno != EINTR)
			return false;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			::kill(child, SIGKILL);
			while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

WTSVariant* makeLifecycleTraderList(const char* id, const char* initMode)
{
	WTSVariant* traders = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", id);
	item->append("module", "UftLifecycleFakeTrader");
	item->append("lifecycle_mode", initMode);
	if (!traders->append(item, false))
	{
		item->release();
		traders->release();
		return nullptr;
	}
	return traders;
}

std::string lifecycleRunnerConfig(const char* id, const char* initMode)
{
	return std::string(
		"basefiles:\n"
		"    session: sessions.yaml\n"
		"env:\n"
		"    product:\n"
		"        session: ALLDAY\n"
		"data:\n"
		"    unused: 1\n"
		"bspolicy: actpolicy.json\n"
		"traders:\n"
		"    - active: true\n"
		"      id: ") + id + "\n"
		"      module: UftLifecycleFakeTrader\n"
		"      lifecycle_mode: " + initMode + "\n";
}

std::vector<std::string> readLifecycleEvents(const fs::path& path)
{
	std::ifstream in(path);
	std::vector<std::string> events;
	std::string event;
	while (std::getline(in, event))
		events.push_back(event);
	return events;
}

bool runReturnsBeforeSignal(WtUftRunner& runner, bool bAsync = false)
{
	std::promise<void> completion;
	std::future<void> finished = completion.get_future();
	std::thread runThread([&runner, &completion, bAsync]() {
		runner.run(bAsync);
		completion.set_value();
	});
	const bool returnedBeforeSignal = finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (!returnedBeforeSignal)
		std::raise(SIGINT);
	runThread.join();
	std::signal(SIGINT, SIG_DFL);
	return returnedBeforeSignal;
}

/* WTSSessionInfo 的 0~2400 单段：任何 hhmm 都在交易时段内，ticker 线程不会落进 10s 休眠。
 * 2400 经 offsetTime(..., false) 仍是 2400，isLastOfSection 只在 24:00 边界命中。 */
// 卡2 2b=A（用户裁定 2026-11-03）：交易时段全天只能写 0-0，0-2400 被会话加载器拒绝
// （迭代03 s5-runners 更新，原为 to: 2400）。
const char* kSessionsYaml =
	"ALLDAY:\n"
	"    name: AllDay\n"
	"    offset: 0\n"
	"    sections:\n"
	"        - from: 0\n"
	"          to: 0\n";

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

const char* kActionPolicyJson =
	"{\n"
	"  \"default\": {\n"
	"    \"order\": [{\"action\":\"open\", \"limit\":100}]\n"
	"  }\n"
	"}\n";

/* A：basefiles 用 string；只有 active parser 缺模块，应拒绝配置且不触及 trader 初始化 */
const char* kConfigA =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
	"    holiday: holidays_absent.yaml\n"
	"bspolicy: actpolicy.json\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers:\n"
	"    - active: false\n"
	"      id: p_off\n"
	"    - active: true\n"
	"      id: p_one\n"
	"      module: no_such_parser\n";

/* B：basefiles 用 array；只有外部 parser 文件里的 active parser 缺模块 */
const char* kConfigB =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity:\n"
	"        - commodities.yaml\n"
	"        - commodities_absent.yaml\n"
	"    contract:\n"
	"        - contracts.yaml\n"
	"        - contracts_absent.yaml\n"
	"bspolicy: actpolicy.json\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers: parsers.yaml\n";

const char* kParsersYaml =
	"parsers:\n"
	"    - active: false\n"
	"      id: p_off\n"
	"    - active: true\n"
	"      module: no_such_parser\n";

/* C：只有 trader 配置文件名不存在；parser 配置不存在不参与本用例 */
const char* kConfigC =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n"
	"traders: traders_absent.yaml\n";

/* D：parser 文件后缀无效；配置在 parser 阶段失败，不声称覆盖 trader 文件 */
const char* kConfigD =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n"
	"parsers: parsers.cfg\n";

/* E：parser 文件里没有 parsers 键；只验证 parser 文件结构错误 */
const char* kConfigE =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n"
	"parsers: parsers_no_key.yaml\n";

/* F：没有 parser 配置，traders 直接给对象，独立命中 trader 类型错误 */
const char* kConfigTraderObject =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n"
	"traders:\n"
	"    only: object\n";

std::string parserRunnerConfig(const std::string& parserYaml)
{
	return std::string(
		"basefiles:\n"
		"    session: sessions.yaml\n"
		"env:\n"
		"    product:\n"
		"        session: ALLDAY\n"
		"data:\n"
		"    unused: 1\n"
		"bspolicy: actpolicy.json\n"
		"parsers:\n") + parserYaml;
}

template <typename Symbol>
Symbol r95ParserSymbol(DllHandle handle, const char* name)
{
	return reinterpret_cast<Symbol>(DLLHelper::get_symbol(handle, name));
}

bool stageR95ParserFixture(fs::path& stagedPath)
{
	const fs::path source(WT_R95_PARSER_PLUGIN_PATH);
	std::error_code ec;
	fs::create_directories("parsers", ec);
	if (ec)
		return false;
	stagedPath = fs::path("parsers") / source.filename();
	fs::copy_file(source, stagedPath, fs::copy_options::overwrite_existing, ec);
	return !ec;
}

bool stageR95UftStrategyFixture()
{
	const fs::path source(WT_R95_UFT_STRATEGY_PLUGIN_PATH);
	std::error_code ec;
	fs::create_directories("uft", ec);
	if (ec)
		return false;
	fs::copy_file(source, fs::path("uft") / source.filename(), fs::copy_options::overwrite_existing, ec);
	return !ec;
}

bool spawnIsolatedUftStrategyProbe(const char* mode)
{
	const fs::path& executablePath = r85TestExecutablePath();
	if (executablePath.empty())
		return false;
	const std::string executable = executablePath.string();
	std::vector<std::string> argStorage{
		executable,
		"--gtest_filter=r95_uft_strategy_child.DISABLED_probe",
		"--gtest_also_run_disabled_tests"};
	std::vector<char*> childArgv;
	childArgv.reserve(argStorage.size() + 1);
	for (std::string& arg : argStorage)
		childArgv.push_back(arg.data());
	childArgv.push_back(nullptr);

	ScopedEnvVar childMode("WT_R95_UFT_STRATEGY_MODE", mode);
	if (!childMode.ok())
		return false;

	pid_t child = -1;
	const int spawnError = ::posix_spawn(&child, executable.c_str(), nullptr, nullptr, childArgv.data(), environ);
	if (spawnError != 0)
		return false;

	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	for (;;)
	{
		const pid_t waited = ::waitpid(child, &status, WNOHANG);
		if (waited == child)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (waited < 0 && errno != EINTR)
			return false;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			::kill(child, SIGKILL);
			while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

/* run() 用的最小成功配置：只要 session + env + data；无 share_domain/parsers/traders/strategies */
const char* kConfigRun =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n";

/* R95-002 negative control: identical valid basefiles/env/policy, with only data omitted. */
const char* kConfigMissingData =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"bspolicy: actpolicy.json\n";

/* R95-002 negative control: identical valid basefiles/data/policy, with only env omitted. */
const char* kConfigMissingEnv =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n";

const char* kConfigBadTrader =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy.json\n"
	"traders:\n"
	"    - active: true\n"
	"      id: invalid_trader\n"
	"      module: no_such_trader\n";

bool prepare_common()
{
	return write_text("sessions.yaml", kSessionsYaml)
		&& write_text("commodities.yaml", kCommoditiesYaml)
		&& write_text("contracts.yaml", kContractsYaml)
		&& write_text("actpolicy.json", kActionPolicyJson);
}

bool addReadyTraderForStrategyProbe(WtUftRunner& runner, TraderAdapterPtr& adapter)
{
	adapter.reset(new TraderAdapter());
	adapter->_sink_channel_ready = true;
	adapter->_trading_day.store(20261101, std::memory_order_release);
	return runner._traders.addAdapter("ready_trader", adapter);
}

} // namespace

TEST(r95_uft_strategy_child, DISABLED_probe)
{
	const char* mode = std::getenv("WT_R95_UFT_STRATEGY_MODE");
	const char* eventLog = std::getenv("WT_R95_UFT_STRATEGY_LOG");
	ASSERT_NE(nullptr, mode);
	ASSERT_NE(nullptr, eventLog);

	if (std::string(mode) == "full_failure")
	{
		WtUftRunner runner;
		EXPECT_FALSE(runner.config("cfg.yaml"));
		EXPECT_FALSE(runner._config_succeeded.load(std::memory_order_acquire));
		EXPECT_TRUE(runner._uft_engine._ctx_map.empty());
		EXPECT_NE(nullptr, runner._uft_stra_mgr.getStrategy("fail_only").get());
		EXPECT_TRUE(runReturnsBeforeSignal(runner));
		return;
	}

	if (std::string(mode) == "multi_failure")
	{
		WtUftRunner runner;
		runner._config = WTSCfgLoader::load_from_file("cfg.yaml");
		ASSERT_NE(nullptr, runner._config);
		TraderAdapterPtr adapter;
		ASSERT_TRUE(addReadyTraderForStrategyProbe(runner, adapter));

		EXPECT_FALSE(runner.initUftStrategies());
		EXPECT_TRUE(runner._uft_engine._ctx_map.empty());
		EXPECT_TRUE(adapter->_sinks.empty());
		const std::string events = read_text(eventLog);
		EXPECT_EQ(std::string::npos, events.find("strategy.ready:"));
		EXPECT_NE(nullptr, runner._uft_stra_mgr.getStrategy("first_success").get());
		EXPECT_NE(nullptr, runner._uft_stra_mgr.getStrategy("second_failure").get());
		EXPECT_TRUE(runner.shutdown());
		return;
	}

	if (std::string(mode) == "duplicate_ids")
	{
		WtUftRunner runner;
		runner._config = WTSCfgLoader::load_from_file("cfg.yaml");
		ASSERT_NE(nullptr, runner._config);
		TraderAdapterPtr adapter;
		ASSERT_TRUE(addReadyTraderForStrategyProbe(runner, adapter));

		EXPECT_FALSE(runner.initUftStrategies());
		EXPECT_TRUE(runner._uft_engine._ctx_map.empty());
		EXPECT_TRUE(adapter->_sinks.empty());
		EXPECT_TRUE(runner.shutdown());
		const std::string events = read_text(eventLog);
		EXPECT_EQ(std::string::npos, events.find("strategy.create:"));
		EXPECT_EQ(std::string::npos, events.find("strategy.init:"));
		return;
	}

	if (std::string(mode) == "ready_success")
	{
		WtUftRunner runner;
		runner._config = WTSCfgLoader::load_from_file("cfg.yaml");
		ASSERT_NE(nullptr, runner._config);
		TraderAdapterPtr adapter;
		ASSERT_TRUE(addReadyTraderForStrategyProbe(runner, adapter));

		EXPECT_TRUE(runner.initUftStrategies());
		EXPECT_EQ(1u, runner._uft_engine._ctx_map.size());
		EXPECT_EQ(1u, adapter->_sinks.size());
		const std::string events = read_text(eventLog);
		const std::size_t initAt = events.find("strategy.init:ready_success:true");
		const std::size_t readyAt = events.find("strategy.ready:ready_success:");
		EXPECT_NE(std::string::npos, initAt);
		EXPECT_NE(std::string::npos, readyAt);
		EXPECT_LT(initAt, readyAt);
		EXPECT_TRUE(runner.shutdown());
		return;
	}

	if (std::string(mode) == "empty_compat")
	{
		for (uint32_t shape = 0; shape < 4; shape++)
		{
			WtUftRunner runner;
			runner._config = WTSVariant::createObject();
			if (shape == 0)
			{
				ASSERT_TRUE(runner._config->append("strategies", WTSVariant::createArray(), false));
			}
			else if (shape == 1)
			{
				ASSERT_TRUE(runner._config->append("strategies", WTSVariant::createObject(), false));
			}
			else
			{
				WTSVariant* strategies = WTSVariant::createObject();
				ASSERT_TRUE(runner._config->append("strategies", strategies, false));
				if (shape == 2)
				{
					ASSERT_TRUE(strategies->append("uft", WTSVariant::createObject(), false));
				}
				else
				{
					WTSVariant* uft = WTSVariant::createArray();
					ASSERT_TRUE(uft->append(WTSVariant::createObject(), false));
					ASSERT_TRUE(strategies->append("uft", uft, false));
				}
			}
			EXPECT_TRUE(runner.initUftStrategies());
			EXPECT_TRUE(runner._uft_engine._ctx_map.empty());
			EXPECT_TRUE(runner.shutdown());
		}

		const std::vector<std::pair<const char*, const char*>> configs{
			{"cfg_missing.yaml", "unused: value\n"},
			{"cfg_no_uft.yaml", "strategies:\n    cta: []\n"},
			{"cfg_empty_uft.yaml", "strategies:\n    uft: []\n"},
			{"cfg_inactive.yaml",
				"strategies:\n"
				"    uft:\n"
				"        - active: false\n"
				"          id: inactive\n"
				"          name: R95FakeUft.Failure\n"
				"          params:\n"
				"              succeed: false\n"}};
		for (const auto& item : configs)
		{
			WtUftRunner runner;
			runner._config = WTSCfgLoader::load_from_file(item.first);
			ASSERT_NE(nullptr, runner._config) << item.first;
			EXPECT_TRUE(runner.initUftStrategies()) << item.first;
			EXPECT_TRUE(runner._uft_engine._ctx_map.empty()) << item.first;
			EXPECT_TRUE(runner.shutdown()) << item.first;
		}
		const std::string events = read_text(eventLog);
		EXPECT_EQ(std::string::npos, events.find("strategy.create:"));
		EXPECT_EQ(std::string::npos, events.find("strategy.init:"));
		EXPECT_EQ(std::string::npos, events.find("strategy.ready:"));
		return;
	}

	FAIL() << "unknown R95 strategy probe mode: " << mode;
}

TEST(r95_uft_strategy, active_init_false_rejects_full_config_and_keeps_run_closed)
{
	ScopedTempCwd cwd("strategy_init_false");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(stageR95UftStrategyFixture());
	ASSERT_TRUE(write_text("cfg.yaml",
		"basefiles:\n"
		"    session: sessions.yaml\n"
		"env:\n"
		"    product:\n"
		"        session: ALLDAY\n"
		"data:\n"
		"    unused: 1\n"
		"bspolicy: actpolicy.json\n"
		"strategies:\n"
		"    uft:\n"
		"        - active: true\n"
		"          id: fail_only\n"
		"          name: R95FakeUft.Failure\n"
		"          params:\n"
		"              succeed: false\n"));
	const fs::path eventLog = fs::absolute("strategy.log");
	ScopedEnvVar logEnv("WT_R95_UFT_STRATEGY_LOG", eventLog.string());
	ASSERT_TRUE(logEnv.ok());

	const bool childPassed = spawnIsolatedUftStrategyProbe("full_failure");
	const std::vector<std::string> events = readLifecycleEvents(eventLog);
	EXPECT_TRUE(childPassed) << read_text(eventLog);
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"strategy.create:fail_only",
		"strategy.init:fail_only:false",
		"strategy.destroy:fail_only",
		"factory.destroy"}), events);
}

TEST(r95_uft_strategy, later_init_false_does_not_publish_earlier_context_or_ready_sink)
{
	ScopedTempCwd cwd("strategy_atomic_failure");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(stageR95UftStrategyFixture());
	ASSERT_TRUE(write_text("cfg.yaml",
		"strategies:\n"
		"    uft:\n"
		"        - active: true\n"
		"          id: first_success\n"
		"          name: R95FakeUft.Success\n"
		"          trader: ready_trader\n"
		"          params:\n"
		"              succeed: true\n"
		"        - active: true\n"
		"          id: second_failure\n"
		"          name: R95FakeUft.Failure\n"
		"          trader: ready_trader\n"
		"          params:\n"
		"              succeed: false\n"));
	const fs::path eventLog = fs::absolute("strategy.log");
	ScopedEnvVar logEnv("WT_R95_UFT_STRATEGY_LOG", eventLog.string());
	ASSERT_TRUE(logEnv.ok());

	const bool childPassed = spawnIsolatedUftStrategyProbe("multi_failure");
	const std::string events = read_text(eventLog);
	EXPECT_TRUE(childPassed) << events;
	EXPECT_NE(std::string::npos, events.find("strategy.init:first_success:true"));
	EXPECT_NE(std::string::npos, events.find("strategy.init:second_failure:false"));
	EXPECT_EQ(std::string::npos, events.find("strategy.ready:"));
}

TEST(r95_uft_strategy, duplicate_active_ids_fail_before_strategy_creation_or_publication)
{
	ScopedTempCwd cwd("strategy_duplicate_ids");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(stageR95UftStrategyFixture());
	ASSERT_TRUE(write_text("cfg.yaml",
		"strategies:\n"
		"    uft:\n"
		"        - active: true\n"
		"          id: duplicate\n"
		"          name: R95FakeUft.Success\n"
		"          trader: ready_trader\n"
		"          params:\n"
		"              succeed: true\n"
		"        - active: true\n"
		"          id: duplicate\n"
		"          name: R95FakeUft.Failure\n"
		"          trader: ready_trader\n"
		"          params:\n"
		"              succeed: true\n"));
	const fs::path eventLog = fs::absolute("strategy.log");
	ScopedEnvVar logEnv("WT_R95_UFT_STRATEGY_LOG", eventLog.string());
	ASSERT_TRUE(logEnv.ok());

	const bool childPassed = spawnIsolatedUftStrategyProbe("duplicate_ids");
	const std::string events = read_text(eventLog);
	EXPECT_TRUE(childPassed) << events;
	EXPECT_EQ(std::string::npos, events.find("factory.create"));
	EXPECT_EQ(std::string::npos, events.find("strategy.create:"));
	EXPECT_EQ(std::string::npos, events.find("strategy.ready:"));
}

TEST(r95_uft_strategy, ready_callback_is_synchronous_after_successful_context_publication)
{
	ScopedTempCwd cwd("strategy_ready_sync");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(stageR95UftStrategyFixture());
	ASSERT_TRUE(write_text("cfg.yaml",
		"strategies:\n"
		"    uft:\n"
		"        - active: true\n"
		"          id: ready_success\n"
		"          name: R95FakeUft.Success\n"
		"          trader: ready_trader\n"
		"          params:\n"
		"              succeed: true\n"));
	const fs::path eventLog = fs::absolute("strategy.log");
	ScopedEnvVar logEnv("WT_R95_UFT_STRATEGY_LOG", eventLog.string());
	ASSERT_TRUE(logEnv.ok());

	const bool childPassed = spawnIsolatedUftStrategyProbe("ready_success");
	const std::string events = read_text(eventLog);
	EXPECT_TRUE(childPassed) << events;
	const std::size_t initAt = events.find("strategy.init:ready_success:true");
	const std::size_t readyAt = events.find("strategy.ready:ready_success:");
	EXPECT_NE(std::string::npos, initAt);
	EXPECT_NE(std::string::npos, readyAt);
	EXPECT_LT(initAt, readyAt);
}

TEST(r95_uft_strategy, absent_empty_and_inactive_strategy_configs_remain_successful)
{
	ScopedTempCwd cwd("strategy_empty_compat");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(stageR95UftStrategyFixture());
	const fs::path eventLog = fs::absolute("strategy.log");
	ScopedEnvVar logEnv("WT_R95_UFT_STRATEGY_LOG", eventLog.string());
	ASSERT_TRUE(logEnv.ok());

	ASSERT_TRUE(write_text("cfg_missing.yaml", "unused: value\n"));
	ASSERT_TRUE(write_text("cfg_no_uft.yaml", "strategies:\n    cta: []\n"));
	ASSERT_TRUE(write_text("cfg_empty_uft.yaml", "strategies:\n    uft: []\n"));
	ASSERT_TRUE(write_text("cfg_inactive.yaml",
		"strategies:\n"
		"    uft:\n"
		"        - active: false\n"
		"          id: inactive\n"
		"          name: R95FakeUft.Failure\n"
		"          params:\n"
		"              succeed: false\n"));

	const bool childPassed = spawnIsolatedUftStrategyProbe("empty_compat");
	const std::string events = read_text(eventLog);
	EXPECT_TRUE(childPassed) << events;
	EXPECT_EQ(std::string::npos, events.find("strategy.create:"));
	EXPECT_EQ(std::string::npos, events.find("strategy.init:"));
	EXPECT_EQ(std::string::npos, events.find("strategy.ready:"));
}

/* -------------------------------------------------------------------------
 * 一、config() 分支覆盖
 * ---------------------------------------------------------------------- */

TEST(r95_uft_config, active_parser_init_failure_rejects_array_config)
{
	ScopedTempCwd guard("cfg_a");
	ASSERT_TRUE(guard.ok()) << "无法进入临时工作目录";
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_a.yaml", kConfigA));

	WtUftRunner runner;
	// Valid basefiles/env/data/policy; the active parser fails before loading an API.
	// This parser-only config must propagate failure without registering a candidate.
	EXPECT_FALSE(runner.config("cfg_a.yaml"));
	EXPECT_TRUE(runner._parsers._adapters.empty());
	EXPECT_TRUE(runner._quarantined_parsers.empty());
	EXPECT_FALSE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
}

TEST(r95_uft_config, active_parser_init_failure_rejects_external_config)
{
	ScopedTempCwd guard("cfg_b");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("parsers.yaml", kParsersYaml));
	ASSERT_TRUE(write_text("cfg_b.yaml", kConfigB));

	WtUftRunner runner;
	// commodity/contract 走 VT_Array，parser 配置从独立 YAML 读取；该文件仅包含
	// inactive 项与 active 缺模块项，不包含 trader 配置。
	EXPECT_FALSE(runner.config("cfg_b.yaml"));
	EXPECT_TRUE(runner._parsers._adapters.empty());
	EXPECT_TRUE(runner._quarantined_parsers.empty());
	EXPECT_FALSE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
}

TEST(r95_uft_config, missing_trader_config_file_rejects_startup)
{
	ScopedTempCwd guard("cfg_c");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_c.yaml", kConfigC));

	WtUftRunner runner;
	// 未配置 parsers；traders 文件缺失被单独检出并拒绝启动。
	EXPECT_FALSE(runner.config("cfg_c.yaml"));
}

TEST(r95_uft_config, unsupported_parser_file_extension_rejects_startup)
{
	ScopedTempCwd guard("cfg_d");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("parsers.cfg", "not: a-supported-extension\n"));
	ASSERT_TRUE(write_text("cfg_d.yaml", kConfigD));

	WtUftRunner runner;
	// 文件存在但 WTSCfgLoader 因后缀不认识返回 NULL；配置在 parser 阶段结束。
	EXPECT_FALSE(runner.config("cfg_d.yaml"));
}

TEST(r95_uft_config, parser_without_key_rejects_startup)
{
	ScopedTempCwd guard("cfg_e");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("parsers_no_key.yaml", "other: value\n"));
	ASSERT_TRUE(write_text("cfg_e.yaml", kConfigE));

	WtUftRunner runner;
	// initParsers 收到 NULL 并返回 false；该 parser-only 配置未提供 trader。
	EXPECT_FALSE(runner.config("cfg_e.yaml"));
}

TEST(r95_uft_config, traders_object_rejects_startup_without_parser_config)
{
	ScopedTempCwd guard("traders_object");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_traders_object.yaml", kConfigTraderObject));

	WtUftRunner runner;
	EXPECT_FALSE(runner.config("cfg_traders_object.yaml"));
}

TEST(r95_uft_parser, active_missing_module_fails_config_and_runner_start)
{
	ScopedTempCwd guard("parser_missing");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_parser_missing.yaml", parserRunnerConfig(
		"    - active: false\n"
		"      id: parser_inactive_missing\n"
		"      module: no_such_parser_module\n"
		"    - active: true\n"
		"      id: parser_active_missing\n"
		"      module: no_such_parser_module\n")));

	WtUftRunner runner;
	EXPECT_FALSE(runner.config("cfg_parser_missing.yaml"));
	EXPECT_FALSE(runner._config_succeeded.load(std::memory_order_acquire));
	EXPECT_TRUE(runner._parsers._adapters.empty());
	EXPECT_TRUE(runner._quarantined_parsers.empty());
	EXPECT_FALSE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
	EXPECT_FALSE(runner.startParsers());
	EXPECT_FALSE(runner._parser_start_attempted);
	EXPECT_TRUE(runReturnsBeforeSignal(runner));
}

TEST(r95_uft_parser, inactive_missing_module_is_skipped)
{
	ScopedTempCwd guard("parser_inactive_missing");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_parser_inactive.yaml", parserRunnerConfig(
		"    - active: false\n"
		"      id: parser_inactive_missing\n"
		"      module: no_such_parser_module\n")));

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_parser_inactive.yaml"));
	EXPECT_TRUE(runner._config_succeeded.load(std::memory_order_acquire));
	EXPECT_TRUE(runner._parsers._adapters.empty());
	EXPECT_TRUE(runner._quarantined_parsers.empty());
	EXPECT_FALSE(runner._parser_quarantine_pending.load(std::memory_order_acquire));
}

TEST(r95_uft_parser, successful_fake_parser_registers_runs_and_quiesces_before_release)
{
	ScopedTempCwd guard("parser_success");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	fs::path stagedPath;
	ASSERT_TRUE(stageR95ParserFixture(stagedPath));
	ScopedInstDir instDir(fs::current_path());

	const fs::path absolutePlugin = fs::absolute(stagedPath);
	DllHandle control = DLLHelper::load_library(absolutePlugin.string().c_str());
	ASSERT_NE(nullptr, control);
	auto resetCounts = r95ParserSymbol<void (*)()>(control, "r130_parser_reset_counts");
	auto proveQuiescent = r95ParserSymbol<void (*)()>(control, "r130_parser_prove_quiescent");
	auto initCount = r95ParserSymbol<uint32_t (*)()>(control, "r130_parser_init_count");
	auto registerCount = r95ParserSymbol<uint32_t (*)()>(control, "r130_parser_register_count");
	auto connectCount = r95ParserSymbol<uint32_t (*)()>(control, "r130_parser_connect_count");
	auto releaseCount = r95ParserSymbol<uint32_t (*)()>(control, "r130_parser_release_count");
	auto deleteCount = r95ParserSymbol<uint32_t (*)()>(control, "r130_parser_delete_count");
	auto unsafeReleaseCount = r95ParserSymbol<uint32_t (*)()>(control, "r130_parser_unsafe_release_count");
	ASSERT_NE(nullptr, resetCounts);
	ASSERT_NE(nullptr, proveQuiescent);
	ASSERT_NE(nullptr, initCount);
	ASSERT_NE(nullptr, registerCount);
	ASSERT_NE(nullptr, connectCount);
	ASSERT_NE(nullptr, releaseCount);
	ASSERT_NE(nullptr, deleteCount);
	ASSERT_NE(nullptr, unsafeReleaseCount);
	resetCounts();

	const std::string parserConfig =
		"    - active: true\n"
		"      id: parser_success\n"
		"      module: R130LifecycleParser\n";
	ASSERT_TRUE(write_text("cfg_parser_success.yaml", parserRunnerConfig(parserConfig)));

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_parser_success.yaml"));
	EXPECT_TRUE(runner._config_succeeded.load(std::memory_order_acquire));
	ParserAdapterPtr adapter = runner._parsers.getAdapter("parser_success");
	ASSERT_NE(nullptr, adapter.get());
	EXPECT_TRUE(adapter->isInitialized());
	EXPECT_EQ(1u, initCount());
	ASSERT_TRUE(runner.startParsers());
	EXPECT_EQ(1u, registerCount());
	EXPECT_EQ(1u, connectCount());

	// This fixture starts no worker thread; its explicit provider barrier is the
	// only permission to release the API after parser startup has returned.
	proveQuiescent();
	ASSERT_TRUE(runner.shutdown());
	EXPECT_TRUE(runner._parsers._adapters.empty());
	EXPECT_EQ(1u, releaseCount());
	EXPECT_EQ(1u, deleteCount());
	EXPECT_EQ(0u, unsafeReleaseCount());
	DLLHelper::free_library(control);
}

TEST(r95_uft_config, configuration_is_one_shot_and_closed_runner_rejects_reconfiguration)
{
	ScopedTempCwd guard("config_once");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_once.yaml", kConfigRun));

	WTSVariant* observedRoot = nullptr;
	WTSVariant* observedEnv = nullptr;
	{
		WtUftRunner runner;
		ASSERT_TRUE(runner.config("cfg_once.yaml"));

		observedRoot = runner._config;
		ASSERT_NE(nullptr, observedRoot);
		observedRoot->retain();
		observedEnv = observedRoot->get("env");
		ASSERT_NE(nullptr, observedEnv);
		observedEnv->retain();

		const uint32_t rootRefsBeforeRetry = observedRoot->retainCount();
		const uint32_t envRefsBeforeRetry = observedEnv->retainCount();
		EXPECT_EQ(2u, rootRefsBeforeRetry); // runner ownership + test observer
		EXPECT_EQ(3u, envRefsBeforeRetry); // root, engine, and test observer
		EXPECT_FALSE(runner.config("cfg_once.yaml"));
		EXPECT_EQ(observedRoot, runner._config);
		EXPECT_EQ(rootRefsBeforeRetry, observedRoot->retainCount());
		EXPECT_EQ(envRefsBeforeRetry, observedEnv->retainCount());

		ASSERT_TRUE(runner.shutdown());
		EXPECT_EQ(nullptr, runner._config);
		EXPECT_EQ(1u, observedRoot->retainCount()); // test observer only
		EXPECT_EQ(3u, observedEnv->retainCount()); // retained root child + engine + test observer
		EXPECT_FALSE(runner.config("cfg_once.yaml"));
	}

	// The runner's value-member engine is destroyed after the runner destructor body.
	// It releases its retained env reference while the test still owns both nodes.
	ASSERT_NE(nullptr, observedEnv);
	EXPECT_EQ(2u, observedEnv->retainCount()); // retained root child + test observer remain
	observedEnv->release();
	observedRoot->release();
}

TEST(r95_uft_config, missing_data_with_valid_base_env_and_policy_rejects_startup)
{
	ScopedTempCwd guard("missing_data");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_missing_data.yaml", kConfigMissingData));
	ASSERT_TRUE(write_text("cfg_valid_data.yaml", kConfigRun));

	{
		WtUftRunner runner;
		ASSERT_FALSE(runner.config("cfg_missing_data.yaml"));

		// A caller that ignores config(false) must not enter startup or the wait loop.
		std::promise<void> completion;
		std::future<void> finished = completion.get_future();
		std::thread runThread([&runner, &completion]() {
			runner.run(false);
			completion.set_value();
		});
		const bool returnedBeforeSignal = finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		if (!returnedBeforeSignal)
			std::raise(SIGINT);
		runThread.join();
		std::signal(SIGINT, SIG_DFL);
		EXPECT_TRUE(returnedBeforeSignal);
	}

	// Configuration is one-shot per object; a new runner can configure the valid control.
	WtUftRunner freshRunner;
	EXPECT_TRUE(freshRunner.config("cfg_valid_data.yaml"));
}

TEST(r95_uft_config, missing_env_with_valid_base_data_and_policy_rejects_startup)
{
	ScopedTempCwd guard("missing_env");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_missing_env.yaml", kConfigMissingEnv));
	ASSERT_TRUE(write_text("cfg_valid_data.yaml", kConfigRun));

	{
		WtUftRunner runner;
		EXPECT_FALSE(runner.config("cfg_missing_env.yaml"));
	}

	// The failed runner has been destroyed; recovery uses a new object, never a
	// second config() attempt on the one-shot instance.
	WtUftRunner freshRunner;
	EXPECT_TRUE(freshRunner.config("cfg_valid_data.yaml"));
}

TEST(r95_uft_config, trader_init_failure_keeps_run_closed)
{
	ScopedTempCwd guard("bad_trader");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_bad_trader.yaml", kConfigBadTrader));

	WtUftRunner runner;
	EXPECT_FALSE(runner.config("cfg_bad_trader.yaml"));
	EXPECT_NO_THROW(runner.run());
}

// Launched with posix_spawn+exec by the parent tests so logger initialization
// and capture happen in a fresh process, outside the full runner test process.
TEST(r85_uft_runner_child, DISABLED_log_capture_probe)
{
	const char* mode = std::getenv("WT_R85_CHILD_MODE");
	const char* output = std::getenv("WT_R85_CAPTURE_LOG");
	ASSERT_NE(nullptr, mode);
	ASSERT_NE(nullptr, output);

	R85LogCapture capture;
	WTSLogger::init(R"({"root":{"level":"error","async":false,"sinks":[]}})", false, &capture);
	ScopedR85LogRegistration logRegistration(capture, fs::path(output));

	if (std::string(mode) == "init_false")
	{
		WTSVariant* traders = makeLifecycleTraderList("runner_init_false", "init_false");
		ASSERT_NE(nullptr, traders);
		{
			WtUftRunner runner;
			EXPECT_FALSE(runner.initTraders(traders));
			EXPECT_EQ(nullptr, runner._traders.getAdapter("runner_init_false").get());

			// Running the manager after init failure must not reach registerSpi/connect.
			runner._traders.run();
		}
		traders->release();
		EXPECT_TRUE(capture.contains("Trader runner_init_false initialization failed; adapter was not registered"));
		return;
	}

	if (std::string(mode) == "config_false")
	{
		WtUftRunner runner;
		EXPECT_FALSE(runner.config("cfg_lifecycle_false.yaml"));
		EXPECT_FALSE(runner._config_succeeded.load(std::memory_order_acquire));
		EXPECT_TRUE(runReturnsBeforeSignal(runner));
		EXPECT_TRUE(capture.contains("Trader runner_config_false initialization failed; adapter was not registered"));
		return;
	}

	FAIL() << "unknown R85 child mode: " << mode;
}

TEST(r85_uft_runner, init_false_plugin_is_logged_cleaned_not_registered_or_run)
{
	ScopedTempCwd cwd("r85_direct_false");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(stageLifecycleTrader());
	const fs::path logPath = fs::absolute("lifecycle.log");
	const fs::path runnerLogPath = fs::absolute("runner.log");
	ScopedEnvVar logEnv("WT_UFT_LIFECYCLE_LOG", logPath.string());
	ASSERT_TRUE(logEnv.ok());

	const bool childPassed = spawnIsolatedLogProbe("init_false", runnerLogPath);
	const std::string runnerLogs = read_text(runnerLogPath);
	EXPECT_TRUE(childPassed) << runnerLogs;
	EXPECT_NE(std::string::npos, runnerLogs.find("Trader runner_init_false initialization failed; adapter was not registered"));
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.init.false",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleEvents(logPath));
}

TEST(r85_uft_runner, full_config_init_false_keeps_runner_run_closed)
{
	ScopedTempCwd cwd("r85_config_false");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(stageLifecycleTrader());
	ASSERT_TRUE(write_text("cfg_lifecycle_false.yaml", lifecycleRunnerConfig("runner_config_false", "init_false")));
	const fs::path logPath = fs::absolute("lifecycle.log");
	const fs::path runnerLogPath = fs::absolute("runner.log");
	ScopedEnvVar logEnv("WT_UFT_LIFECYCLE_LOG", logPath.string());
	ASSERT_TRUE(logEnv.ok());

	const bool childPassed = spawnIsolatedLogProbe("config_false", runnerLogPath);
	const std::string runnerLogs = read_text(runnerLogPath);
	EXPECT_TRUE(childPassed) << runnerLogs;
	EXPECT_NE(std::string::npos, runnerLogs.find("Trader runner_config_false initialization failed; adapter was not registered"));
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.init.false",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleEvents(logPath));
}

TEST(r85_uft_runner, successful_fake_trader_config_registers_and_runs)
{
	ScopedTempCwd cwd("r85_config_success");
	ASSERT_TRUE(cwd.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(stageLifecycleTrader());
	ScopedInstDir instDir(fs::current_path());
	ASSERT_TRUE(write_text("cfg_lifecycle_success.yaml", lifecycleRunnerConfig("runner_config_success", "success")));
	const fs::path logPath = fs::absolute("lifecycle.log");
	ScopedEnvVar logEnv("WT_UFT_LIFECYCLE_LOG", logPath.string());
	ASSERT_TRUE(logEnv.ok());

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_lifecycle_success.yaml"));
	EXPECT_TRUE(runner._config_succeeded.load(std::memory_order_acquire));
	EXPECT_NE(nullptr, runner._traders.getAdapter("runner_config_success").get());
	runner._traders.run();
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.registerSpi",
		"api.connect"}), readLifecycleEvents(logPath));

	ASSERT_TRUE(runner.shutdown());
	EXPECT_EQ((std::vector<std::string>{
		"factory.create",
		"api.init refs=2",
		"api.registerSpi",
		"api.connect",
		"api.unregisterSpi",
		"api.release refs=2",
		"factory.delete.begin",
		"api.destroy refs=2",
		"factory.delete.end"}), readLifecycleEvents(logPath));
}

TEST(r95_uft_config, missing_default_action_policy_rejects_startup)
{
	ScopedTempCwd guard("missing_default_policy");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("invalid_policy.json",
		"{ \"onlygroup\": { \"order\": [{\"action\":\"open\",\"limit\":1}], \"filters\":[\"SHFE.rb\"] } }"));
	ASSERT_TRUE(write_text("cfg_invalid_policy.yaml",
		"basefiles:\n"
		"    session: sessions.yaml\n"
		"env:\n"
		"    product:\n"
		"        session: ALLDAY\n"
		"data:\n"
		"    unused: 1\n"
		"bspolicy: invalid_policy.json\n"
		"traders:\n"
		"    - active: true\n"
		"      id: t_one\n"
		"      module: no_such_trader\n"
		"strategies:\n"
		"    uft:\n"
		"        - active: true\n"
		"          id: s_one\n"
		"          name: s_one\n"
		"          trader: t_one\n"));

	WtUftRunner runner;
	EXPECT_FALSE(runner.config("cfg_invalid_policy.yaml"));

	// Even if a caller ignores config(false) and invokes run(), startup must
	// return before engine, parser, trader, strategy, or wait-loop execution.
	std::promise<void> completion;
	std::future<void> finished = completion.get_future();
	std::thread runThread([&]() {
		runner.run(false);
		completion.set_value();
	});
	const bool returnedBeforeSignal = finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (!returnedBeforeSignal)
		std::raise(SIGINT);
	runThread.join();
	std::signal(SIGINT, SIG_DFL);
	EXPECT_TRUE(returnedBeforeSignal);
}

/* -------------------------------------------------------------------------
 * 二、run()（WtUftRunner.cpp::run）
 *
 * 先 raise(SIGINT)：构造函数安装的回调把 _to_exit 置 true，run() 启动各模块后立即通过退出轮询。
 * 无 share_domain 时不会启动共享域 watcher。
 * ---------------------------------------------------------------------- */
TEST(r95_uft_run, signal_flag_makes_run_return_immediately)
{
	ScopedTempCwd guard("run");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("actpolicy.json", kActionPolicyJson));
	ASSERT_TRUE(write_text("cfg_run.yaml", kConfigRun));

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_run.yaml"));

	// 信号回调将 _to_exit 置 true。
	ASSERT_EQ(std::raise(SIGINT), 0);

	// 有效 ALLDAY session 允许 engine、空 parser/trader 管理器和未初始化的
	// ShareManager 继续运行；因 _to_exit 已为 true 不进入轮询。析构时 engine 停止 ticker。
	EXPECT_NO_THROW(runner.run(false));

	// 还原默认 SIGINT 处理，避免析构后仍挂着指向本栈对象的回调
	std::signal(SIGINT, SIG_DFL);
}

TEST(r95_uft_run, async_mode_returns_after_startup_and_owner_can_shutdown)
{
	ScopedTempCwd guard("async_run");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_async.yaml", kConfigRun));

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_async.yaml"));
	EXPECT_TRUE(runReturnsBeforeSignal(runner, true));
	EXPECT_TRUE(runner._parser_start_attempted);
	EXPECT_FALSE(runner._shutdown_complete.load(std::memory_order_acquire));
	EXPECT_TRUE(runner.shutdown());
	EXPECT_TRUE(runner._shutdown_complete.load(std::memory_order_acquire));
}

TEST(r95_uft_run, synchronous_mode_waits_for_exit_signal)
{
	ScopedTempCwd guard("sync_run");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_sync.yaml", kConfigRun));

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_sync.yaml"));
	EXPECT_FALSE(runReturnsBeforeSignal(runner, false));
	EXPECT_TRUE(runner._shutdown_complete.load(std::memory_order_acquire));
}

TEST(r95_uft_run, unknown_session_returns_before_starting_runner_loop)
{
	ScopedTempCwd guard("unknown_session_run");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("cfg_unknown_session.yaml",
		"basefiles:\n"
		"    session: sessions.yaml\n"
		"env:\n"
		"    product:\n"
		"        session: MISSING_SESSION\n"
		"data:\n"
		"    unused: 1\n"
		"bspolicy: actpolicy_absent.yaml\n"));

	WtUftRunner runner;
	EXPECT_FALSE(runner.config("cfg_unknown_session.yaml"));

	std::promise<void> completion;
	std::future<void> finished = completion.get_future();
	std::thread runThread([&]() {
		runner.run(false);
		completion.set_value();
	});
	const bool returnedBeforeSignal = finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	if (!returnedBeforeSignal)
		std::raise(SIGINT);
	runThread.join();
	std::signal(SIGINT, SIG_DFL);

	EXPECT_TRUE(returnedBeforeSignal);
}

TEST(r95_uft_run, unconfigured_runner_returns_without_entering_loop)
{
	WtUftRunner runner;
	EXPECT_NO_THROW(runner.run(false));
	std::signal(SIGINT, SIG_DFL);
}
