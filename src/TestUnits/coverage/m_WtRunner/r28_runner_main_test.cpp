/*
 * R28 lane —— src/WtRunner/WtRunner.cpp（可执行体 main 目标 CoverageTests_m_WtRunner）
 *
 * 为什么这个文件必须存在（诊断结论，详见 manifests/R28_runner_mains.md）：
 *   R6 已有的 9 条启用用例里，4 条在测试栈帧上直接构造 `WtRunner runner;`。
 *   实测这类进程被 **SIGKILL(9)** 打死（无 core、无 DiagnosticReports 记录），
 *   进程退出前没写 LLVM 计数器 → profraw 是 0 字节 → coverage.sh 直接丢弃
 *   （`dropped_empty`），于是 WtRunner.cpp 一行都没记上。剩下 5 条只走
 *   main.cpp 的 --help 分支与全局数据表，同样不进 WtRunner.cpp 的函数体。
 *   同一个类改成 **堆上构造**（new / std::unique_ptr）后 ctor+dtor 全程正常。
 *   常规用例的 WtRunner 一律 unique_ptr 持有；栈/堆构造析构对照仅在隔离子进程中运行。
 *
 * 安全口径：
 *   - 不调用 run()（起线程 + while(!_to_exit) 轮询 + install_signal_hooks）；
 *   - 不链真实行情/交易时段，config() 只走到 parsers/traders/executers 的
 *     「文件不存在 / active:false」支路，避开 dlopen 与 shm 挂载；
 *   - 每条用例自己唯一的夹具文件名，共享 cwd 下不互相踩。
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "Share/DLLHelper.hpp"
#include "Includes/ILogHandler.h"
#include "WtCore/EventNotifier.h"
#include "WtCore/CtaStrategyMgr.h"
#include "WtCore/HftStrategyMgr.h"
#include "WtCore/SelStrategyMgr.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtExecMgr.h"
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDistExecuter.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtCore/CtaStraContext.h"
#include "WtCore/HftStraContext.h"

// Preinclude WtRunner.h's dependencies above so this access shim only changes
// access on WtRunner itself; the macro is removed immediately after the include.
#define private public
#include "WtRunner/WtRunner.h"
#undef private
#include "WtCore/WtHelper.h"
#include "Includes/WTSVariant.hpp"
#include "Share/StdUtils.hpp"
#include "Share/charconv.hpp"

static_assert(sizeof(WtRunner) <= 64 * 1024,
	"WtRunner must remain small enough to exclude a giant stack frame");

TEST(R42HftTraderBinding, LaterUnknownTraderDoesNotPublishEarlierContext)
{
	const char* pluginPath = std::getenv("R42_HFT_PLUGIN");
	if (pluginPath == nullptr || pluginPath[0] == '\0')
		GTEST_SKIP() << "R42_HFT_PLUGIN must name the independently built HFT factory";
	const auto factoryDir = std::filesystem::path(WtHelper::getCWD()) / "hft";
	std::filesystem::create_directories(factoryDir);
	std::filesystem::copy_file(pluginPath, factoryDir / std::filesystem::path(pluginPath).filename(),
		std::filesystem::copy_options::overwrite_existing);
	auto* runner = new WtRunner();
	TraderAdapterPtr trader(new TraderAdapter());
	ASSERT_TRUE(runner->_traders.addAdapter("known", trader));
	WTSVariant* config = WTSVariant::createObject();
	WTSVariant* strategies = WTSVariant::createObject();
	WTSVariant* hft = WTSVariant::createArray();
	for (const auto& entry : {std::pair<const char*, const char*>("first", "known"),
		std::pair<const char*, const char*>("second", "unknown")})
	{
		WTSVariant* unit = WTSVariant::createObject();
		unit->append("active", true);
		unit->append("id", entry.first);
		unit->append("name", "WtHftStraFact.SimpleHft");
		unit->append("trader", entry.second);
		unit->append("params", WTSVariant::createObject(), false);
		hft->append(unit, false);
	}
	strategies->append("hft", hft, false);
	config->append("strategies", strategies, false);
	runner->_config = config;
	HftStraContext probe(nullptr, "r42_probe", false, 0);
	const auto next = probe.id() + 1;
	EXPECT_FALSE(runner->initHftStrategies());
	EXPECT_EQ(nullptr, runner->_hft_engine.getContext(next).get());
	EXPECT_EQ(nullptr, runner->_hft_stra_mgr.getStrategy("first").get());
}

TEST(R42HftTraderBinding, KnownTraderStillPublishesBoundContext)
{
	const char* pluginPath = std::getenv("R42_HFT_PLUGIN");
	if (pluginPath == nullptr || pluginPath[0] == '\0')
		GTEST_SKIP() << "R42_HFT_PLUGIN must name the independently built HFT factory";
	const auto factoryDir = std::filesystem::path(WtHelper::getCWD()) / "hft";
	std::filesystem::create_directories(factoryDir);
	std::filesystem::copy_file(pluginPath, factoryDir / std::filesystem::path(pluginPath).filename(),
		std::filesystem::copy_options::overwrite_existing);
	auto* runner = new WtRunner();
	TraderAdapterPtr trader(new TraderAdapter());
	ASSERT_TRUE(runner->_traders.addAdapter("known", trader));
	WTSVariant* config = WTSVariant::createObject();
	WTSVariant* strategies = WTSVariant::createObject();
	WTSVariant* hft = WTSVariant::createArray();
	WTSVariant* unit = WTSVariant::createObject();
	unit->append("active", true);
	unit->append("id", "valid");
	unit->append("name", "WtHftStraFact.SimpleHft");
	unit->append("trader", "known");
	// I03-NEW-103: SimpleHft validates its parameter schema (BUG-WSN06-002); `code` is required.
	WTSVariant* params = WTSVariant::createObject();
	params->append("code", "SHFE.rb.2510");
	unit->append("params", params, false);
	hft->append(unit, false);
	strategies->append("hft", hft, false);
	config->append("strategies", strategies, false);
	runner->_config = config;
	HftStraContext probe(nullptr, "r42_probe", false, 0);
	const auto next = probe.id() + 1;
	ASSERT_TRUE(runner->initHftStrategies());
	EXPECT_NE(nullptr, runner->_hft_engine.getContext(next).get());
	EXPECT_NE(nullptr, runner->_hft_stra_mgr.getStrategy("valid").get());
}

/* 生产 TU（src/WtRunner/main.cpp）经 -Dmain=wt_coverage_main_wtrunner 改名后的入口 */
int wt_coverage_main_wtrunner(int argc, char* argv[]);

/* clang 覆盖率运行时的落盘文件名开关：fork 出的子进程必须换名字，
   否则子进程 exit() 时写的是父进程已经展开过的 %p 路径，会把父进程的用例计数冲掉 */
extern "C" void __llvm_profile_set_filename(const char*);

/* 生产 TU（src/WtRunner/WtRunner.cpp:26）的自由函数，未在 WtRunner.h 声明 */
const char* getBinDir();

namespace {

class R28ParserCommandApi : public IParserApi
{
public:
	bool init(WTSVariant*) override { return true; }
	bool connect() override { ++calls; if (onConnect) onConnect(); return result; }
	void registerSpi(IParserSpi*) override {}
	void subscribe(const CodeSet&) override {}
	bool result = false;
	int calls = 0;
	std::function<void()> onConnect;
};

class R28ParserCommandEngine final : public WtEngine
{
public:
	void run() override { ++calls; if (throwOnRun) throw std::runtime_error("engine start failure"); }
	void on_bar(const char*, const char*, uint32_t, WTSBarStruct*) override {}
	int calls = 0;
	bool throwOnRun = false;
};

// I03-NEW-045 / 卡9a：经 initExt 注入的旧式 IParserApi 没有回调排空证明，runner 关闭时保留 parser owner，
// 此时析构按设计 fail-closed（std::terminate，进程入口 main.cpp 从不销毁这种 runner）。用例沿用 n01 的做法：
// 关闭成功则正常销毁；关闭失败时必须是“保留 parser owner”而非其他失败，并让 runner 与其 engine 存活到进程退出。
std::vector<std::unique_ptr<WtRunner>>& r28_retained_runners()
{
	static auto* runners = new std::vector<std::unique_ptr<WtRunner>>();
	return *runners;
}
std::vector<std::unique_ptr<R28ParserCommandEngine>>& r28_retained_engines()
{
	static auto* engines = new std::vector<std::unique_ptr<R28ParserCommandEngine>>();
	return *engines;
}
void r28_finish_runner(std::unique_ptr<WtRunner>& runner, std::unique_ptr<R28ParserCommandEngine>& engine)
{
	if (runner->shutdown())
	{
		runner.reset();
		return;
	}
	EXPECT_TRUE(runner->parserOwnersRetained()) << "shutdown failed for a reason other than retained parser owners";
	r28_retained_runners().push_back(std::move(runner));
	r28_retained_engines().push_back(std::move(engine));
}

TEST(R28ParserCommands, engine_exception_does_not_report_success_after_parser_submission)
{
	for (bool async : {false, true})
	{
		auto runner = std::make_unique<WtRunner>();
		auto engine = std::make_unique<R28ParserCommandEngine>();
		runner->_engine = engine.get();
		engine->throwOnRun = true;
		auto parser = std::make_shared<ParserAdapter>();
		auto* api = new R28ParserCommandApi();
		api->result = true;
		ASSERT_TRUE(parser->initExt("throwing", api, engine.get(), &runner->_bd_mgr));
		ASSERT_TRUE(runner->_parsers.addAdapter("throwing", parser));
		runner->run(async);
		EXPECT_TRUE(runner->parserCommandsSubmitted());
		EXPECT_FALSE(runner->runSucceeded());
		EXPECT_EQ(engine->calls, 1);
		r28_finish_runner(runner, engine);
	}
}

TEST(R28ParserCommands, shutdown_during_startup_does_not_report_success)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<R28ParserCommandEngine>();
	runner->_engine = engine.get();
	auto parser = std::make_shared<ParserAdapter>();
	auto* api = new R28ParserCommandApi();
	api->result = true;
	api->onConnect = [&] { EXPECT_FALSE(runner->shutdown()); };
	ASSERT_TRUE(parser->initExt("shutdown", api, engine.get(), &runner->_bd_mgr));
	ASSERT_TRUE(runner->_parsers.addAdapter("shutdown", parser));
	runner->run(true);
	EXPECT_TRUE(runner->parserCommandsSubmitted());
	EXPECT_FALSE(runner->runSucceeded());
	EXPECT_EQ(engine->calls, 0);
	r28_finish_runner(runner, engine);
}

