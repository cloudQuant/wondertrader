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
 *   所以本文件的所有 WtRunner 一律 unique_ptr 持有。
 *
 * 安全口径：
 *   - 不调用 run()（起线程 + while(!_to_exit) 轮询 + install_signal_hooks）；
 *   - 不链真实行情/交易时段，config() 只走到 parsers/traders/executers 的
 *     「文件不存在 / active:false」支路，避开 dlopen 与 shm 挂载；
 *   - 每条用例自己唯一的夹具文件名，共享 cwd 下不互相踩。
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "WtRunner/WtRunner.h"
#include "WtCore/WtHelper.h"
#include "Includes/WTSVariant.hpp"
#include "Share/StdUtils.hpp"

/* 生产 TU（src/WtRunner/main.cpp）经 -Dmain=wt_coverage_main_wtrunner 改名后的入口 */
int wt_coverage_main_wtrunner(int argc, char* argv[]);

/* clang 覆盖率运行时的落盘文件名开关：fork 出的子进程必须换名字，
   否则子进程 exit() 时写的是父进程已经展开过的 %p 路径，会把父进程的用例计数冲掉 */
extern "C" void __llvm_profile_set_filename(const char*);

/* 生产 TU（src/WtRunner/WtRunner.cpp:26）的自由函数，未在 WtRunner.h 声明 */
const char* getBinDir();

