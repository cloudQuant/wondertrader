/*
 * R104 lane —— src/WtRunner/WtRunner.cpp 的 initParsers / initTraders / initExecuters
 *
 * 与 R28 的分工：
 *   R28 把 config() 的 basefiles / notifier / strategies / expliciter 早退面全部钉住了，
 *   WtRunner.cpp 停在 328/491；剩下的 163 行里最大的一块是三个子初始化器的
 *   **active:true 体**（:440~455、:476~544、:563~571）。本轮用「模块名刻意不存在」的
 *   插件配置把这三段走通：
 *     ParserAdapter::init   在 WtCore/ParserAdapter.cpp:105~121 处 load_library 失败后
 *                           原地 return false，不起接收线程；
 *     TraderAdapter::init   在 WtCore/TraderAdapter.cpp:163~178 处同样失败即返回；
 *     WtLocalExecuter::init 只读 scale/clear/groups（WtLocalExecuter.cpp:75~157），
 *                           不需要工厂、poolsize=0 时不建线程池；
 *     WtDiffExecuter::init  只读 scale 并从 execdata/ 读一份不存在的 json
 *                           （WtDiffExecuter.cpp:84~122），读不到直接 return；
 *     WtDistExecuter::init  只读 scale（WtDistExecuter.cpp:47~57）。
 *   所以整条 config() 会在 :276 正常返回 true，全程不 dlopen 真实厂商库、不连柜台、不起线程。
 *
 * 安全口径（沿用 R28 的实测结论）：
 *   - WtRunner 必须堆上构造（栈上构造实测 SIGKILL，见 BUG-R28-001）；
 *   - 不调 run()（:579~612，会起解析/交易/引擎线程并进 while(!_to_exit)）；
 *   - 每条用例自己唯一的夹具名（r104wt_ 前缀），与 r6/r28 的夹具不重名。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

#include "Includes/WTSVariant.hpp"
#include "Share/StdUtils.hpp"
#include "WtCore/WtHelper.h"
#include "../n02_wtcore_engines/r160_common.h"

// Pre-include WtRunner's dependencies so the white-box seam below exposes only
// WtRunner's private trader registry in this test translation unit.
#include "Includes/ILogHandler.h"
#include "WtCore/EventNotifier.h"
#include "WtCore/CtaStrategyMgr.h"
#include "WtCore/HftStrategyMgr.h"
#include "WtCore/SelStrategyMgr.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDistExecuter.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#define private public
#include "WtRunner/WtRunner.h"
#undef private

using namespace wtp;

namespace {

namespace runner_fs = std::filesystem;

std::string fixture(const std::string& name)
{
	return std::string("./r104wt_") + name;
}

void write_fixture(const std::string& name, const std::string& body)
{
	StdFile::write_file_content(fixture(name).c_str(), body);
}

/* 形状与 manifests/R28 里验证过的一致，只把品种/合约换成 r104p/r104p2412 避免与 r28 夹具撞码 */
const char* kSessions = R"JSON({
 "SD0930":{"name":"r104","offset":0,"auction":{"from":929,"to":930},
  "sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}
})JSON";

const char* kCommodities = R"JSON({
 "SSE":{"r104p":{"covermode":0,"pricemode":0,"category":1,"precision":1,
  "pricetick":0.01,"volscale":100,"name":"r104p","exchg":"SSE",
  "session":"SD0930","holiday":"CHINA"}}
})JSON";

const char* kContracts = R"JSON({
 "SSE":{"r104p2412":{"name":"r104p2412","code":"r104p2412","exchg":"SSE",
  "product":"r104p","maxlimitqty":20,"maxmarketqty":10}}
})JSON";

const char* kHolidays = R"JSON({"CHINA":["20240101","20240212"]})JSON";

const char* kHots = R"JSON({
 "SSE":{"r104p":[{"date":20240102,"from":"","to":"r104p2412"}]}
})JSON";

const char* kLogCfg = R"YAML(root:
    async: false
    level: error
    sinks:
    -   filename: Logs/r104wt_%s.log
        pattern: '[%Y.%m.%d %H:%M:%S - %-5l] %v'
        truncate: true
        type: daily_file_sink
)YAML";