TEST(R28ParserCommands, runner_reports_partial_submission_without_releasing_owners)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<R28ParserCommandEngine>();
	runner->_engine = engine.get();
	auto failed = std::make_shared<ParserAdapter>();
	auto good = std::make_shared<ParserAdapter>();
	auto* failedApi = new R28ParserCommandApi();
	auto* goodApi = new R28ParserCommandApi();
	goodApi->result = true;
	bool callbackSawSubmitted = true;
	failedApi->onConnect = [&] { callbackSawSubmitted = runner->parserCommandsSubmitted(); };
	ASSERT_TRUE(failed->initExt("failure", failedApi, engine.get(), &runner->_bd_mgr));
	ASSERT_TRUE(good->initExt("success", goodApi, engine.get(), &runner->_bd_mgr));
	ASSERT_TRUE(runner->_parsers.addAdapter("failure", failed));
	ASSERT_TRUE(runner->_parsers.addAdapter("success", good));
	EXPECT_FALSE(runner->parserCommandsSubmitted());
	EXPECT_FALSE(runner->runSucceeded());
	runner->run(true);
	EXPECT_FALSE(callbackSawSubmitted);
	EXPECT_FALSE(runner->parserCommandsSubmitted());
	EXPECT_TRUE(runner->runSucceeded());
	EXPECT_EQ(failedApi->calls, 1);
	EXPECT_EQ(goodApi->calls, 1);
	EXPECT_EQ(engine->calls, 1);
	EXPECT_EQ(runner->_parsers.getAdapter("failure"), failed);
	EXPECT_EQ(runner->_parsers.getAdapter("success"), good);
	r28_finish_runner(runner, engine);
}

TEST(R28ParserCommands, runner_reports_success_only_after_connect_returns)
{
	auto runner = std::make_unique<WtRunner>();
	auto engine = std::make_unique<R28ParserCommandEngine>();
	runner->_engine = engine.get();
	auto parser = std::make_shared<ParserAdapter>();
	auto* api = new R28ParserCommandApi();
	api->result = true;
	bool callbackSawSubmitted = true;
	api->onConnect = [&] { callbackSawSubmitted = runner->parserCommandsSubmitted(); };
	ASSERT_TRUE(parser->initExt("success", api, engine.get(), &runner->_bd_mgr));
	ASSERT_TRUE(runner->_parsers.addAdapter("success", parser));
	runner->run(true);
	EXPECT_FALSE(callbackSawSubmitted);
	EXPECT_TRUE(runner->parserCommandsSubmitted());
	EXPECT_TRUE(runner->runSucceeded());
	EXPECT_EQ(api->calls, 1);
	EXPECT_EQ(engine->calls, 1);
	r28_finish_runner(runner, engine);
}

TEST(R28ParserCommands, synchronous_run_reports_only_after_loop_returns)
{
	for (bool result : {false, true})
	{
		auto runner = std::make_unique<WtRunner>();
		auto engine = std::make_unique<R28ParserCommandEngine>();
		runner->_engine = engine.get();
		auto parser = std::make_shared<ParserAdapter>();
		auto* api = new R28ParserCommandApi();
		api->result = result;
		api->onConnect = [&] { runner->_to_exit.store(true); };
		ASSERT_TRUE(parser->initExt("sync", api, engine.get(), &runner->_bd_mgr));
		ASSERT_TRUE(runner->_parsers.addAdapter("sync", parser));
		runner->run(false);
		EXPECT_EQ(runner->parserCommandsSubmitted(), result);
		EXPECT_TRUE(runner->runSucceeded());
		EXPECT_EQ(api->calls, 1);
		EXPECT_EQ(runner->_parsers.getAdapter("sync"), parser);
		r28_finish_runner(runner, engine);
	}
}

/* 每条用例一个前缀，夹具与产物都在共享 cwd 下，必须互不同名 */
const char* kTag = "r28wt";

struct ChildResult
{
	int status = -1;      // waitpid 的原始 status
	bool exited = false;
	int code = -1;
	int signal = -1;
	std::string out;
};

struct RunnerLifecycleResult
{
	bool forked = false;
	bool timed_out = false;
	bool reaped = false;
	int status = -1;
	int wait_error = 0;
	std::string markers;
};

/*
 * 子进程的计数文件必须落在「和父进程同一个目录」，否则 coverage.sh 的
 * `find $PROF_DIR -name '*.profraw'` 收不到，main.cpp 的覆盖就凭空归零（实测第一轮就是这样）。
 * 做法：读 case_runner.sh 导出的 LLVM_PROFILE_FILE（形如 <profdir>/<tag>-%p.profraw），
 * 自己把 %p 展开成父进程 pid，再插一段本用例专属的后缀，保证父子不互相覆盖。
 * 注意 __llvm_profile_set_filename 给的名字里不要再留 %p/%m，运行时对显式设置的名字
 * 不再做修饰符展开。
 */
std::string child_profpath(const std::string& name)
{
	const char* env = getenv("LLVM_PROFILE_FILE");
	std::string base = (env != nullptr && *env != '\0') ? std::string(env) : std::string("./%p.profraw");

	// 手工展开 %p（不依赖 StrUtil，避免为一条路径再拉一个头）
	const std::string pid = std::to_string(static_cast<long>(getpid()));
	for (std::string::size_type pos = base.find("%p"); pos != std::string::npos; pos = base.find("%p", pos + pid.size()))
		base.replace(pos, 2, pid);

	const std::string suffix = std::string("_") + name + ".profraw";
	const std::string::size_type dot = base.rfind(".profraw");
	if (dot == std::string::npos)
		return base + suffix;
	return base.substr(0, dot) + suffix;
}

/*
 * 在子进程里调用生产入口。必须是子进程：
 *  - `-h/--help` 走 cppcli.hpp:1897-1900 的 std::exit(0)，父进程里调等于把 gtest 宿主提前收尾
 *    （实测判决是 NORUN，不是 PASS）；
 *  - 非 help 分支会执行 main.cpp:104 的栈上 `WtRunner runner;`，实测 SIGKILL(9)。
 * 子进程读到的 stdout 回收给用例断言。
 */
ChildResult entry_in_child(std::vector<const char*> words, const std::string& name)
{
	ChildResult r;
	int fds[2];
	if (pipe(fds) != 0)
		return r;

	pid_t pid = fork();
	if (pid < 0)
	{
		close(fds[0]);
		close(fds[1]);
		return r;
	}

	if (pid == 0)
	{
		// 子进程：换计数文件名 -> stdout 接管道 -> stderr 丢弃 -> 调入口
		__llvm_profile_set_filename(child_profpath(name).c_str());
		dup2(fds[1], STDOUT_FILENO);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0)
		{
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		close(fds[0]);
		close(fds[1]);

		std::vector<char*> argv;
		argv.reserve(words.size());
		for (const char* w : words)
			argv.push_back(const_cast<char*>(w));
		const int rc = wt_coverage_main_wtrunner(static_cast<int>(argv.size()), argv.data());
		// 入口真的返回了（说明 help 分支以外没有 exit/崩）
		_exit(rc == 0 ? 0 : 3);
	}

	close(fds[1]);
	const int saved = fds[0];
	char buf[4096];
	ssize_t got = 0;
	while ((got = read(saved, buf, sizeof(buf))) > 0)
		r.out.append(buf, static_cast<size_t>(got));
	close(saved);

	int raw = 0;
	waitpid(pid, &raw, 0);
	r.status = raw;
	r.exited = WIFEXITED(raw);
	r.code = r.exited ? WEXITSTATUS(raw) : -1;
	r.signal = WIFSIGNALED(raw) ? WTERMSIG(raw) : -1;
	return r;
}

/*
 * 隔离诊断 WtRunner 的构造/析构。子进程只构造、写 C、析构、写 D；
 * 父进程用 WNOHANG 最多观察 2 秒，超时即杀死并回收子进程。
 * 只读至多 3 字节的 marker 管道，不做等待 EOF 的无界读取。
 */
RunnerLifecycleResult runner_lifecycle_in_child(bool useHeap)
{
	RunnerLifecycleResult r;
	int fds[2];
	if (pipe(fds) != 0)
		return r;

	const pid_t pid = fork();
	if (pid < 0)
	{
		close(fds[0]);
		close(fds[1]);
		return r;
	}

	if (pid == 0)
	{
		close(fds[0]);
		struct rlimit coreLimit;
		coreLimit.rlim_cur = 0;
		coreLimit.rlim_max = 0;
		if (setrlimit(RLIMIT_CORE, &coreLimit) != 0)
			_exit(125);

		bool wroteC = false;
		try
		{
			if (useHeap)
			{
				{
					std::unique_ptr<WtRunner> runner(new WtRunner());
					wroteC = write(fds[1], "C", 1) == 1;
				}
			}
			else
			{
				{
					WtRunner runner;
					wroteC = write(fds[1], "C", 1) == 1;
				}
			}
		}
		catch (...)
		{
			_exit(124);
		}

		if (!wroteC)
			_exit(123);
		if (write(fds[1], "D", 1) != 1)
			_exit(122);
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	r.forked = true;
	int raw = -1;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for (;;)
	{
		const pid_t waited = waitpid(pid, &raw, WNOHANG);
		if (waited == pid)
		{
			r.reaped = true;
			break;
		}
		if (waited < 0 && errno != EINTR)
		{
			r.wait_error = errno;
			(void)kill(pid, SIGKILL);
			pid_t reaped;
			do
			{
				reaped = waitpid(pid, &raw, 0);
			} while (reaped < 0 && errno == EINTR);
			r.reaped = reaped == pid;
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			r.timed_out = true;
			(void)kill(pid, SIGKILL);
			pid_t reaped;
			do
			{
				reaped = waitpid(pid, &raw, 0);
			} while (reaped < 0 && errno == EINTR);
			r.reaped = reaped == pid;
			break;
		}
		usleep(10000);
	}

	if (r.reaped)
	{
		r.status = raw;
		const int readFlags = fcntl(fds[0], F_GETFL, 0);
		if (readFlags >= 0 && fcntl(fds[0], F_SETFL, readFlags | O_NONBLOCK) == 0)
		{
			char markers[3] = {};
			const ssize_t got = read(fds[0], markers, sizeof(markers));
			if (got > 0)
				r.markers.assign(markers, static_cast<size_t>(got));
		}
	}
	close(fds[0]);
	return r;
}

std::string fixture_path(const std::string& name)
{
	return std::string("./") + kTag + "_" + name;
}

void write_fixture(const std::string& name, const std::string& body)
{
	StdFile::write_file_content(fixture_path(name).c_str(), body);
}

void write_empty_array_sidecar(const std::string& name, const char* section)
{
	write_fixture(name, std::string(section) + ": []\n");
}

std::unique_ptr<WtRunner> make_runner()
{
	/* 唯一被验证为安全的构造方式：堆。栈上构造会 SIGKILL 宿主（BUG-R28-001） */
	return std::unique_ptr<WtRunner>(new WtRunner());
}

int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_wtrunner(static_cast<int>(argv.size()), argv.data());
}

/* ---- 最小可用基础数据：一份 sessions/commodities/contracts/holidays/hots ---- */
const char* kSessions = R"JSON({
 "SD0930":{"name":"r28","offset":0,"auction":{"from":929,"to":930},
  "sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}
})JSON";

const char* kCommodities = R"JSON({
 "SSE":{"r28p":{"covermode":0,"pricemode":0,"category":1,"precision":1,
  "pricetick":0.01,"volscale":100,"name":"r28p","exchg":"SSE",
  "session":"SD0930","holiday":"CHINA"}}
})JSON";

const char* kContracts = R"JSON({
 "SSE":{"r28p2412":{"name":"r28p2412","code":"r28p2412","exchg":"SSE",
  "product":"r28p","maxlimitqty":20,"maxmarketqty":10}}
})JSON";

const char* kHolidays = R"JSON({"CHINA":["20240101","20240212"]})JSON";

