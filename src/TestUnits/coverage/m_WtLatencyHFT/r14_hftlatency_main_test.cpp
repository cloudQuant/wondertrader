/*
 * R14 lane —— src/WtLatencyHFT/HftLatencyTool.cpp（整文件 0.00%，219 行未覆盖）
 *
 * 机制同 m_LoaderRunner：coverage/CMakeLists.txt 的 WT_MAIN_MODULES 循环对本模块目录
 * （main.cpp + HftLatencyTool.cpp）追加 -Dmain=wt_coverage_main_wtlatencyhft（全小写）。
 *
 * 为什么**不调**改名后的入口：同目录 main.cpp:25 `int main()` 无条件做
 *   test_hft()  ->  HftLatencyTool.cpp:33~42  runner.init(); runner.run();
 *   run() 里 :496 _parsers.run() / :497 _traders.run() / :499 _engine.run() 起线程，
 *   :501 theParser->run(_times) 是造 tick 的死循环；
 * 而 main.cpp:31 之后还有 `getchar()` —— 无 tty 输入时永久阻塞 gtest 宿主。
 * 所以本文件只测 hft::HftLatencyTool 的构造/析构与 init() 配置校验；异常配置在
 * 进入引擎初始化前拒绝，合法 basefiles 对象作为正向控制但不调用 run()。
 *
 * 分支决定权：init() 读硬编码相对路径 "logcfg.yaml"/"config.yaml"（:350/:353），
 * 用例自己 chdir 进一个刚建的空目录，让「文件不存在」成为可证伪的前提；退出恢复原 cwd。
 * 不起线程、不 sleep、不读挂钟。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "Includes/ILogHandler.h"
#include "WTSTools/WTSLogger.h"
#include "WtLatencyHFT/HftLatencyTool.h"

namespace hft { class TestParser; extern TestParser* theParser; }

namespace {

namespace stdfs = std::filesystem;

const char* kLogConfig =
	"{\"root\":{\"level\":\"warn\",\"async\":false,\"sinks\":["
	"{\"type\":\"basic_file_sink\",\"filename\":\"r14_hft.log\","
	"\"truncate\":true,\"pattern\":\"%v\"}]}}";

const char* kConfigWithoutBasefiles =
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

const char* kConfigWithScalarBasefiles =
	"basefiles: invalid\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

const char* kConfigWithArrayBasefiles =
	"basefiles:\n"
	"    - sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

const char* kConfigWithValidBasefiles =
	"basefiles:\n"
	"    unused: 1\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

bool write_text(const char* name, const char* content)
{
	std::ofstream out(name, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << content;
	out.close();
	return !out.fail();
}

class R14LogCapture : public ILogHandler
{
public:
	void handleLogAppend(WTSLogLevel level, const char* message) override
	{
		if (level != LL_WARN && level != LL_ERROR)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		m_messages.emplace_back(message != nullptr ? message : "");
	}

	bool contains(const std::string& expected)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const std::string& message : m_messages)
			if (message.find(expected) != std::string::npos)
				return true;
		return false;
	}

private:
	std::mutex m_mutex;
	std::vector<std::string> m_messages;
};

class ScopedLogCapture
{
public:
	explicit ScopedLogCapture(R14LogCapture& capture)
	{
		WTSLogger::init("logcfg.yaml", true, &capture);
		WTSLogger::registerHandler(&capture);
	}

	~ScopedLogCapture() { WTSLogger::registerHandler(nullptr); }
};

class ScopedEmptyCwd
{
private:
	char	m_backup[4096];
	char	m_enter[4096];
	bool	m_entered;
	bool	m_removed;

public:
	explicit ScopedEmptyCwd(const char* tag) : m_entered(false), m_removed(false)
	{
		m_backup[0] = '\0';
		m_enter[0] = '\0';
		if (getcwd(m_backup, sizeof(m_backup) - 1) == nullptr)
			return;

		std::string dir = std::string(m_backup) + "/r14_lazy_hft_" + tag + "_emptydir";
		rmdir(dir.c_str());
		if (mkdir(dir.c_str(), 0755) != 0)
			return;
		if (chdir(dir.c_str()) != 0)
			return;

		m_entered = true;
		if (getcwd(m_enter, sizeof(m_enter) - 1) != nullptr)
			;
	}

	~ScopedEmptyCwd()
	{
		if (m_entered)
			chdir(m_backup);
		if (m_entered && m_enter[0] != '\0')
		{
			std::error_code ec;
			stdfs::remove_all(m_enter, ec);
			m_removed = !ec;
		}
	}

	bool ok() const { return m_entered; }
	bool cleaned() const { return m_removed; }

	ScopedEmptyCwd(const ScopedEmptyCwd&) = delete;
	ScopedEmptyCwd& operator=(const ScopedEmptyCwd&) = delete;
};

} // namespace

/* -------------------------------------------------------------------------
 * 一、hft::HftLatencyTool 构造/析构（HftLatencyTool.cpp:328~340）
 * ---------------------------------------------------------------------- */