/* ActionPolicyMgr::init 需要一个能解析出 default 组的文件（WtRunner.cpp:157-160 的闸门） */
const char* kActPolicy = R"YAML(default:
    order:
    -   action: close
        limit: 0
    -   action: open
        limit: 0
)YAML";

std::string basefiles_block()
{
	return std::string("basefiles:\n")
		+ "    session: " + fixture("sessions.json") + "\n"
		+ "    commodity: " + fixture("commodities.json") + "\n"
		+ "    contract: " + fixture("contracts.json") + "\n"
		+ "    holiday: " + fixture("holidays.json") + "\n"
		+ "    hot: " + fixture("hots.json") + "\n";
}

std::string env_block()
{
	return std::string("env:\n")
		+ "    name: cta\n"
		+ "    fees: " + fixture("absent_fees.json") + "\n"
		+ "    filters: " + fixture("absent_filters.yaml") + "\n"
		+ "    poolsize: 0\n";
}

std::string tail_block()
{
	return std::string("data:\n    store:\n        path: ") + fixture("data/") + "\n"
		+ "bspolicy: " + fixture("actpolicy.yaml") + "\n";
}

void prepare_base_fixtures()
{
	write_fixture("sessions.json", kSessions);
	write_fixture("commodities.json", kCommodities);
	write_fixture("contracts.json", kContracts);
	write_fixture("holidays.json", kHolidays);
	write_fixture("hots.json", kHots);
	write_fixture("logcfg.yaml", kLogCfg);
	write_fixture("actpolicy.yaml", kActPolicy);
}

std::unique_ptr<WtRunner> make_runner()
{
	/* 堆上构造：栈上构造实测 SIGKILL 宿主（BUG-R28-001） */
	return std::unique_ptr<WtRunner>(new WtRunner());
}

class ScopedRunnerInstDir
{
public:
	explicit ScopedRunnerInstDir(const std::string& path)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(path.c_str());
	}

	~ScopedRunnerInstDir()
	{
		WtHelper::setInstDir(_old.c_str());
	}

private:
	std::string _old;
};

class RunnerTraderFixture
{
public:
	explicit RunnerTraderFixture(const std::string& tag)
		: _temp(tag)
	{
		// WtRunner::init resolves its initial install directory from the process CWD.
		const std::string initLogPath = _temp.path() + "/runner_logcfg.yaml";
		const std::string initLogCfg =
			"root:\n    async: false\n    level: error\n    sinks:\n"
			"    -   filename: " + _temp.path() + "/runner_%s.log\n"
			"        pattern: '[%Y.%m.%d %H:%M:%S - %-5l] %v'\n"
			"        truncate: true\n        type: daily_file_sink\n";
		StdFile::write_file_content(initLogPath.c_str(), initLogCfg);
		_runner = make_runner();
		_runner->init(initLogPath);

		const std::string moduleDir = r160::find_module_dir("TraderMocker");
		if (moduleDir.empty())
		{
			_error = "libTraderMocker not found in the test executable's bin directory or its parents";
			return;
		}

		_module_name = DLLHelper::wrap_module("TraderMocker", "lib");
		_module_source = runner_fs::path(moduleDir) / _module_name;
		if (!runner_fs::exists(_module_source))
		{
			_error = "TraderMocker build artifact missing: " + _module_source.string();
			return;
		}

		std::error_code ec;
		runner_fs::create_directories(runner_fs::path(_temp.path()) / "traders", ec);
		if (ec)
		{
			_error = "cannot create temporary traders directory: " + ec.message();
			return;
		}
		runner_fs::create_symlink(_module_source,
			runner_fs::path(_temp.path()) / "traders" / _module_name, ec);
		if (ec)
		{
			_error = "cannot stage TraderMocker from " + _module_source.string() + ": " + ec.message();
			return;
		}

		_cwd.reset(new r160::ScopedCwd(_temp.path()));
		if (!_cwd->ok())
		{
			_error = "cannot change CWD to temporary trader fixture";
			return;
		}
		_instdir.reset(new ScopedRunnerInstDir(_temp.path() + "/"));
		// All config/data paths now resolve inside the isolated test directory.
		prepare_base_fixtures();
		_ready = true;
	}