/* 三段式分月合约标准代码（见本轮已知坑：两段式会被 CodeHelper 整段当 product） */
const char* kHots = R"JSON({
 "SSE":{"r28p":[{"date":20240102,"from":"","to":"r28p2412"}]}
})JSON";

const char* kLogCfg = R"YAML(root:
    async: false
    level: error
    sinks:
    -   filename: Logs/r28wt_%s.log
        pattern: '[%Y.%m.%d %H:%M:%S - %-5l] %v'
        truncate: true
        type: daily_file_sink
)YAML";

/* 空规则组：保留必需的 default 组，但不配置订单规则或过滤器 */
const char* kActPolicyEmpty = R"YAML(default:
    order: []
)YAML";

/* 一个有内容的规则组，覆盖 ActionPolicyMgr 的遍历体（不是 WtRunner.cpp 的行，
   但是 config() 里 `if (!initActionPolicy()) return false;` 之后才能继续往下） */
const char* kActPolicyDefault = R"YAML(default:
    order:
    -   action: close
        limit: 0
    -   action: open
        limit: 0
)YAML";

/* 基础数据段的公共部分 */
std::string basefiles_block(const std::string& hotPath = fixture_path("hots.json"))
{
	return std::string("basefiles:\n")
		+ "    session: " + fixture_path("sessions.json") + "\n"
		+ "    commodity: " + fixture_path("commodities.json") + "\n"
		+ "    contract: " + fixture_path("contracts.json") + "\n"
		+ "    holiday: " + fixture_path("holidays.json") + "\n"
		+ "    hot: " + hotPath + "\n";
}

std::string env_block(const char* engineName)
{
	return std::string("env:\n")
		+ "    name: " + engineName + "\n"
		+ "    fees: " + fixture_path("fees.json") + "\n"
		+ "    filters: " + fixture_path("absent_filters.yaml") + "\n"
		+ "    poolsize: 0\n";
}

std::string tail_block()
{
	return std::string("data:\n    store:\n        path: " + fixture_path("data/") + "\n")
		+ "bspolicy: " + fixture_path("actpolicy.yaml") + "\n";
}

/* 写齐 config() 前半段需要的全部夹具 */
void prepare_base_fixtures()
{
	write_fixture("sessions.json", kSessions);
	write_fixture("commodities.json", kCommodities);
	write_fixture("contracts.json", kContracts);
	write_fixture("holidays.json", kHolidays);
	write_fixture("hots.json", kHots);
	write_fixture("logcfg.yaml", kLogCfg);
	write_fixture("actpolicy.yaml", kActPolicyDefault);
	// 迭代03（I03-NEW-008 / 卡13d 费用门禁）：配置了的费用模板路径必须存在；这些用例验证
	// 路由/执行器配置，改用有效空模板（原指向不存在的 absent_fees.json，与 r104 同样处理）。
	write_fixture("fees.json", "{}");
}

class CtaFactoryCwdFixture
{
public:
	CtaFactoryCwdFixture() = default;
	CtaFactoryCwdFixture(const CtaFactoryCwdFixture&) = delete;
	CtaFactoryCwdFixture& operator=(const CtaFactoryCwdFixture&) = delete;

	~CtaFactoryCwdFixture()
	{
		if (_owns_data_storage_module)
		{
			std::error_code ec;
			std::filesystem::remove(_data_storage_module, ec);
		}
		if (_owns_directory)
		{
			std::error_code ec;
			std::filesystem::remove_all(_directory, ec);
		}
	}

	bool install(std::string& error)
	{
		_directory = std::filesystem::path(WtHelper::getCWD()) / "cta";
		std::error_code ec;
		if (std::filesystem::exists(_directory, ec))
		{
			error = "refusing to replace an existing runner cta/ directory";
			return false;
		}
		if (ec)
		{
			error = "could not inspect runner cta/ directory: " + ec.message();
			return false;
		}
		if (!std::filesystem::create_directory(_directory, ec) || ec)
		{
			error = "could not create runner cta/ directory";
			if (ec)
				error += ": " + ec.message();
			return false;
		}
		_owns_directory = true;

		const std::filesystem::path data_storage_source(WT_R28_DATA_STORAGE_PLUGIN_PATH);
		const std::filesystem::file_status data_storage_source_status =
			std::filesystem::symlink_status(data_storage_source, ec);
		if (ec)
		{
			error = "could not inspect real WtDataStorage plugin at " + data_storage_source.string()
				+ ": " + ec.message();
			return false;
		}
		if (std::filesystem::is_symlink(data_storage_source_status)
			|| !std::filesystem::is_regular_file(data_storage_source_status))
		{
			error = "real WtDataStorage plugin is not an ordinary regular file at "
				+ data_storage_source.string();
			return false;
		}

		const std::filesystem::path cwd(WtHelper::getCWD());
		const std::string data_storage_name = DLLHelper::wrap_module("WtDataStorage");
		_data_storage_module = cwd / data_storage_name;
		const std::filesystem::file_status data_storage_target_status =
			std::filesystem::symlink_status(_data_storage_module, ec);
		if (ec == std::errc::no_such_file_or_directory)
			ec.clear();
		else if (ec)
		{
			error = "could not inspect runner WtDataStorage target " + _data_storage_module.string()
				+ ": " + ec.message();
			return false;
		}
		if (data_storage_target_status.type() != std::filesystem::file_type::not_found)
		{
			error = "refusing to replace an existing runner WtDataStorage target: "
				+ _data_storage_module.string();
			return false;
		}
		if (!std::filesystem::copy_file(data_storage_source, _data_storage_module,
			std::filesystem::copy_options::none, ec) || ec)
		{
			error = "could not copy WtDataStorage into isolated runner cwd root";
			if (ec)
				error += ": " + ec.message();
			return false;
		}
		_owns_data_storage_module = true;

		const std::filesystem::file_status staged_data_storage_status =
			std::filesystem::symlink_status(_data_storage_module, ec);
		if (ec)
		{
			error = "could not inspect staged WtDataStorage plugin at "
				+ _data_storage_module.string() + ": " + ec.message();
			return false;
		}
		if (std::filesystem::is_symlink(staged_data_storage_status)
			|| !std::filesystem::is_regular_file(staged_data_storage_status))
		{
			error = "staged WtDataStorage plugin is not an ordinary regular file at "
				+ _data_storage_module.string();
			return false;
		}

		const std::string module = DLLHelper::wrap_module("WtCtaStraFact");
		const std::filesystem::path source(WT_R28_CTA_FACTORY_PLUGIN_PATH);
		const std::filesystem::file_status source_status = std::filesystem::symlink_status(source, ec);
		if (ec)
		{
			error = "could not inspect real WtCtaStraFact plugin at " + source.string() + ": " + ec.message();
			return false;
		}
		if (std::filesystem::is_symlink(source_status)
			|| !std::filesystem::is_regular_file(source_status))
		{
			error = "real WtCtaStraFact plugin is not an ordinary regular file at " + source.string();
			return false;
		}
		if (!std::filesystem::copy_file(source, _directory / module, ec) || ec)
		{
			error = "could not copy WtCtaStraFact into isolated runner cta/ directory";
			if (ec)
				error += ": " + ec.message();
			return false;
		}
		return true;
	}

	const std::filesystem::path& directory() const { return _directory; }

private:
	std::filesystem::path _directory;
	bool _owns_directory = false;
	std::filesystem::path _data_storage_module;
	bool _owns_data_storage_module = false;
};

} // namespace

/* =========================================================================
 * 一、WtRunner 构造/析构诊断：仅在 fork 子进程中运行
 * ====================================================================== */

TEST(r28_wtrunner_lifecycle, stack_construct_destroy_reports_cd_in_child)
{
	const RunnerLifecycleResult r = runner_lifecycle_in_child(false);
	ASSERT_TRUE(r.forked) << "pipe/fork failed";
	EXPECT_FALSE(r.timed_out) << "stack constructor/destructor exceeded 2-second watchdog";
	ASSERT_TRUE(r.reaped) << "child was not reaped; wait_error=" << r.wait_error;
	ASSERT_NE(-1, r.status);
	EXPECT_TRUE(WIFEXITED(r.status))
		<< "child signal=" << (WIFSIGNALED(r.status) ? WTERMSIG(r.status) : -1);
	if (WIFEXITED(r.status))
		EXPECT_EQ(0, WEXITSTATUS(r.status));
	EXPECT_EQ("CD", r.markers) << "C marks constructor return; D marks destructor return";
}

TEST(r28_wtrunner_lifecycle, heap_construct_destroy_reports_cd_in_child)
{
	const RunnerLifecycleResult r = runner_lifecycle_in_child(true);
	ASSERT_TRUE(r.forked) << "pipe/fork failed";
	EXPECT_FALSE(r.timed_out) << "heap constructor/destructor exceeded 2-second watchdog";
	ASSERT_TRUE(r.reaped) << "child was not reaped; wait_error=" << r.wait_error;
	ASSERT_NE(-1, r.status);
	EXPECT_TRUE(WIFEXITED(r.status))
		<< "child signal=" << (WIFSIGNALED(r.status) ? WTERMSIG(r.status) : -1);
	if (WIFEXITED(r.status))
		EXPECT_EQ(0, WEXITSTATUS(r.status));
	EXPECT_EQ("CD", r.markers) << "C marks constructor return; D marks destructor return";
}

/* =========================================================================
 * 二、入口 wt_coverage_main_wtrunner（src/WtRunner/main.cpp:51-127）
 *     只能在子进程里调：
 *       - help 分支被 cppcli.hpp:1897-1900 的 std::exit(0) 截断，父进程里调
 *         等于提前收尾整个 gtest 宿主（判决会是 NORUN 而不是 PASS）；
 *       - 非 help 分支第一件事就是 main.cpp:104 的栈上 WtRunner，实测 SIGKILL(9)。
 *     所以这里断言的是「子进程的退出状态 + 子进程 stdout 的实际文本」。
 * ====================================================================== */

TEST(r28_wtrunner_entry, long_help_prints_usage_doc_in_child)
{
	ChildResult r = entry_in_child({"wt_runner", "--help"}, "help1");
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0) << "cppcli help 走 std::exit(0)";
	// 可观察副作用：buildHelpDoc() 把三条 Rule 的说明打全了
	EXPECT_NE(r.out.find("-c | --config"), std::string::npos) << r.out;
	EXPECT_NE(r.out.find("-l | --logcfg"), std::string::npos);
	EXPECT_NE(r.out.find("-h | --help"), std::string::npos);
	EXPECT_NE(r.out.find("gain help doc"), std::string::npos);
}

TEST(r28_wtrunner_entry, short_help_prints_usage_doc_in_child)
{
	ChildResult r = entry_in_child({"wt_runner", "-h"}, "help2");
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0);
	EXPECT_NE(r.out.find("configure filepath"), std::string::npos) << r.out;
}

TEST(r28_wtrunner_entry, help_wins_even_when_c_and_l_are_present)
{
	// main.cpp:84 opt.parse() 一执行完就 exit(0)，-c/-l 的值根本不会被读
	ChildResult r = entry_in_child({"wt_runner", "-c", "./r28wt_absent_a.yaml",
		"-l", "./r28wt_absent_b.yaml", "-h"}, "help3");
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0);
	EXPECT_NE(r.out.find("gain help doc"), std::string::npos) << r.out;
}