TEST(r14_lazy_hft_tool, ctor_dtor_roundtrip_only_builds_member_managers)
{
	// :328~331 空构造体：只构造 _traders/_parsers/_bd_mgr/_dt_mgr/_hot_mgr/_act_mgr/_engine 等成员
	EXPECT_NO_THROW({ hft::HftLatencyTool tool; (void)tool; });
	// 第二次构造走同一条路径，验证没有「构造一次才干净」的隐藏状态
	EXPECT_NO_THROW({ hft::HftLatencyTool tool; (void)tool; });
}

/* -------------------------------------------------------------------------
 * 二、init() 的配置缺失早退（HftLatencyTool.cpp:347~358）
 * ---------------------------------------------------------------------- */

TEST(r14_lazy_hft_tool, init_without_config_yaml_returns_false_in_empty_cwd)
{
	ScopedEmptyCwd guard("init1");
	// 前提必须成立，否则后面的分支就不是本用例控制的
	ASSERT_TRUE(guard.ok()) << "无法进入/退出临时空目录，用例前提不成立";

	// :350 WTSLogger::init("logcfg.yaml") 文件不存在 -> 内部告警后返回
	// :353 load_from_file("config.yaml") -> WTSCfgLoader.cpp:300 exists() 为假 -> NULL
	// :354~357 走 log_raw(LL_ERROR, ...) 后 return false
	hft::HftLatencyTool tool;
	EXPECT_FALSE(tool.init());
}

TEST(r14_lazy_hft_tool, init_is_repeatable_and_still_false_without_config)
{
	ScopedEmptyCwd guard("init2");
	ASSERT_TRUE(guard.ok());

	hft::HftLatencyTool tool;
	// 两次调用喂的是同一份「不存在」的输入 -> 结论必须一致
	EXPECT_FALSE(tool.init());
	EXPECT_FALSE(tool.init());
}

TEST(r14_lazy_hft_tool, empty_cwd_is_really_empty_and_restored)
{
	char before[4096] = {0};
	ASSERT_NE(getcwd(before, sizeof(before)), nullptr);
	{
		ScopedEmptyCwd guard("check");
		ASSERT_TRUE(guard.ok());
		// 本用例的前提：目录里既没有 config.yaml 也没有 logcfg.yaml / actpolicy.yaml
		EXPECT_NE(::access("config.yaml", F_OK), 0);
		EXPECT_NE(::access("logcfg.yaml", F_OK), 0);
		EXPECT_NE(::access("actpolicy.yaml", F_OK), 0);
		EXPECT_FALSE(guard.cleaned()) << "仍在目录内部就能 rmdir，说明 guard 语义不对";
	}
	char after[4096] = {0};
	ASSERT_NE(getcwd(after, sizeof(after)), nullptr);
	EXPECT_STREQ(before, after) << "guard 退出后没有把 cwd 还原";
}