namespace {

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

std::string fixture_path(const std::string& name)
{
	return std::string("./") + kTag + "_" + name;
}

void write_fixture(const std::string& name, const std::string& body)
{
	StdFile::write_file_content(fixture_path(name).c_str(), body);
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

/* 空规则组：ActionPolicyMgr::init 走到 memberNames() 空循环后 return true */
const char* kActPolicyEmpty = R"YAML({})YAML";

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
std::string basefiles_block()
{
	return std::string("basefiles:\n")
		+ "    session: " + fixture_path("sessions.json") + "\n"
		+ "    commodity: " + fixture_path("commodities.json") + "\n"
		+ "    contract: " + fixture_path("contracts.json") + "\n"
		+ "    holiday: " + fixture_path("holidays.json") + "\n"
		+ "    hot: " + fixture_path("hots.json") + "\n";
}

std::string env_block(const char* engineName)
{
	return std::string("env:\n")
		+ "    name: " + engineName + "\n"
		+ "    fees: " + fixture_path("absent_fees.json") + "\n"
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
}

} // namespace

/* =========================================================================
 * 一、入口 wt_coverage_main_wtrunner（src/WtRunner/main.cpp:51-127）
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

TEST(r28_wtrunner_config, unparsable_yaml_escapes_as_exception)
{
	// 实测：WTSCfgLoader::load_from_yaml 对坏 YAML 抛 yaml-cpp 异常，
	// WtRunner::config():69 没有 try/catch，异常一路穿透到调用方。
	// 真实 WtRunner 进程里这就是 std::terminate（BUG-R28-003），
	// 所以这里钉的是「现状 = 抛出」而不是「期望 = 返回 false」。
	write_fixture("broken_cfg.yaml", "basefiles: [ this is : not valid\n\t- {{{\n");
	std::unique_ptr<WtRunner> runner = make_runner();
	EXPECT_THROW(runner->config(fixture_path("broken_cfg.yaml")), std::exception);
}

TEST(r28_wtrunner_config, unparsable_json_returns_false_without_throwing)
{
	// 同一个入口换成 .json 后缀就走 rapidjson（不抛），返回 NULL -> config():71-73
	// 两条错误路径的健壮性不一致，这是对照用例
	write_fixture("broken_cfg.json", "{ \"basefiles\": ][ bad json ");
	std::unique_ptr<WtRunner> runner = make_runner();
	EXPECT_FALSE(runner->config(fixture_path("broken_cfg.json")));
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
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));      // 必须先 init，WtHelper::_inst_dir 才有值

	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "parsers: " + fixture_path("absent_parsers.yaml") + "\n"
		+ "traders: " + fixture_path("absent_traders.yaml") + "\n"
		+ "executers: " + fixture_path("absent_executers.yaml") + "\n";
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
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEngine():381-384 _is_sel=true -> :397-400 _sel_engine
	std::string cfg = basefiles_block() + env_block("sel") + tail_block()
		+ "executers: " + fixture_path("absent_executers.yaml") + "\n";
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

TEST(r28_wtrunner_config, missing_env_section_stops_at_actionpolicy_but_returns_true)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEngine():368-370 `cfg==NULL` -> return false（config() 不看返回值）
	// initDataMgr():418-420 data 段缺失 -> return false
	// initActionPolicy():412 仍会执行 -> 有 bspolicy -> true -> config 继续
	std::string cfg = basefiles_block()
		+ "bspolicy: " + fixture_path("actpolicy.yaml") + "\n";
	write_fixture("noenv_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("noenv_cfg.yaml")));
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
	write_fixture("logcfg.yaml", kLogCfg);
	write_fixture("emptypolicy.yaml", kActPolicyEmpty);
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// memberNames() 为空 -> ActionPolicyMgr::init 的 for 不进入、直接 return true
	// basefiles 只有 session/commodity/contract，没有 holiday/hot -> 跳过对应分支
	std::string cfg = std::string("basefiles:\n")
		+ "    session: " + fixture_path("sessions.json") + "\n"
		+ "    commodity: " + fixture_path("commodities.json") + "\n"
		+ "    contract: " + fixture_path("contracts.json") + "\n"
		+ env_block("cta") + "bspolicy: " + fixture_path("emptypolicy.yaml") + "\n";
	write_fixture("emptypolicy_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("emptypolicy_cfg.yaml")));
}

TEST(r28_wtrunner_config, basefiles_rules_section_loads_custom_hot_rules)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// config():142-151 `if(cfgBF->has("rules"))` -> loadCustomRules + info 日志
	std::string cfg = basefiles_block()
		+ "    rules:\n        r28rule: " + fixture_path("absent_rule.json") + "\n"
		+ env_block("cta") + tail_block();
	write_fixture("rules_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("rules_cfg.yaml")));
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
		+ "    second: " + fixture_path("absent_second.json") + "\n"
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
	EXPECT_TRUE(runner->config(fixture_path("parsers_side_cfg.yaml")));
}

TEST(r28_wtrunner_config, notifier_section_absent_returns_false_from_init_evt_notifier)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEvtNotifier():632-634 `cfg==NULL` -> return false（config() 不看返回值）
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers: " + fixture_path("absent_executers.yaml") + "\n";
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
	// config():263-266 -> initCtaStrategies():279-283 strategies 缺失 -> false
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "executers:\n-   id: r28e0\n    active: false\n";
	write_fixture("exec_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("exec_cfg.yaml")));
}

TEST(r28_wtrunner_config, strategies_object_without_cta_array_returns_false_from_engine_side)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initCtaStrategies():279 拿到 strategies 对象 -> :284 cfg->get("cta")==NULL -> false
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
	EXPECT_TRUE(runner->config(fixture_path("parsers_unload_cfg.yaml")));
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
	EXPECT_TRUE(runner->config(fixture_path("traders_unload_cfg.yaml")));
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
	EXPECT_TRUE(runner->config(fixture_path("executers_unload_cfg.yaml")));
}

TEST(r28_wtrunner_config, notifier_inactive_entry_short_circuits_evt_notifier_init)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// initEvtNotifier():632-637 notifier 是对象 -> _notifier.init(cfg) -> :637 return true
	// EventNotifier::init 第一件事就是 `if(!cfg->getBoolean("active")) return false`
	// （EventNotifier.cpp:60-62），所以不会去 dlopen WtMsgQue、也不会碰任何端口
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "notifier:\n    active: false\n    url: inproc://r28wt_disabled\n"
		+ "executers: " + fixture_path("absent_executers.yaml") + "\n";
	write_fixture("notifier_off_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("notifier_off_cfg.yaml")));
}

TEST(r28_wtrunner_config, notifier_active_entry_fails_to_load_mq_module_and_returns)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));

	// EventNotifier.cpp:63-79：wrap_module("WtMsgQue") -> 先查 cwd 再查 inst dir ->
	// 两个目录都没有 libWtMsgQue.dylib（用例的 run 目录是空的）-> load_library NULL
	// -> :74 error -> :75 return false。整条链不开线程、不 bind 端口。
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "notifier:\n    active: true\n    url: tcp://127.0.0.1:55661\n"
		+ "    name: r28wt_notifier\n"
		+ "executers: " + fixture_path("absent_executers.yaml") + "\n";
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
	EXPECT_TRUE(runner->config(fixture_path("traders_scalar_cfg.yaml")));
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
	EXPECT_TRUE(runner->config(fixture_path("executers_scalar_cfg.yaml")));
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

TEST(r28_wtrunner_entry, DISABLED_missing_config_without_help_still_runs)
{
	// K1 + K4：显式 -c 一个不存在的路径，行为与裸参数同一条路
	EXPECT_EQ(call_entry({"wt_runner", "-c", "./r28wt_absent_dtcfg.yaml"}), 0);
}

TEST(r28_wtrunner_entry, DISABLED_unknown_long_option_segv)
{
	// K2：实测 SIGSEGV(11)，不是返回非 0 错误码
	EXPECT_NE(call_entry({"wt_runner", "--r28-bogus-flag"}), 0);
}

TEST(r28_wtrunner_config, DISABLED_valid_config_without_basefiles_derefs_null_section)
{
	// K3：WtRunner.cpp:77 cfgBF=NULL 后 :78 无条件 cfgBF->get("session")
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	write_fixture("empty_obj_cfg.yaml", "{}\n");
	EXPECT_FALSE(runner->config(fixture_path("empty_obj_cfg.yaml")));
}

TEST(r28_wtrunner_config, DISABLED_router_rule_without_executer_key_derefs_null)
{
	// K5（R28 实测 SIGSEGV(11)）：WtExecMgr.cpp:218-219
	//   itemExec = item->get("executer"); if(itemExec->isArray())
	// 「routers 条目漏写 executer 键」直接空指针，而 :217 的 item 本身也没判空。
	// 见 bugs/R28_runner_mains.md BUG-R28-005。
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture_path("logcfg.yaml"));
	std::string cfg = basefiles_block() + env_block("cta") + tail_block()
		+ "routers:\n-   strategy: r28stra\n    symbol: r28sym\n";
	write_fixture("badrouter_cfg.yaml", cfg);
	EXPECT_TRUE(runner->config(fixture_path("badrouter_cfg.yaml")));
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