TEST(r28_wtrunner_entry, help_first_word_still_exits_zero)
{
	ChildResult r = entry_in_child({"wt_runner", "-h", "-c", "./r28wt_absent_c.yaml"},
		"help4");
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0);
}

TEST(r28_wtrunner_entry, help_line_shape_is_typed_from_opt_impl)
{
	// 覆盖 buildHelpDoc() 的定宽排版输出：列头 + 换行续行都要在
	ChildResult r = entry_in_child({"wt_runner", "--help"}, "help5");
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_NE(r.out.find("MUST-ENTER["), std::string::npos) << r.out;
	EXPECT_NE(r.out.find("DEFAULT->["), std::string::npos) << r.out;
	// -c 的描述超过列宽会被折行，第二行以制表缩进开头
	EXPECT_NE(r.out.find("default"), std::string::npos) << r.out;
}

/* =========================================================================
 * 二、getBinDir()（WtRunner.cpp:26-37）
 * ====================================================================== */

TEST(r28_wtrunner_binroot, second_call_hits_cached_branch)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	const std::string copy(first);
	// basePath 非空 -> if(basePath.empty()) 的假分支，返回同一地址
	EXPECT_EQ(getBinDir(), first);
	EXPECT_FALSE(copy.empty());
}

TEST(r28_wtrunner_binroot, value_matches_process_cwd)
{
	// getBinDir 是 boost::filesystem::initial_path + standardisePath
	std::string cwd = WtHelper::getCWD();
	EXPECT_FALSE(cwd.empty());
	EXPECT_EQ(std::string(getBinDir()), cwd);
}

/* =========================================================================
 * 三、init()（WtRunner.cpp:53-65）
 * ====================================================================== */

TEST(r28_wtrunner_init, missing_logcfg_takes_warn_branch_and_stamps_inst_dir)
{
	WtHelper::setInstDir("");
	std::unique_ptr<WtRunner> runner = make_runner();
	// :55 WTSLogger::init(不存在) -> :57 setInstDir -> :59 !StdFile::exists 真 -> :60 warn
	runner->init("./r28wt_definitely_absent_logcfg.yaml");
	EXPECT_FALSE(WtHelper::getInstDir().empty());
	EXPECT_EQ(std::string(WtHelper::getInstDir()), std::string(getBinDir()));
}

TEST(r28_wtrunner_init, present_logcfg_skips_warn_and_creates_log_dir)
{
	write_fixture("logcfg.yaml", kLogCfg);
	std::unique_ptr<WtRunner> runner = make_runner();
	// :59 StdFile::exists 为真 -> 不进 warn 分支
	runner->init(fixture_path("logcfg.yaml"));
	// 可观察副作用：spdlog 的 daily_file_sink 会建出 Logs/ 目录
	EXPECT_TRUE(StdFile::exists("./Logs")) << "logcfg 里的 Logs/r28wt_%s.log 应该把 Logs/ 建出来";
}

TEST(r28_wtrunner_init, repeat_init_is_idempotent_on_inst_dir)
{
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init("./r28wt_absent_logcfg_1.yaml");
	const std::string after_first = WtHelper::getInstDir();
	runner->init("./r28wt_absent_logcfg_2.yaml");
	EXPECT_EQ(WtHelper::getInstDir(), after_first) << "setInstDir 每次都是同一个 getBinDir()";
}

/* =========================================================================
 * 四、config()（WtRunner.cpp:67-277）
 * ====================================================================== */

TEST(r28_wtrunner_config, missing_file_returns_false)
{
	std::unique_ptr<WtRunner> runner = make_runner();
	// :69 load_from_file 返回 NULL -> :71 error -> :73 return false
	EXPECT_FALSE(runner->config("./r28wt_absent_cfg.yaml"));
}

TEST(r28_wtrunner_config, unparsable_yaml_returns_false_without_throwing)
{
	// WTSCfgLoader.h 承诺文件/内容解析失败返回 NULL；对 WtRunner 来说应转成 false。
	// 这项回归不把 yaml-cpp 异常向外传播当成预期行为（BUG-R28-003）。
	prepare_base_fixtures();
	write_fixture("broken_cfg.yaml", "basefiles: [ this is : not valid\n\t- {{{\n");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	EXPECT_FALSE(runner->config(fixture_path("broken_cfg.yaml")));
}

TEST(r28_wtrunner_config, unparsable_json_returns_false_without_throwing)
{
	// 同一个入口换成 .json 后缀就走 rapidjson（不抛），返回 NULL -> config():71-73
	// 两条错误路径的健壮性不一致，这是对照用例
	write_fixture("broken_cfg.json", "{ \"basefiles\": ][ bad json ");
	std::unique_ptr<WtRunner> runner = make_runner();
	EXPECT_FALSE(runner->config(fixture_path("broken_cfg.json")));
}

TEST(r28_wtrunner_config, valid_json_config_succeeds)
{
	prepare_base_fixtures();
	write_empty_array_sidecar("parsers_empty.yaml", "parsers");
	write_empty_array_sidecar("traders_empty.yaml", "traders");
	write_empty_array_sidecar("executers_empty.yaml", "executers");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	std::string cfg = std::string("{\n")
		+ "  \"basefiles\": {"
		+ "\"session\": \"" + fixture_path("sessions.json") + "\","
		+ "\"commodity\": \"" + fixture_path("commodities.json") + "\","
		+ "\"contract\": \"" + fixture_path("contracts.json") + "\","
		+ "\"holiday\": \"" + fixture_path("holidays.json") + "\","
		+ "\"hot\": \"" + fixture_path("hots.json") + "\"},\n"
		+ "  \"env\": {\"name\": \"cta\", \"fees\": \"" + fixture_path("fees.json")
		+ "\", \"filters\": \"" + fixture_path("absent_filters.yaml") + "\", \"poolsize\": 0},\n"
		+ "  \"data\": {\"store\": {\"module\": \"WtDataStorage\", \"path\": \""
		+ fixture_path("data/") + "\"}},\n"
		+ "  \"bspolicy\": \"" + fixture_path("actpolicy.yaml") + "\",\n"
		+ "  \"parsers\": \"" + fixture_path("parsers_empty.yaml") + "\",\n"
		+ "  \"traders\": \"" + fixture_path("traders_empty.yaml") + "\",\n"
		+ "  \"executers\": \"" + fixture_path("executers_empty.yaml") + "\"\n"
		+ "}\n";
	write_fixture("valid_cfg.json", cfg);
	EXPECT_TRUE(runner->config(fixture_path("valid_cfg.json")));
}

TEST(r28_wtrunner_config, extensionless_or_unknown_suffix_returns_false)
{
	// load_from_file 只认 .json/.yaml/.yml，其它后缀直接 return NULL（:29-31）
	// -> config():71-73 的错误日志支
	write_fixture("cfg_without_suffix", "basefiles: {}\n");
	std::unique_ptr<WtRunner> runner = make_runner();
	EXPECT_FALSE(runner->config(fixture_path("cfg_without_suffix")));
}

TEST(r28_wtrunner_config, empty_file_returns_false)
{
	// load_from_file:11-13 content.empty() -> NULL
	write_fixture("empty_cfg.yaml", "");
	std::unique_ptr<WtRunner> runner = make_runner();
	EXPECT_FALSE(runner->config(fixture_path("empty_cfg.yaml")));
}

TEST(r28_wtrunner_config, full_cta_config_reaches_true_and_writes_portfolio_files)
{
	prepare_base_fixtures();
	write_empty_array_sidecar("parsers_empty.yaml", "parsers");
	write_empty_array_sidecar("traders_empty.yaml", "traders");
	write_empty_array_sidecar("executers_empty.yaml", "executers");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));      // 必须先 init，WtHelper::_inst_dir 才有值

	// 显式指定真实 WtDataStorage 模块；config 成功现要求 manager 检查
	// createDataReader 工厂创建结果，避免用默认配置掩盖缺失插件。
	std::string cfg = basefiles_block() + env_block("cta")
		+ "data:\n    store:\n        module: WtDataStorage\n        path: " + fixture_path("data/") + "\n"
		+ "bspolicy: " + fixture_path("actpolicy.yaml") + "\n"
		+ "parsers: " + fixture_path("parsers_empty.yaml") + "\n"
		+ "traders: " + fixture_path("traders_empty.yaml") + "\n"
		+ "executers: " + fixture_path("executers_empty.yaml") + "\n";
	write_fixture("cta_cfg.yaml", cfg);

	// :77-152 basefiles 全段 -> :154 initEngine -> :156 initDataMgr
	// -> :157-158 initActionPolicy 成功 -> :160-277 继续 -> :276 return true
	EXPECT_TRUE(runner->config(fixture_path("cta_cfg.yaml")));

	// 可观察副作用：WtEngine::init_outputs() 会在 portfolio/ 下建 trades.csv + closes.csv
	const std::string pdir = WtHelper::getPortifolioDir();
	EXPECT_TRUE(StdFile::exists((pdir + "trades.csv").c_str())) << pdir;
	EXPECT_TRUE(StdFile::exists((pdir + "closes.csv").c_str()));
}

TEST(r28_wtrunner_config, hft_env_routes_to_hft_engine_and_skips_executers)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// env.name=hft -> initEngine():386-390 走 _hft_engine 分支，_is_hft=true
	// config():227 `if(!_is_hft)` 假 -> 整段 executers 不读；:263-266 走 initHftStrategies
	std::string cfg = basefiles_block() + env_block("hft") + tail_block();
	write_fixture("hft_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("hft_cfg.yaml")));
}