	~RunnerTraderFixture()
	{
		// The real TraderMocker is initialized but never connected; destroy it
		// before its staged module and temporary CWD are removed.
		_runner.reset();
		_instdir.reset();
		_cwd.reset();
	}

	bool ready() const { return _ready; }
	const std::string& error() const { return _error; }
	std::string temp_path() const { return _temp.path(); }
	const runner_fs::path& module_source() const { return _module_source; }
	WtRunner& runner() { return *_runner; }

private:
	r160::TempDir _temp;
	std::unique_ptr<WtRunner> _runner;
	std::unique_ptr<r160::ScopedCwd> _cwd;
	std::unique_ptr<ScopedRunnerInstDir> _instdir;
	std::string _module_name;
	runner_fs::path _module_source;
	std::string _error;
	bool _ready = false;
};

std::string one_trader_block(const std::string& id, std::uint32_t mockerId,
	const std::string& module = "TraderMocker")
{
	return "-   id: " + id + "\n"
		+ "    active: true\n"
		+ "    module: " + module + "\n"
		+ "    mockerid: " + std::to_string(mockerId) + "\n"
		+ "    span: 0\n    udp_port: 0\n";
}

} // namespace

/* =========================================================================
 * 一、executer 的 active 体（WtRunner.cpp:476~548）
 * ====================================================================== */

TEST(r104_wtrunner_config, all_three_executer_kinds_without_external_modules)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture("logcfg.yaml"));

	/*
	 * config():231~268 executers 数组 -> initExecuters():470~548
	 *   :481~507 local（missing trader 留作 executer 的缺失关联分支）
	 *   :508~534 diff （覆盖 diff 初始化分支）
	 *   :535~542 dist
	 */
	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "executers:\n"
		+ "-   id: r104wt_e_local\n    active: true\n    name: local\n    trader: r104wt_t1\n"
		+ "-   id: r104wt_e_diff\n    active: true\n    name: diff\n    trader: r104wt_t1\n"
		+ "-   id: r104wt_e_diff2\n    active: true\n    name: diff\n    trader: r104wt_absent\n"
		+ "-   id: r104wt_e_dist\n    active: true\n    name: dist\n";
	write_fixture("all_children.yaml", cfg);

	EXPECT_TRUE(runner->config(fixture("all_children.yaml")));

	// 可观察副作用：WtEngine::init_outputs() 在 portfolio/ 下建 trades.csv / closes.csv
	const std::string pdir = WtHelper::getPortifolioDir();
	EXPECT_TRUE(StdFile::exists((pdir + "trades.csv").c_str())) << pdir;
}

TEST(r104_wtrunner_config, active_trader_mocker_initializes_and_is_registered)
{
	RunnerTraderFixture fixtureEnv("r104_trader_success");
	ASSERT_TRUE(fixtureEnv.ready()) << fixtureEnv.error();
	WtRunner& runner = fixtureEnv.runner();

	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "traders:\n" + one_trader_block("r104wt_trader_ok", 104101);
	write_fixture("trader_success.yaml", cfg);

	ASSERT_TRUE(runner.config(fixture("trader_success.yaml")))
		<< "TraderMocker artifact: " << fixtureEnv.module_source().string();
	EXPECT_EQ(static_cast<size_t>(1), runner._traders.getAdapters().size());
	EXPECT_NE(nullptr, runner._traders.getAdapter("r104wt_trader_ok"));
	EXPECT_TRUE(runner_fs::exists(runner_fs::path(fixtureEnv.temp_path()) / "mocker_104101"));
	// This config/init positive control deliberately does not call run/connect.
}

TEST(r104_wtrunner_config, active_trader_missing_module_fails_without_registration)
{
	RunnerTraderFixture fixtureEnv("r104_trader_missing");
	ASSERT_TRUE(fixtureEnv.ready()) << fixtureEnv.error();
	WtRunner& runner = fixtureEnv.runner();

	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "traders:\n" + one_trader_block("r104wt_trader_missing", 104102, "R104MissingTrader");
	write_fixture("trader_missing.yaml", cfg);

	EXPECT_FALSE(runner.config(fixture("trader_missing.yaml")));
	EXPECT_TRUE(runner._traders.getAdapters().empty());
	EXPECT_EQ(nullptr, runner._traders.getAdapter("r104wt_trader_missing"));
	// Failed configuration must not enter run()'s event loop.
	runner.run(false);
}