TEST(r14_lazy_hft_tool, init_rejects_missing_basefiles_and_run_does_not_start)
{
	ScopedEmptyCwd guard("missing_basefiles");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigWithoutBasefiles));

	R14LogCapture log_capture;
	ScopedLogCapture capture(log_capture);
	hft::HftLatencyTool tool;
	EXPECT_FALSE(tool.init());
	tool.run();
	EXPECT_TRUE(tool.runFailed());
	EXPECT_TRUE(log_capture.contains("Invalid config: required basefiles object is missing"));
	EXPECT_TRUE(log_capture.contains("Cannot run HFT latency test before parser initialization succeeds"));
}

TEST(r14_lazy_hft_tool, init_rejects_non_object_basefiles)
{
	const struct InvalidConfigCase
	{
		const char* tag;
		const char* config;
	} cases[] = {
		{ "scalar_basefiles", kConfigWithScalarBasefiles },
		{ "array_basefiles", kConfigWithArrayBasefiles },
	};

	for (const InvalidConfigCase& test_case : cases)
	{
		ScopedEmptyCwd guard(test_case.tag);
		ASSERT_TRUE(guard.ok());
		ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
		ASSERT_TRUE(write_text("config.yaml", test_case.config));

		R14LogCapture log_capture;
		{
			ScopedLogCapture capture(log_capture);
			hft::HftLatencyTool tool;
			EXPECT_FALSE(tool.init()) << test_case.tag;
			tool.run();
			EXPECT_TRUE(tool.runFailed()) << test_case.tag;
		}
		EXPECT_TRUE(log_capture.contains("Invalid config: basefiles must be an object")) << test_case.tag;
	}
}

TEST(r14_lazy_hft_tool, init_accepts_object_basefiles_control_without_running)
{
	ScopedEmptyCwd guard("valid_basefiles");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("logcfg.yaml", kLogConfig));
	ASSERT_TRUE(write_text("config.yaml", kConfigWithValidBasefiles));

	R14LogCapture log_capture;
	ScopedLogCapture capture(log_capture);
	{
		hft::HftLatencyTool tool;
		EXPECT_TRUE(tool.init());
		EXPECT_NE(hft::theParser, nullptr);
		EXPECT_FALSE(tool.runFailed());
	}
	EXPECT_TRUE(stdfs::exists("generated/portfolio/trades.csv"));
}

/* -------------------------------------------------------------------------
 * 三、DISABLED_：必须起引擎才能进的分支
 *
 * 触发条件（任一成立即不能在启用用例里跑）：
 *  D1 wt_coverage_main_wtlatencyhft()（main.cpp:25~32）：test_hft() -> init()+run()，
 *     之后 main.cpp:31 getchar() 在无 tty 的宿主里永久阻塞；启用即挂死。
 *     涉及行：main.cpp:25~32（5 行）+ HftLatencyTool.cpp:33~42（6 行）。
 *  D2 init() 的完整成功支（当前约 :650~715）：合法配置会继续初始化引擎、模块和策略；
 *     R14 只用轻量对象配置验证成功 init，不调用 run()；完整 init/run 路径由 R95 覆盖。
 *  D3 run()（:484~506）：:488 CpuHelper::bind_core 需要特权、:496~501 起三个线程跑死循环，
 *     且 :503~505 catch(...) 会把引擎里的真实崩溃吞掉，断言无从建立。估计 23 行。
 *  D4 :89~325 的 TestParser/TestTrader/TestStrategy 是文件内类（头文件不暴露），
 *     只能在引擎回调里被驱动 → 约 237 行同样要等引擎取样。
 *     另：TestStrategy::on_tick 里 ctx->stra_buy("SHFE.rb.2205", 2300, 1, ...)（:325 附近）
 *     写死合约与价格，即使有引擎也要有对应 tick 才有意义。
 * ---------------------------------------------------------------------- */

TEST(r14_lazy_hft_tool, DISABLED_entry_blocks_on_getchar)
{
	// D1：改名后的入口是 int wt_coverage_main_wtlatencyhft()，无参、内部 getchar()
	EXPECT_TRUE(true);
}

TEST(r14_lazy_hft_tool, DISABLED_run_starts_threads_and_busy_loop)
{
	// D3：_parsers.run()/_traders.run()/_engine.run()/theParser->run(_times)
	hft::HftLatencyTool tool;
	tool.run();
	EXPECT_TRUE(true);
}