TEST(r28_wtrunner_config, sel_env_routes_to_sel_engine)
{
	prepare_base_fixtures();
	write_empty_array_sidecar("executers_empty.yaml", "executers");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEngine():381-384 _is_sel=true -> :397-400 _sel_engine
	std::string cfg = basefiles_block() + env_block("sel") + tail_block()
		+ "executers: " + fixture_path("executers_empty.yaml") + "\n";
	write_fixture("sel_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("sel_cfg.yaml")));
}

TEST(r28_wtrunner_config, env_name_empty_defaults_to_cta_branch)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEngine():375 `strlen(name)==0` 真 -> CTA 分支
	std::string cfg = std::string(basefiles_block())
		+ "env:\n    name: \"\"\n    poolsize: 0\n"
		+ tail_block();
	write_fixture("emptyname_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("emptyname_cfg.yaml")));
}

TEST(r28_wtrunner_config, unknown_env_name_falls_through_to_hft_else)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEngine():390-394 兜底 else -> _is_hft = true（连 cta 之外的拼错都归 HFT）
	std::string cfg = std::string(basefiles_block())
		+ "env:\n    name: R28NoSuchEngine\n    poolsize: 0\n"
		+ tail_block();
	write_fixture("badname_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("badname_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_env_section_with_data_present_makes_config_fail)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// data 与 bspolicy 均存在，只缺必需的 env 对象；配置必须受控失败。
	std::string cfg = basefiles_block() + tail_block();
	write_fixture("noenv_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("noenv_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_data_section_makes_config_fail)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// env 完整使 engine 已初始化；缺少 data 时 initDataMgr() 返回 false，
	// config() 应将必需数据管理器初始化失败传递给调用方。
	const std::string cfg = basefiles_block() + env_block("cta")
		+ "bspolicy: " + fixture_path("actpolicy.yaml") + "\n";
	write_fixture("nodata_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("nodata_cfg.yaml")));
}

TEST(r28_wtrunner_config, invalid_data_store_module_makes_config_fail)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// WtDtMgr::initStore cannot load this module. config() must propagate that
	// failure instead of continuing as though the data manager were initialized.
	const std::string cfg = basefiles_block() + env_block("cta")
		+ "data:\n    store:\n        module: R28MissingDataStorage\n        path: " + fixture_path("data/") + "\n"
		+ "bspolicy: " + fixture_path("actpolicy.yaml") + "\n";
	write_fixture("invalid_data_store_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("invalid_data_store_cfg.yaml")));
	// initEngine 已运行，但数据管理初始化失败必须清空未就绪 engine；run 只能安全拒绝。
	EXPECT_NO_THROW(runner->run(false));
}

TEST(r28_wtrunner_config, missing_bspolicy_makes_config_return_false_early)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// ActionPolicyMgr::init("") 加载不到文件返回 false
	// -> config():157-158 `if(!initActionPolicy()) return false;`
	std::string cfg = basefiles_block() + env_block("cta")
		+ "data:\n    store:\n        path: " + fixture_path("data/") + "\n";
	write_fixture("nobs_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("nobs_cfg.yaml")));
}

TEST(r28_wtrunner_config, empty_action_policy_group_still_passes_the_gate)
{
	prepare_base_fixtures();
	write_fixture("emptypolicy.yaml", kActPolicyEmpty);
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// default 组存在且 order 为空 -> ActionPolicyMgr::init 加载合法空规则组
	// basefiles 只有 session/commodity/contract，没有 holiday/hot -> 跳过对应分支
	std::string cfg = std::string("basefiles:\n")
		+ "    session: " + fixture_path("sessions.json") + "\n"
		+ "    commodity: " + fixture_path("commodities.json") + "\n"
		+ "    contract: " + fixture_path("contracts.json") + "\n"
		+ env_block("cta")
		+ "data:\n    store:\n        path: " + fixture_path("data/") + "\n"
		+ "bspolicy: " + fixture_path("emptypolicy.yaml") + "\n";
	write_fixture("emptypolicy_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("emptypolicy_cfg.yaml")));
}

TEST(r28_wtrunner_config, basefiles_rules_section_loads_custom_hot_rules)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// Custom rules accept a valid file and propagate the WTSHotMgr result.
	write_fixture("rule.json", kHots);
	std::string cfg = basefiles_block()
		+ "    rules:\n        r28rule: " + fixture_path("rule.json") + "\n"
		+ env_block("cta") + tail_block();
	write_fixture("rules_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("rules_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_hot_file_fails_and_run_is_skipped)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block(fixture_path("absent_hots.json"))
		+ env_block("cta") + tail_block();
	write_fixture("missing_hot_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("missing_hot_cfg.yaml")));
}

TEST(r28_wtrunner_config, unparsable_hot_file_fails)
{
	prepare_base_fixtures();
	write_fixture("broken_hots.json", "{ not valid json");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block(fixture_path("broken_hots.json"))
		+ env_block("cta") + tail_block();
	write_fixture("broken_hot_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("broken_hot_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_second_hot_file_fails)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block()
		+ "    second: " + fixture_path("absent_seconds.json") + "\n"
		+ env_block("cta") + tail_block();
	write_fixture("missing_second_hot_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("missing_second_hot_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_custom_rule_file_fails)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block()
		+ "    rules:\n        r28rule: " + fixture_path("absent_rule.json") + "\n"
		+ env_block("cta") + tail_block();
	write_fixture("missing_rule_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("missing_rule_cfg.yaml")));
}

TEST(r28_wtrunner_config, basefiles_session_key_absent_skips_load_sessions)
{
	write_fixture("commodities.json", kCommodities);
	write_fixture("contracts.json", kContracts);
	write_fixture("logcfg.yaml", kLogCfg);
	write_fixture("actpolicy.yaml", kActPolicyDefault);
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():78 `if(cfgBF->get("session"))` 假分支
	// I03-NEW-053（整批校验后发布）：会话未加载时引用会话的品种文件整份被拒（记错误日志），合约随之被拒；
	// Runner 配置本身不因此失败，仍走 session 键缺省分支。卡13d 后缺费用模板文件会使配置失败，
	// 因此补一个有效空模板（与 prepare_base_fixtures 一致）。
	write_fixture("fees.json", "{}");
	std::string cfg = std::string("basefiles:\n")
		+ "    commodity: " + fixture_path("commodities.json") + "\n"
		+ "    contract: " + fixture_path("contracts.json") + "\n"
		+ env_block("cta") + tail_block();
	write_fixture("nosession_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("nosession_cfg.yaml")));
}

TEST(r28_wtrunner_config, contract_section_as_array_loads_each_entry)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():95-107 contract 是数组 -> asCString 分支 + getContracts 循环
	std::string cfg = std::string("basefiles:\n")
		+ "    session: " + fixture_path("sessions.json") + "\n"
		+ "    commodity:\n    - " + fixture_path("commodities.json") + "\n"
		+ "    contract:\n    - " + fixture_path("contracts.json") + "\n"
		+ "    holiday: " + fixture_path("holidays.json") + "\n"
		+ env_block("cta") + tail_block();
	write_fixture("array_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("array_cfg.yaml")));
}

TEST(r28_wtrunner_config, parsers_inline_array_with_inactive_entry_loads_zero)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():188-191 parsers 是数组 -> initParsers():434-437 `!active` continue
	// -> :455 "{} parsers loaded"
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "parsers:\n-   id: r28p0\n    active: false\n    module: NoSuchModule\n";
	write_fixture("parsers_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("parsers_cfg.yaml")));
}

TEST(r28_wtrunner_config, traders_inline_array_with_inactive_entry_loads_zero)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():219-222 -> initTraders():543-546 `!active` continue -> :554 log
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "traders:\n-   id: r28t0\n    active: false\n    module: NoSuchModule\n";
	write_fixture("traders_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("traders_cfg.yaml")));
}

TEST(r28_wtrunner_config, parsers_side_file_loads_and_reports_failure)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():166-178 parsers 是字符串且文件存在 -> 二次 load_from_file -> initParsers(NULL)
	// 侧文件里没有 parsers 段 -> initParsers():425-427 cfgParser==NULL -> false
	// -> :176 error "Loading parsers failed"
	write_fixture("parsers_side.yaml", "nothing_here: 1\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "parsers: " + fixture_path("parsers_side.yaml") + "\n";
	write_fixture("parsers_side_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("parsers_side_cfg.yaml")));
}

TEST(r28_wtrunner_config, notifier_section_absent_returns_false_from_init_evt_notifier)
{
	prepare_base_fixtures();
	write_empty_array_sidecar("executers_empty.yaml", "executers");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEvtNotifier():632-634 `cfg==NULL` -> return false（config() 不看返回值）
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers: " + fixture_path("executers_empty.yaml") + "\n";
	write_fixture("nonotifier_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("nonotifier_cfg.yaml")));
}

TEST(r28_wtrunner_config, executers_inline_array_inactive_and_router_absent)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():228-256 executers 数组 -> initExecuters():466-468 加载 executer/ 工厂目录
	// -> 唯一条目 active:false -> continue -> :512 "{} executers loaded"
	// config():259-261 routers 缺失 -> 跳过 loadRouterRules
	// initCtaStrategies(): strategies 缺失表示没有策略，正常继续配置。
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers:\n-   id: r28e0\n    active: false\n";
	write_fixture("exec_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("exec_cfg.yaml")));
}

TEST(r28_wtrunner_config, strategies_object_without_cta_array_is_accepted_as_no_strategy)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initCtaStrategies() treats an absent optional CTA list as no strategies.
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "strategies:\n    hft:\n    -   id: r28h0\n        active: false\n";
	write_fixture("nocta_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("nocta_cfg.yaml")));
}

TEST(r28_wtrunner_config, strategies_cta_inactive_entry_is_skipped)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initCtaStrategies():288-296 cta 数组里唯一条目 active:false -> continue -> return true
	// 不进 createStrategy（那会 dlopen cta/ 工厂目录）
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "strategies:\n    cta:\n    -   id: r28c0\n        active: false\n"
		+ "        name: NoSuchFact.Nope\n        slippage: 1\n";
	write_fixture("ctainactive_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("ctainactive_cfg.yaml")));
}

TEST(r28_wtrunner_strategies, active_strategy_missing_or_unsupported_name_fails_configuration)
{
	prepare_base_fixtures();
	{
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		std::string cfg = basefiles_block() + env_block("cta") + tail_block()
			+ "strategies:\n    cta:\n    -   id: r28_missing_strategy_name\n        active: true\n";
		write_fixture("cta_missing_strategy_name_cfg.yaml", cfg);
		EXPECT_FALSE(runner->config(fixture_path("cta_missing_strategy_name_cfg.yaml")));
	}

	{
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		std::string cfg = basefiles_block() + env_block("cta") + tail_block()
			+ "strategies:\n    cta:\n    -   id: r28_unknown_strategy_factory\n        active: true\n"
			+ "        name: R28NoSuchFactory.SomeUnit\n";
		write_fixture("cta_unknown_strategy_factory_cfg.yaml", cfg);
		EXPECT_FALSE(runner->config(fixture_path("cta_unknown_strategy_factory_cfg.yaml")));
	}

	{
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		std::string cfg = basefiles_block() + env_block("hft") + tail_block()
			+ "strategies:\n    hft:\n    -   id: r28_missing_hft_strategy_name\n        active: true\n";
		write_fixture("hft_missing_strategy_name_cfg.yaml", cfg);
		EXPECT_FALSE(runner->config(fixture_path("hft_missing_strategy_name_cfg.yaml")));
	}

	{
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		std::string cfg = basefiles_block() + env_block("hft") + tail_block()
			+ "strategies:\n    hft:\n    -   id: r28_unknown_hft_strategy_factory\n        active: true\n"
			+ "        name: R28NoSuchFactory.SomeUnit\n";
		write_fixture("hft_unknown_strategy_factory_cfg.yaml", cfg);
		EXPECT_FALSE(runner->config(fixture_path("hft_unknown_strategy_factory_cfg.yaml")));
	}
}

TEST(r28_wtrunner_strategies, real_cta_factory_null_self_is_rejected_and_dual_thrust_configures)
{
	prepare_base_fixtures();
	CtaFactoryCwdFixture factory_fixture;
	std::string fixture_error;
	ASSERT_TRUE(factory_fixture.install(fixture_error)) << fixture_error;

	// Verify the real factory contract directly: unsupported units return a
	// non-null wrapper with a null strategy, while DualThrust is registered.
	CtaStrategyMgr manager;
	const std::string factory_dir = factory_fixture.directory().string();
	ASSERT_TRUE(manager.loadFactories(factory_dir.c_str()));
	CtaStrategyPtr unsupported = manager.createStrategy(
		"WtCtaStraFact.DoesNotExist", "r104_unsupported_unit");
	ASSERT_NE(nullptr, unsupported);
	EXPECT_EQ(nullptr, unsupported->self());

	CtaStrategyPtr supported = manager.createStrategy(
		"WtCtaStraFact.DualThrust", "r104_dual_thrust");
	ASSERT_NE(nullptr, supported);
	ASSERT_NE(nullptr, supported->self());
	EXPECT_EQ(supported, manager.getStrategy("r104_dual_thrust"));

	{
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		CtaStraContext context_id_probe(nullptr, "r104_null_self_context_probe", 0);
		const uint32_t rejected_context_id = context_id_probe.id() + 1;
		const std::string cfg = basefiles_block() + env_block("cta") + tail_block()
			+ "strategies:\n    cta:\n    -   id: r104_null_self\n        active: true\n"
			+ "        name: WtCtaStraFact.DoesNotExist\n        params: {}\n";
		write_fixture("cta_real_factory_null_self_cfg.yaml", cfg);
		EXPECT_FALSE(runner->config(fixture_path("cta_real_factory_null_self_cfg.yaml")));
		EXPECT_EQ(nullptr, runner->_cta_engine.getContext(rejected_context_id));
	}

	{
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		CtaStraContext context_id_probe(nullptr, "r104_dual_thrust_context_probe", 0);
		const uint32_t registered_context_id = context_id_probe.id() + 1;
		const std::string cfg = basefiles_block() + env_block("cta") + tail_block()
			+ "strategies:\n    cta:\n    -   id: r104_dual_thrust_runner\n        active: true\n"
			+ "        name: WtCtaStraFact.DualThrust\n        slippage: 1\n"
			+ "        params:\n            days: 5\n            k1: 0.5\n            k2: 0.5\n"
			+ "            period: d1\n            count: 10\n"
			+ "            code: SSE.r28p2412\n            stock: false\n";
		write_fixture("cta_real_factory_dual_thrust_cfg.yaml", cfg);
		EXPECT_TRUE(runner->config(fixture_path("cta_real_factory_dual_thrust_cfg.yaml")));
		CtaContextPtr context = runner->_cta_engine.getContext(registered_context_id);
		ASSERT_NE(nullptr, context);
		CtaStraContext* strategy_context = dynamic_cast<CtaStraContext*>(context.get());
		ASSERT_NE(nullptr, strategy_context);
		ASSERT_NE(nullptr, strategy_context->get_stragety());
		EXPECT_STREQ("WtCtaStraFact", strategy_context->get_stragety()->getFactName());
		EXPECT_STREQ("DualThrust", strategy_context->get_stragety()->getName());
		CtaStrategyPtr runner_strategy = runner->_cta_stra_mgr.getStrategy("r104_dual_thrust_runner");
		ASSERT_NE(nullptr, runner_strategy);
		EXPECT_EQ(strategy_context->get_stragety(), runner_strategy->self());
	}
}

TEST(r28_wtrunner_config, second_config_call_rereads_from_new_path)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	std::string cfg = basefiles_block() + env_block("cta") + tail_block();
	write_fixture("first_cfg.yaml", cfg);
	// 第一次成功后 _config 指向第一份；第二次路径不存在 -> 走 :69-73，返回 false
	EXPECT_TRUE(runner->config(fixture_path("first_cfg.yaml")));
	EXPECT_FALSE(runner->config("./r28wt_second_absent_cfg.yaml"));
	// 再来一次合法路径，验证对象没有被前一次失败破坏
	EXPECT_TRUE(runner->config(fixture_path("first_cfg.yaml")));
}

TEST(r28_wtrunner_config, parsers_side_file_exists_but_unloadable_logs_error)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():166-187 parsers 是字符串、文件存在但 load_from_file 返回 NULL
	// （.txt 后缀不在白名单）-> :182 error "Loading parser config {} failed"
	write_fixture("parsers_unloadable.txt", "parsers:\n-   id: x\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "parsers: " + fixture_path("parsers_unloadable.txt") + "\n";
	write_fixture("parsers_unload_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("parsers_unload_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_parser_configuration_file_fails)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "parsers: " + fixture_path("absent_parsers.yaml") + "\n";
	write_fixture("missing_parser_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("missing_parser_cfg.yaml")));
}

TEST(r28_wtrunner_config, traders_side_file_with_inactive_array_loads_zero)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():196-216 traders 是字符串且文件存在 -> 二次 load -> initTraders(侧文件的 traders)
	// -> :543-546 active:false continue -> :554 "{} traders loaded"
	write_fixture("traders_side.yaml", "traders:\n-   id: r28t1\n    active: false\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "traders: " + fixture_path("traders_side.yaml") + "\n";
	write_fixture("traders_side_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("traders_side_cfg.yaml")));
}

TEST(r28_wtrunner_config, missing_trader_configuration_file_fails_and_run_is_skipped)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "traders: " + fixture_path("absent_traders.yaml") + "\n";
	write_fixture("missing_trader_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("missing_trader_cfg.yaml")));
}

TEST(r28_wtrunner_config, traders_side_file_unloadable_logs_error)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():212-215 traders 字符串指向存在但加载不出来的文件 -> :214 error
	write_fixture("traders_unloadable.txt", "traders: []\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "traders: " + fixture_path("traders_unloadable.txt") + "\n";
	write_fixture("traders_unload_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("traders_unload_cfg.yaml")));
}

TEST(r28_wtrunner_config, executers_side_file_with_inactive_array_loads_zero)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():231-247 executers 是字符串且文件存在 -> load -> initExecuters + routers 缺省
	// -> initExecuters():470-472 active:false continue -> :512 log
	write_fixture("executers_side.yaml",
		"executers:\n-   id: r28e1\n    active: false\n"
		"routers:\n-   strategy: r28stra\n    executer: r28exec\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers: " + fixture_path("executers_side.yaml") + "\n";
	write_fixture("executers_side_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("executers_side_cfg.yaml")));
}

TEST(r28_wtrunner_config, executers_side_file_with_malformed_router_rules_fails)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initExecuters succeeds with the inactive fixture; the following sidecar
	// router load must propagate WtExecuterMgr's atomic validation failure.
	write_fixture("executers_router_bad.yaml",
		"executers:\n-   id: r28e_bad_router\n    active: false\n"
		"routers:\n-   strategy: r28stra\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers: " + fixture_path("executers_router_bad.yaml") + "\n";
	write_fixture("executers_router_bad_cfg.yaml", cfg);

	EXPECT_FALSE(runner->config(fixture_path("executers_router_bad_cfg.yaml")));
}

TEST(r28_wtrunner_config, executers_side_file_unloadable_logs_error)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():242-246 executer 侧文件加载失败 -> :244 error
	write_fixture("executers_unloadable.txt", "executers: []\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers: " + fixture_path("executers_unloadable.txt") + "\n";
	write_fixture("executers_unload_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("executers_unload_cfg.yaml")));
}

TEST(r28_wtrunner_config, notifier_inactive_entry_short_circuits_evt_notifier_init)
{
	prepare_base_fixtures();
	write_empty_array_sidecar("executers_empty.yaml", "executers");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEvtNotifier():632-637 notifier 是对象 -> _notifier.init(cfg) -> :637 return true
	// EventNotifier::init 第一件事就是 `if(!cfg->getBoolean("active")) return false`
	// （EventNotifier.cpp:60-62），所以不会去 dlopen WtMsgQue、也不会碰任何端口
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "notifier:\n    active: false\n    url: inproc://r28wt_disabled\n"
		+ "executers: " + fixture_path("executers_empty.yaml") + "\n";
	write_fixture("notifier_off_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("notifier_off_cfg.yaml")));
}

TEST(r28_wtrunner_config, notifier_active_entry_fails_to_load_mq_module_and_returns)
{
	prepare_base_fixtures();
	write_empty_array_sidecar("executers_empty.yaml", "executers");
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// EventNotifier.cpp:63-79：wrap_module("WtMsgQue") -> 先查 cwd 再查 inst dir ->
	// 两个目录都没有 libWtMsgQue.dylib（用例的 run 目录是空的）-> load_library NULL
	// -> :74 error -> :75 return false。整条链不开线程、不 bind 端口。
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "notifier:\n    active: true\n    url: tcp://127.0.0.1:55661\n"
		+ "    name: r28wt_notifier\n"
		+ "executers: " + fixture_path("executers_empty.yaml") + "\n";
	write_fixture("notifier_on_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("notifier_on_cfg.yaml")));
}

TEST(r28_wtrunner_config, routers_section_reaches_load_router_rules)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():259-261 `if(cfgRouter != NULL) _cta_engine.loadRouterRules(cfgRouter)`
	// -> WtExecMgr.cpp:210-238 两条分支都要走到：executer 是数组（:221-227）
	//    和 executer 是字符串（:229-233）。同时 executers 段完全缺失 -> :228 cfgExec==NULL
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "routers:\n"
		+ "-   strategy: r28straA\n    executer:\n    -   r28execA1\n    -   r28execA2\n"
		+ "-   strategy: r28straB\n    executer: r28execB\n";
	write_fixture("routers_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("routers_cfg.yaml")));
}

TEST(r28_wtrunner_config, router_rule_without_strategy_fails_configuration)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "routers:\n-   executer: r28exec\n";
	write_fixture("routers_missing_strategy_cfg.yaml", cfg);
	bool configured = true;
	EXPECT_NO_THROW(configured = runner->config(fixture_path("routers_missing_strategy_cfg.yaml")));
	EXPECT_FALSE(configured);
}

TEST(r28_wtrunner_config, router_rule_with_non_string_strategy_json_fails_configuration)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// YAML scalars are represented as strings by WTSCfgLoader; use JSON to
	// preserve the numeric strategy type that the manager must reject.
	std::string cfg = std::string("{\n")
		+ "  \"basefiles\": {\n"
		+ "    \"session\": \"" + fixture_path("sessions.json") + "\",\n"
		+ "    \"commodity\": \"" + fixture_path("commodities.json") + "\",\n"
		+ "    \"contract\": \"" + fixture_path("contracts.json") + "\",\n"
		+ "    \"holiday\": \"" + fixture_path("holidays.json") + "\",\n"
		+ "    \"hot\": \"" + fixture_path("hots.json") + "\"\n"
		+ "  },\n"
		+ "  \"env\": {\"name\": \"cta\", \"fees\": \"" + fixture_path("fees.json")
		+ "\", \"filters\": \"" + fixture_path("absent_filters.yaml") + "\", \"poolsize\": 0},\n"
		+ "  \"data\": {\"store\": {\"path\": \"" + fixture_path("data/") + "\"}},\n"
		+ "  \"bspolicy\": \"" + fixture_path("actpolicy.yaml") + "\",\n"
		+ "  \"routers\": [{\"strategy\": 7, \"executer\": \"r28exec\"}]\n"
		+ "}\n";
	write_fixture("routers_number_strategy_cfg.json", cfg);
	bool configured = true;
	EXPECT_NO_THROW(configured = runner->config(fixture_path("routers_number_strategy_cfg.json")));
	EXPECT_FALSE(configured);
}

TEST(r28_wtrunner_config, traders_side_file_non_array_reports_loading_traders_failed)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// 侧文件里的 traders 写成字符串而不是数组
	// -> initTraders():553-554 `type != VT_Array` -> return false
	// -> config():209 error "Loading traders failed"
	write_fixture("traders_scalar.yaml", "traders: r28_not_an_array\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "traders: " + fixture_path("traders_scalar.yaml") + "\n";
	write_fixture("traders_scalar_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("traders_scalar_cfg.yaml")));
}

TEST(r28_wtrunner_config, executers_side_file_non_array_reports_loading_executers_failed)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// 同上，换成 executers：initExecuters():463-464 return false -> config():246 error
	write_fixture("executers_scalar.yaml", "executers: r28_not_an_array\n");
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers: " + fixture_path("executers_scalar.yaml") + "\n";
	write_fixture("executers_scalar_cfg.yaml", cfg);
	EXPECT_FALSE(runner->config(fixture_path("executers_scalar_cfg.yaml")));
}

/* =========================================================================
 * 五、handleLogAppend()（WtRunner.cpp:614-627）
 * ====================================================================== */

TEST(r28_wtrunner_logappend, all_seven_declared_levels_route_to_notifier)
{
	std::unique_ptr<WtRunner> runner = make_runner();
	// LOG_TAGS[ll-100]，LL_ALL=100..LL_NONE=106（Includes/WTSTypes.h）
	for (uint32_t ll = LL_ALL; ll <= LL_NONE; ll++)
	{
		ASSERT_LT(ll - 100u, 7u) << "级别枚举与 LOG_TAGS 长度不同步: ll=" << ll;
		// _notifier 未 init -> _mq_sid==0 -> notify_log 立刻返回（EventNotifier.cpp:116）
		EXPECT_NO_THROW(runner->handleLogAppend(static_cast<WTSLogLevel>(ll), "r28 probe"))
			<< "ll=" << ll;
	}
}

TEST(r28_wtrunner_logappend, tag_table_content_matches_wtrunner_definition)
{
	// WtRunner.cpp:614-622 —— 与 WtDtPorter/WtPorter 侧同名表各 TU 各定义一份（BUG-R6-010）
	extern const char* LOG_TAGS[];
	EXPECT_STREQ(LOG_TAGS[0], "all");
	EXPECT_STREQ(LOG_TAGS[3], "warn");
	EXPECT_STREQ(LOG_TAGS[5], "fatal");
	EXPECT_STREQ(LOG_TAGS[6], "none");
}

/* =========================================================================
 * 六、DISABLED_：确认会打死宿主 / 挂死的支路
 *
 * 触发条件实测记录（每条都在隔离目录里单跑过，退出码写在注释里）：
 *  K1 测试栈帧上构造 WtRunner（或 WtCtaEngine / WtSelEngine）→ SIGKILL(9)，
 *     进程 0.1s 内消失、无 core、无 DiagnosticReports 记录、peak RSS 只 32MB
 *     （不是栈溢出，sizeof(WtRunner)=5232）；同一类型改成 new 出来则 ctor+dtor 全好。
 *     => main.cpp 里 `WtRunner runner;` 之后的所有行（:104-127）在进程内取不到覆盖。
 *  K2 未认别的长选项 → SIGSEGV(11)。cppcli 对未知开关不返回错误码而是直接崩。
 *  K3 `{}` 这种合法但缺 basefiles 的配置 → WtRunner.cpp:77-78 空指针。
 *  K4 config() 成功后 run() → _parsers.run()/_traders.run() 起线程 +
 *     install_signal_hooks + while(!_to_exit) 10ms 轮询死循环，永不返回。
 *  K5 routers 条目漏写 executer 键 → WtExecMgr.cpp:218 空指针（实测 SIGSEGV(11)）。
 *  K6 strategies.cta 里 active:true 的条目 → 需要 cwd 下有可 dlopen 的 cta/ 工厂目录，
 *     否则 CtaStrategyMgr::createStrategy 返回 NULL 后 :307 空指针。
 * ====================================================================== */

TEST(r28_wtrunner_entry, DISABLED_stack_allocated_runner_is_sigkilled)
{
	// K1：照抄 main.cpp:104 的形状。启用会把 gtest 宿主整个打死、profraw 归零。
	WtRunner runner;
	EXPECT_TRUE(runner.config("./r28wt_absent_cfg.yaml"));
}

TEST(r28_wtrunner_entry, DISABLED_bare_argv_reaches_stack_runner_and_null_engine_run)
{
	// K1 + K4：main.cpp:104 栈上 WtRunner -> :127 runner.run(false) -> :585 空 _engine
	EXPECT_EQ(call_entry({"wt_runner"}), 0);
}

TEST(r28_wtrunner_entry, main_returns_nonzero_and_skips_run_when_hot_config_fails)
{
	prepare_base_fixtures();
	const std::string cfg = basefiles_block(fixture_path("absent_main_hots.json"))
		+ env_block("cta") + tail_block();
	write_fixture("main_bad_hot_cfg.yaml", cfg);

	const ChildResult result = entry_in_child(
		{"wt_runner", "-c", "./r28wt_main_bad_hot_cfg.yaml"}, "main_hot_failure_nonzero");
	ASSERT_TRUE(result.exited) << "main must return after hot configuration failure; signal=" << result.signal;
	EXPECT_EQ(result.code, 3) << "entry_in_child maps any nonzero main result to exit code 3";
}

TEST(r28_wtrunner_entry, unknown_long_option_segv)
{
	// 迭代03 最终验收复核后启用（原禁用：K2 未知长选项 SIGSEGV）。
	EXPECT_NE(call_entry({"wt_runner", "--r28-bogus-flag"}), 0);
}

TEST(r28_wtrunner_config, valid_config_without_basefiles_derefs_null_section)
{
	// 迭代03 最终验收复核后启用（原禁用：K3 缺 basefiles 空指针）。
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	write_fixture("empty_obj_cfg.yaml", "{}\n");
	EXPECT_FALSE(runner->config(fixture_path("empty_obj_cfg.yaml")));
}

TEST(r28_wtrunner_config, router_rule_without_executer_key_fails_configuration)
{
	// Initialize a host executor first; invalid top-level routing must propagate
	// false through failConfig and quiesce the already-registered owner.
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers:\n-   id: r28e_router_failure\n    active: true\n    name: dist\n"
		+ "routers:\n-   strategy: r28stra\n    symbol: r28sym\n";
	write_fixture("badrouter_cfg.yaml", cfg);
	bool configured = true;
	EXPECT_NO_THROW(configured = runner->config(fixture_path("badrouter_cfg.yaml")));
	EXPECT_FALSE(configured);
	EXPECT_NO_THROW(runner->run(false));
}

TEST(r28_wtrunner_strategies, DISABLED_active_cta_entry_needs_real_factory_dir)
{
	// K6：initCtaStrategies():302-310 需要 WtHelper::getCWD()+"cta/" 里真的能 dlopen 到
	// 策略工厂（CtaStrategyMgr::loadFactories），且 createStrategy 返回非空才会继续
	// stra->self()->init(...)。用例的 run 目录里没有 cta/ 工厂 -> createStrategy 返回 NULL
	// -> :307 stra->self() 空指针。启用它需要「工厂目录注入」接缝。
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "strategies:\n    cta:\n    -   id: r28c1\n        active: true\n"
		+ "        name: WtCtaStraFact.DualThrust\n        slippage: 1\n"
		+ "        params:\n            code: SHFE.rb.2510\n";
	write_fixture("ctaactive_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("ctaactive_cfg.yaml")));
}

TEST(r28_wtrunner_config, router_null_preflight_rejects_inline_json_and_yaml_without_mutation)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	WTSVariant* seed = WTSVariant::createArray();
	WTSVariant* rule = WTSVariant::createObject();
	rule->append("strategy", "seed");
	rule->append("executer", "kept");
	seed->append(rule, false);
	ASSERT_TRUE(runner->_cta_engine.getExecuterMgr().load_router_rules(seed));
	seed->release();
	const std::string jsonPrefix = std::string("{\"basefiles\":{\"session\":\"") + fixture_path("sessions.json") + "\",\"commodity\":\"" + fixture_path("commodities.json") + "\",\"contract\":\"" + fixture_path("contracts.json") + "\",\"holiday\":\"" + fixture_path("holidays.json") + "\",\"hot\":\"" + fixture_path("hots.json") + "\"},\"env\":{\"name\":\"cta\"},\"data\":{\"store\":{\"path\":\"" + fixture_path("data/") + "\"}},\"bspolicy\":\"" + fixture_path("actpolicy.yaml") + "\",\"routers\":";
	const std::string yamlPrefix = basefiles_block() + env_block("cta") + tail_block() + "routers: ";
	struct Case { const char* suffix; const char* name; const char* routers; bool json; };
	const Case cases[] = {
		{"json", "rule_leading", "[null,{\"strategy\":\"fresh\",\"executer\":\"x\"}]", true},
		{"json", "rule_trailing", "[{\"strategy\":\"fresh\",\"executer\":\"x\"},null]", true},
		{"json", "nested", "[{\"strategy\":\"fresh\",\"executer\":[\"x\",null,\"y\"]}]", true},
		{"yaml", "rule_leading", "[null, {strategy: fresh, executer: x}]", false},
		{"yaml", "rule_trailing", "[{strategy: fresh, executer: x}, ~]", false},
		{"yaml", "nested", "[{strategy: fresh, executer: [x, null, y]}]", false},
		{"json", "top_nul_key", "[null]", true},
		{"yaml", "top_nul_key", "[null]", false},
		{"json", "nested_nul_key", "[{\"strategy\":\"fresh\",\"executer\\u0000alias\":[\"x\",null]}]", true},
		{"yaml", "nested_nul_key", "[{strategy: fresh, \"executer\\0alias\": [x, null]}]", false},
	};
	for (const auto& c : cases)
	{
		const std::string name = std::string("router_null_inline_") + c.name + "." + c.suffix;
		std::string prefix = c.json ? jsonPrefix : yamlPrefix;
		if (std::string(c.name) == "top_nul_key")
		{
			const std::string key = c.json ? "\"routers\":" : "routers:";
			const std::string alias = c.json ? "\"routers\\u0000alias\":" : "\"routers\\0alias\":";
			prefix.replace(prefix.rfind(key), key.size(), alias);
		}
		write_fixture(name, prefix + c.routers + (c.json ? "}\n" : "\n"));
		EXPECT_FALSE(runner->config(fixture_path(name))) << name;
		const auto& kept = runner->_cta_engine.getExecuterMgr().get_route_by_strategy("seed");
		EXPECT_EQ(1u, kept.size()) << name;
		EXPECT_NE(kept.end(), kept.find("kept")) << name;
		const auto& fresh = runner->_cta_engine.getExecuterMgr().get_route_by_strategy("fresh");
		EXPECT_NE(fresh.end(), fresh.find("ALL")) << name;
	}
}

TEST(r28_wtrunner_config, router_null_preflight_rejects_sidecar_json_and_yaml_without_mutation)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	WTSVariant* seed = WTSVariant::createArray();
	WTSVariant* rule = WTSVariant::createObject();
	rule->append("strategy", "seed");
	rule->append("executer", "kept");
	seed->append(rule, false);
	ASSERT_TRUE(runner->_cta_engine.getExecuterMgr().load_router_rules(seed));
	seed->release();
	struct Case { const char* suffix; const char* name; const char* content; };
	const Case cases[] = {
		{"json", "rule", "{\"executers\":[],\"routers\":[{\"strategy\":\"fresh\",\"executer\":\"x\"},null]}"},
		{"json", "nested", "{\"executers\":[],\"routers\":[{\"strategy\":\"fresh\",\"executer\":[null,\"x\"]}]}"},
		{"yaml", "rule", "executers: []\nrouters: [null, {strategy: fresh, executer: x}]\n"},
		{"yaml", "nested", "executers: []\nrouters: [{strategy: fresh, executer: [x, ~]}]\n"},
		{"json", "top_nul_key", "{\"executers\":[],\"routers\\u0000alias\":[null]}"},
		{"json", "nested_nul_key", "{\"executers\":[],\"routers\":[{\"strategy\":\"fresh\",\"executer\\u0000alias\":[\"x\",null]}]}"},
		{"yaml", "top_nul_key", "executers: []\n\"routers\\0alias\": [null]\n"},
		{"yaml", "nested_nul_key", "executers: []\nrouters: [{strategy: fresh, \"executer\\0alias\": [x, null]}]\n"},
	};
	for (const auto& c : cases)
	{
		const std::string name = std::string("router_null_side_") + c.name + "." + c.suffix;
		write_fixture(name, c.content);
		const std::string cfg = basefiles_block() + env_block("cta") + tail_block() + "executers: " + fixture_path(name) + "\n";
		const std::string mainName = std::string("router_null_side_main_") + c.name + "." + c.suffix + ".yaml";
		write_fixture(mainName, cfg);
		EXPECT_FALSE(runner->config(fixture_path(mainName))) << name;
		const auto& kept = runner->_cta_engine.getExecuterMgr().get_route_by_strategy("seed");
		EXPECT_EQ(1u, kept.size()) << name;
		EXPECT_NE(kept.end(), kept.find("kept")) << name;
		const auto& fresh = runner->_cta_engine.getExecuterMgr().get_route_by_strategy("fresh");
		EXPECT_NE(fresh.end(), fresh.find("ALL")) << name;
	}
}

TEST(r28_wtrunner_config, hft_ignores_unused_router_nulls)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string cfg = basefiles_block() + env_block("hft") + tail_block() + "routers: [null]\n";
	write_fixture("hft_null_router.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("hft_null_router.yaml")));
}

TEST(r28_wtrunner_config, router_sidecar_uses_preflight_snapshot_after_file_removed)
{
	// I03-NEW-077: the observer needs the info-level "Data manager initialized" log,
	// but WTSLogger::init only takes effect once per process and earlier cases in the
	// same process initialise the logger at level error, which hid the log and made
	// the case order dependent. The threadsafe death-test style re-executes the test
	// binary, so this case runs in a fresh process whose own logcfg is the first one.
	// The assertions are unchanged; the child exits 0 only when none of them failed.
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT({
		{
			prepare_base_fixtures();
			std::string logConfig = kLogCfg;
			logConfig.replace(logConfig.find("level: error"), std::strlen("level: error"), "level: info");
			write_fixture("logcfg.yaml", logConfig);
			const std::string side = fixture_path("router_snapshot_side.yaml");
			write_fixture("router_snapshot_side.yaml", "executers: []\nrouters: [{strategy: snapshot, executer: kept}]\n");
			const std::string cfg = basefiles_block() + env_block("cta") + tail_block() + "executers: " + side + "\n";
			write_fixture("router_snapshot_main.yaml", cfg);
			std::unique_ptr<WtRunner> runner = make_runner();
			runner->init(fixture_path("logcfg.yaml"));
			struct DeleteAfterPreflight : ILogHandler
			{
				explicit DeleteAfterPreflight(std::string file) : path(std::move(file)) {}
				void handleLogAppend(WTSLogLevel, const char* message) override
				{
					if (!observed && std::string(message).find("Data manager initialized") != std::string::npos)
					{
						observed = true;
						removed = std::remove(path.c_str()) == 0;
					}
				}
				std::string path;
				bool observed = false;
				bool removed = false;
			} observer(side);
			struct ResetLogHandler { ~ResetLogHandler() { WTSLogger::registerHandler(nullptr); } } reset;
			WTSLogger::registerHandler(&observer);
			EXPECT_TRUE(runner->config(fixture_path("router_snapshot_main.yaml")));
			EXPECT_TRUE(observer.observed);
			EXPECT_TRUE(observer.removed);
			const auto& route = runner->_cta_engine.getExecuterMgr().get_route_by_strategy("snapshot");
			EXPECT_NE(route.end(), route.find("kept"));
		}
		std::exit(::testing::Test::HasFailure() ? 1 : 0);
	}, ::testing::ExitedWithCode(0), "");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(r28_wtrunner_config, router_nul_alias_with_valid_executer_keeps_legacy_key_semantics)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	const std::string side = "{\"executers\":[],\"routers\\u0000alias\":[{\"strategy\":\"alias\",\"executer\\u0000alias\":[\"kept\"]}]}";
	write_fixture("router_alias_valid.json", side);
	const std::string cfg = basefiles_block() + env_block("cta") + tail_block() + "executers: " + fixture_path("router_alias_valid.json") + "\n";
	write_fixture("router_alias_valid_main.yaml", cfg);
	// 迭代03（20261103-fees-groups-review）：配置加载器拒绝原始或解码后含 NUL 的键/字符串，
	// 含 "\u0000" 键的执行器侧车整体加载失败，Runner 配置 fail-closed、不登记任何路由
	// （改前期望：按 NUL 前缀键生效并登记 alias→kept）。
	EXPECT_FALSE(runner->config(fixture_path("router_alias_valid_main.yaml")));
	const auto& route = runner->_cta_engine.getExecuterMgr().get_route_by_strategy("alias");
	EXPECT_EQ(route.end(), route.find("kept"));
}

TEST(r28_wtrunner_config, normalized_loader_does_not_apply_content_encoding_fallback)
{
	// This byte string is invalid UTF-8 and GB2312; JSON's existing parser
	// preserves the quoted raw byte. The normalized API must not transcode it.
	std::string raw = "{\"note\":\"";
	raw.push_back(static_cast<char>(0xff));
	raw += "\"}";
	WTSVariant* parsed = WTSCfgLoader::load_from_normalized_content(raw, false);
	ASSERT_NE(nullptr, parsed);
	const std::string value = parsed->getString("note");
	ASSERT_EQ(1u, value.size());
	EXPECT_EQ(0xffu, static_cast<unsigned char>(value[0]));
	parsed->release();
#ifndef _WIN32
	// The content API intentionally keeps its own historic conversion rule.
	WTSVariant* viaContent = WTSCfgLoader::load_from_content(raw, false);
	EXPECT_EQ(nullptr, viaContent);
	if (viaContent)
		viaContent->release();
#endif
	EXPECT_EQ(nullptr, WTSCfgLoader::load_from_normalized_content("", false));
}

TEST(r28_wtrunner_config, encoded_files_preserve_legacy_loader_normalization)
{
	prepare_base_fixtures();
	const std::string utf8("\xe4\xb8\xad\xe6\x96\x87", 6);
	const std::string gb2312("\xd6\xd0\xce\xc4", 4);
	struct Case { const char* name; std::string text; bool yaml; };
	const Case cases[] = {{"utf8.json", utf8, false}, {"gb2312.json", gb2312, false},
		{"utf8.yaml", utf8, true}, {"gb2312.yaml", gb2312, true}};
	for (const auto& c : cases)
	{
		const std::string path = std::string("encoding_") + c.name;
		const std::string cfg = c.yaml
			? basefiles_block() + env_block("cta") + tail_block() + "note: \"" + c.text + "\"\nrouters: [{strategy: s, executer: x}]\n"
			: std::string("{\"basefiles\":{\"session\":\"") + fixture_path("sessions.json") + "\",\"commodity\":\"" + fixture_path("commodities.json") + "\",\"contract\":\"" + fixture_path("contracts.json") + "\",\"holiday\":\"" + fixture_path("holidays.json") + "\",\"hot\":\"" + fixture_path("hots.json") + "\"},\"env\":{\"name\":\"cta\"},\"data\":{\"store\":{\"path\":\"" + fixture_path("data/") + "\"}},\"bspolicy\":\"" + fixture_path("actpolicy.yaml") + "\",\"note\":\"" + c.text + "\",\"routers\":[{\"strategy\":\"s\",\"executer\":\"x\"}]}";
		write_fixture(path, cfg);
		WTSVariant* legacy = WTSCfgLoader::load_from_file(fixture_path(path));
		ASSERT_NE(nullptr, legacy) << path;
		const std::string legacyValue = legacy->getString("note");
		legacy->release();
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		ASSERT_TRUE(runner->config(fixture_path(path))) << path;
		ASSERT_NE(nullptr, runner->_config);
		EXPECT_EQ(legacyValue, runner->_config->getString("note")) << path;
	}
}

TEST(r28_wtrunner_config, invalid_encoded_file_remains_rejected_by_legacy_and_runner)
{
	prepare_base_fixtures();
	std::string invalid(1, static_cast<char>(0xff));
	invalid += "{\"basefiles\":{},\"env\":{\"name\":\"cta\"},\"data\":{},\"routers\":[null]}";
#ifndef _WIN32
	ASSERT_FALSE(EncodingHelper::isUtf8((unsigned char*)invalid.data(), invalid.size()));
	std::string normalized;
	normalized = ChartoUTF8(invalid);
	ASSERT_TRUE(normalized.empty());
	EXPECT_EQ(nullptr, WTSCfgLoader::load_from_normalized_content(normalized, false));
#endif
	write_fixture("invalid_encoding.json", invalid);
	WTSVariant* legacy = WTSCfgLoader::load_from_file(fixture_path("invalid_encoding.json"));
	ASSERT_EQ(nullptr, legacy);
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	EXPECT_FALSE(runner->config(fixture_path("invalid_encoding.json")));
}

TEST(r28_wtrunner_config, embedded_nul_file_keeps_legacy_parser_prefix_semantics)
{
	prepare_base_fixtures();
	const std::string yaml = basefiles_block() + env_block("cta") + tail_block();
	const std::string json = std::string("{\"basefiles\":{\"session\":\"") + fixture_path("sessions.json") + "\",\"commodity\":\"" + fixture_path("commodities.json") + "\",\"contract\":\"" + fixture_path("contracts.json") + "\",\"holiday\":\"" + fixture_path("holidays.json") + "\",\"hot\":\"" + fixture_path("hots.json") + "\"},\"env\":{\"name\":\"cta\"},\"data\":{\"store\":{\"path\":\"" + fixture_path("data/") + "\"}},\"bspolicy\":\"" + fixture_path("actpolicy.yaml") + "\"}";
	for (bool isYaml : { false, true })
	{
		const std::string name = isYaml ? "encoding_rawnul.yaml" : "encoding_rawnul.json";
		const std::string prefix = isYaml ? yaml : json;
		const std::string content = prefix + std::string(1, '\0') + (isYaml ? "routers: [null]\n" : "{invalid}");
		write_fixture(name, content);
		// 迭代03（20261103-fees-groups-review）：含原始 NUL 的配置文件整体拒绝，不再按 NUL 前的前缀解析
		// （改前期望：加载成功且 Runner 配置成功）。
		WTSVariant* legacy = WTSCfgLoader::load_from_file(fixture_path(name));
		EXPECT_EQ(nullptr, legacy) << name;
		if (legacy != nullptr)
			legacy->release();
		std::unique_ptr<WtRunner> runner = make_runner();
		runner->init(fixture_path("logcfg.yaml"));
		EXPECT_FALSE(runner->config(fixture_path(name))) << name;
	}
}