TEST(r104_wtrunner_config, later_trader_failure_removes_prior_adapter_from_registry)
{
	RunnerTraderFixture fixtureEnv("r104_trader_rollback");
	ASSERT_TRUE(fixtureEnv.ready()) << fixtureEnv.error();
	WtRunner& runner = fixtureEnv.runner();

	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "traders:\n"
		+ one_trader_block("r104wt_trader_first", 104103)
		+ one_trader_block("r104wt_trader_second", 104104, "R104MissingTrader");
	write_fixture("trader_rollback.yaml", cfg);

	EXPECT_FALSE(runner.config(fixture("trader_rollback.yaml")));
	// TraderMocker creates this directory during init, proving item 1 initialized
	// before item 2 failed. The manager's public registry view proves item 1 was
	// removed. There is no release spy here, so this does not independently prove
	// how many times the plugin release callback ran.
	EXPECT_TRUE(runner_fs::exists(runner_fs::path(fixtureEnv.temp_path()) / "mocker_104103"));
	EXPECT_TRUE(runner._traders.getAdapters().empty());
	EXPECT_EQ(nullptr, runner._traders.getAdapter("r104wt_trader_first"));
	EXPECT_EQ(nullptr, runner._traders.getAdapter("r104wt_trader_second"));
	runner.run(false);
}

TEST(r104_wtrunner_config, executer_name_defaults_to_local_and_missing_trader_paths)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture("logcfg.yaml"));

	/*
	 * initExecuters():476~479 name 缺省 -> "local"；:481~491 无 trader 键 ->
	 *   :488 真 -> :490 error -> :506 addExecuter；
	 * :492~504 有 trader 键但 adapter 不存在 -> :494 非空指针、getAdapter 得空 -> :502 error；
	 * :508~517 diff 无 trader -> :515 真 -> :517 error -> :533 addExecuter；
	 * :535~541 dist 兜底支。
	 */
	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "executers:\n"
		+ "-   id: r104wt_e_default\n    active: true\n    scale: 1\n"
		+ "-   id: r104wt_e_local_absent\n    active: true\n    name: local\n    trader: r104wt_absent\n"
		+ "-   id: r104wt_e_diff2\n    active: true\n    name: diff\n"
		+ "-   id: r104wt_e_dist2\n    active: true\n    name: dist\n";
	write_fixture("default_exec.yaml", cfg);

	EXPECT_TRUE(runner->config(fixture("default_exec.yaml")));
}

TEST(r104_wtrunner_config, executers_side_file_string_branch_reaches_init_executers)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture("logcfg.yaml"));

	/*
	 * config():231~247 executers 是字符串 -> StdFile::exists 真 -> 二次 load_from_file
	 *   -> :245 initExecuters(侧文件里的 executers 数组)
	 *   -> WtRunner.cpp:470~491（侧文件条目没有 name/trader -> local 缺省 + 空 trader 错误支）。
	 */
	write_fixture("executers_side.yaml",
		"executers:\n"
		"-   id: r104wt_e_side\n    active: true\n");
	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "executers: " + fixture("executers_side.yaml") + "\n";
	write_fixture("exec_side_cfg.yaml", cfg);

	EXPECT_TRUE(runner->config(fixture("exec_side_cfg.yaml")));
}

/* =========================================================================
 * 二、DISABLED_：起线程的 run()
 * ====================================================================== */

TEST(r104_wtrunner_config, DISABLED_run_starts_parsers_traders_engine_and_polls_forever)
{
	/*
	 * K4（R28 已实测）：run() 的 :582 _parsers.run() / :583 _traders.run() /
	 * :585 _engine->run() 会起接收线程与 CTA 定时器线程，:591~596 install_signal_hooks
	 * 改全局信号表，:599~603 while(!_to_exit) 每 10ms 轮询永不返回。
	 * 即便 bAsync=true 跳过 :598~604，_cta_engine.run() 也要 env.product.session 且
	 * 会留下 WtCtaRtTicker 线程，属于「必须起线程」的留白面。
	 */
	SUCCEED() << "run() intentionally left disabled";
}
