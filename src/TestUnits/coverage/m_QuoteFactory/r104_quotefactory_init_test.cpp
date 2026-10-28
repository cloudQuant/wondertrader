/*
 * R104 lane —— src/QuoteFactory/main.cpp 的 initialize()/initParsers()/initDataMgr()
 *
 * 与 r14 的分工：
 *   r14 只钉住「配置缺失早退」那一段（:96~105）和入口的 help/缺文件分支；
 *   本文件补的是 :64~262 的**成功路径**：一份自带 basefiles + allday:true + 假 writer
 *   模块的配置可以让 initialize() 一路走到 :257 的 `if(!bAlldayMode)` 假分支后正常返回，
 *   既不挂 shm/broadcaster（配置里没有这两段），也不进非全天模式下的
 *   g_stateMon.run()（:260 起线程 + :259 的 5ms sleep）。
 *
 * 为什么用 fork 子进程跑 initialize()：
 *   1) 子进程里的全局 g_baseDataMgr/g_hotMgr/g_dataMgr/g_parsers 是进程私有的，
 *      一条用例跑脏了不会影响同二进制里的其它用例；
 *   2) 万一某段路径在评审环境里崩掉，gtest 宿主仍然能写出自己的 profraw；
 *   3) 子进程在 initialize() 返回后**显式**调 __llvm_profile_write_file() 再 _exit，
 *      不依赖 atexit —— _exit 本身不会触发 llvm 的退出写盘，这一步不能省。
 *   子进程名下的计数文件由 case_runner.sh 的 LLVM_PROFILE_FILE 里的 %p 自动分开。
 *
 * 安全口径：所有配置里的 module 名都刻意取不可能存在的插件名，DLLHelper::load_library
 * 失败后原地 return（DataManager.cpp:79-95 / ParserAdapter.cpp:110-118），不起线程、
 * 不 dlopen 真实厂商库、不碰网络和共享内存。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Includes/WTSVariant.hpp"
#include "Share/StdUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WtDtCore/ParserAdapter.h"
#include "WtDtCore/WtHelper.h"

using namespace wtp;

/* 生产 TU（QuoteFactory/main.cpp:50）的自由函数，未在头文件里声明 */
const char* getBinDir();
/* main.cpp:70 */
void initParsers(WTSVariant* cfg);
/* main.cpp:96 */
void initialize(const std::string& filename);
/* main.cpp:27 —— 生产 TU 里的全局适配器表，用来给 initParsers() 的落表结果做可证伪断言 */
extern ParserAdapterMgr g_parsers;

/* clang 覆盖率运行时：显式落盘（子进程用 _exit 收尾时唯一可靠的写盘方式） */
extern "C" int __llvm_profile_write_file(void);

namespace {

struct ChildOutcome
{
	bool exited = false;
	int code = -1;
	int signal = -1;
	std::string out;
};

std::string fixture(const std::string& name)
{
	return std::string("./r104qf_") + name;
}

void write_fixture(const std::string& name, const std::string& body)
{
	StdFile::write_file_content(fixture(name).c_str(), body);
}

/*
 * 在子进程里调 initialize()，父进程只做判决。
 * 子进程 stdout 接管道：initialize() 正常返回才会写 "RETURNED module_dir=<值>"。
 */
ChildOutcome initialize_in_child(const std::string& cfg)
{
	ChildOutcome r;
	int fds[2] = { -1, -1 };
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
		dup2(fds[1], STDOUT_FILENO);
		const int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0)
		{
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		close(fds[0]);
		close(fds[1]);

		initialize(cfg);

		/* 执行到这里 = initialize() 已经返回（没有停在 :254/:260 的线程上） */
		const std::string line = std::string("RETURNED module_dir=")
			+ WtHelper::get_module_dir() + "\n";
		const ssize_t ignored = write(STDOUT_FILENO, line.c_str(), line.size());
		(void)ignored;

		(void)__llvm_profile_write_file();
		_exit(0);
	}

	close(fds[1]);
	char buf[512];
	ssize_t got = 0;
	while ((got = read(fds[0], buf, sizeof(buf))) > 0)
		r.out.append(buf, static_cast<size_t>(got));
	close(fds[0]);

	int raw = 0;
	const pid_t done = waitpid(pid, &raw, 0);
	(void)done;
	r.exited = WIFEXITED(raw) != 0;
	r.code = r.exited ? WEXITSTATUS(raw) : -1;
	r.signal = WIFSIGNALED(raw) ? WTERMSIG(raw) : -1;
	return r;
}

/* ---------------- 基础数据夹具（形状与 manifests/R28 里验证过的一致） ---------------- */

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

/* HOT/2ND/rules 三处共用同一形状：exchg -> product -> 换月条目数组 */
const char* kHotRules = R"JSON({
 "SSE":{"r104p":[{"date":20240102,"from":"","to":"r104p2412"}]}
})JSON";

/*
 * 全量配置：每个分支都取「真」一侧。
 *   :108~113 session      :115~121 commodity(字符串)
 *   :131~137 contract     :147~151 holiday      :152~156 hot
 *   :158~162 second       :164~173 rules         :190~198 allday
 *   :199 writer(假模块)    :201~219 index(文件缺失)
 *   :221~250 parsers(数组，含 active:false 与 active:true 空 id)
 */
const char* kFullCfg = R"YAML(basefiles:
    session: ./r104qf_sessions.json
    commodity: ./r104qf_commodities.json
    contract: ./r104qf_contracts.json
    holiday: ./r104qf_holidays.json
    hot: ./r104qf_hot.json
    second: ./r104qf_second.json
    rules:
        R104: ./r104qf_rules.json
allday: true
writer:
    module: R104NoSuchWriterModule
index: ./r104qf_absent_index.yaml
parsers:
-   id: r104p_off
    active: false
    module: R104NoSuchParserModule
-   id: ""
    active: true
    module: R104NoSuchParserModule
)YAML";

/* commodity/contract 走数组分支（:122~128、:138~144），parsers 走「侧文件可加载」（:224~235） */
const char* kArrayCfg = R"YAML(basefiles:
    session: ./r104qf_sessions.json
    commodity:
    - ./r104qf_commodities.json
    contract:
    - ./r104qf_contracts.json
    holiday: ./r104qf_holidays.json
    hot: ./r104qf_hot.json
allday: true
writer:
    module: R104NoSuchWriterModule
parsers: ./r104qf_side_parsers.yaml
)YAML";

const char* kSideParsers = R"YAML(parsers:
-   id: r104p_side
    active: false
    module: R104NoSuchParserModule
)YAML";

/* 最小配置：basefiles 是空对象 -> :109/:115/:131/:147/:152/:158/:164 的「假」侧，
   parsers 是字符串但文件不存在 -> :241~243 */
const char* kMinimalCfg = R"YAML(basefiles: {}
allday: true
writer:
    module: R104NoSuchWriterModule
parsers: ./r104qf_absent_parsers.yaml
)YAML";

/* parsers 侧文件存在但后缀不在 load_from_file 白名单 -> :236~239 */
const char* kUnloadableParsers = R"YAML(parsers: []
)YAML";

} // namespace

/* =========================================================================
 * 一、initialize() 的成功路径
 * ====================================================================== */

TEST(r104_quote_initialize, full_allday_config_reaches_end_of_function)
{
	write_fixture("sessions.json", kSessions);
	write_fixture("commodities.json", kCommodities);
	write_fixture("contracts.json", kContracts);
	write_fixture("holidays.json", kHolidays);
	write_fixture("hot.json", kHotRules);
	write_fixture("second.json", kHotRules);
	write_fixture("rules.json", kHotRules);
	write_fixture("full.yaml", kFullCfg);

	/* 先在本进程取一次缓存值，子进程继承同一份 static（:53 的假分支） */
	const std::string expected_module_dir = getBinDir();

	/*
	 * 覆盖 main.cpp:98~262 的主干：
	 *  :98 set_module_dir -> :100 load -> :108~173 五类基础数据 + rules
	 *  -> :190 allday 真 -> :195~198 日志 -> :199 initDataMgr(writer)
	 *  -> :201~204 index 工厂目录缺失 -> :207~209/:215~217 index 配置加载失败
	 *  -> :221~250 parsers 数组（:72~76 continue、:78~91 active 且自动 id）
	 *  -> :252 release -> :254 g_parsers.run() -> :257 假分支 -> :262 返回
	 */
	ChildOutcome r = initialize_in_child(fixture("full.yaml"));
	ASSERT_TRUE(r.exited) << "initialize() 被子进程信号 " << r.signal << " 打断";
	EXPECT_EQ(r.code, 0);
	// 可证伪观测点：只有 initialize() 真的跑到 :98 并返回，这一行才会出现
	EXPECT_NE(r.out.find("RETURNED module_dir=" + expected_module_dir), std::string::npos)
		<< "child stdout=" << r.out;
}

TEST(r104_quote_initialize, array_basefiles_and_loadable_side_parser_file)
{
	write_fixture("sessions.json", kSessions);
	write_fixture("commodities.json", kCommodities);
	write_fixture("contracts.json", kContracts);
	write_fixture("holidays.json", kHolidays);
	write_fixture("hot.json", kHotRules);
	write_fixture("side_parsers.yaml", kSideParsers);
	write_fixture("array.yaml", kArrayCfg);

	/*
	 * :122~128 commodity 数组、:138~144 contract 数组；
	 * :224~235 parsers 是字符串且文件存在 -> 二次 load_from_file -> initParsers(侧文件数组)
	 * -> :72~76 active:false continue -> :93 "{} market data parsers loaded in total"
	 */
	ChildOutcome r = initialize_in_child(fixture("array.yaml"));
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0);
	EXPECT_NE(r.out.find("RETURNED module_dir="), std::string::npos) << r.out;
}

TEST(r104_quote_initialize, minimal_config_skips_absent_basefiles_children)
{
	write_fixture("minimal.yaml", kMinimalCfg);

	/*
	 * basefiles 里一个子键都没有 -> :109/:115/:131/:147/:152/:158/:164 的假侧；
	 * parsers 字符串指向不存在的文件 -> :226 假 -> :241~243 error；
	 * 其余与全量配置同一条尾段（:252/:254/:257）。
	 */
	ChildOutcome r = initialize_in_child(fixture("minimal.yaml"));
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0);
	EXPECT_NE(r.out.find("RETURNED module_dir="), std::string::npos) << r.out;
}

TEST(r104_quote_initialize, unloadable_parser_side_file_logs_and_returns)
{
	write_fixture("parsers_unloadable.txt", kUnloadableParsers);

	std::string cfg = kMinimalCfg;
	// 把最小配置里的 parsers 指向存在但后缀不在白名单的侧文件 -> :227 真 / :230 得 NULL
	// -> :236~239 "Loading parser config {} failed"
	const std::string oldLine = "parsers: ./r104qf_absent_parsers.yaml";
	const std::string newLine = "parsers: ./r104qf_parsers_unloadable.txt";
	const std::string::size_type pos = cfg.find(oldLine);
	ASSERT_NE(pos, std::string::npos);
	cfg.replace(pos, oldLine.size(), newLine);
	write_fixture("unloadable.yaml", cfg);

	ChildOutcome r = initialize_in_child(fixture("unloadable.yaml"));
	ASSERT_TRUE(r.exited) << "signal=" << r.signal;
	EXPECT_EQ(r.code, 0);
	EXPECT_NE(r.out.find("RETURNED module_dir="), std::string::npos) << r.out;
}

/* =========================================================================
 * 二、initParsers()（main.cpp:70~94）直接驱动
 * ====================================================================== */

TEST(r104_quote_initparsers, active_entries_with_and_without_id_land_in_manager)
{
	const char* body = R"YAML(parsers:
-   id: r104p_direct
    active: true
    module: R104NoSuchParserModule
-   id: ""
    active: true
    module: R104NoSuchParserModule
-   id: r104p_inactive
    active: false
    module: R104NoSuchParserModule
)YAML";
	write_fixture("direct_parsers.yaml", body);

	WTSVariant* root = WTSCfgLoader::load_from_file(fixture("direct_parsers.yaml").c_str());
	ASSERT_NE(root, nullptr);
	WTSVariant* parsers = root->get("parsers");
	ASSERT_NE(parsers, nullptr);
	ASSERT_EQ(parsers->size(), 3u);

	const uint32_t before = g_parsers.size();
	/*
	 * :71~94 全长：:72 循环、:74~76 active:false continue、
	 * :78~90 两条 active（第二条 id 为空 -> :81~86 自动 id）、:93 汇总日志
	 */
	initParsers(parsers);

	EXPECT_EQ(g_parsers.size(), before + 2u);
	EXPECT_NE(g_parsers.getAdapter("r104p_direct"), nullptr);
	EXPECT_EQ(g_parsers.getAdapter("r104p_inactive"), nullptr);

	root->release();
}

/* =========================================================================
 * 三、DISABLED_：确认会崩 / 会挂的分支
 * ====================================================================== */

TEST(r104_quote_initialize, DISABLED_config_without_basefiles_derefs_null)
{
	/*
	 * D1 main.cpp:108 `cfgBF = config->get("basefiles")` 没有判空，
	 *    :109 直接 cfgBF->get("session") -> 空指针（同一份代码在 WtRunner.cpp:77~78 也是）。
	 *    同族缺陷已登记为 BUG-R104-001；本迭代不改产品代码，这里只留记录。
	 */
	write_fixture("nobase.yaml",
		"allday: true\n"
		"writer:\n    module: R104NoSuchWriterModule\n");
	ChildOutcome r = initialize_in_child(fixture("nobase.yaml"));
	EXPECT_EQ(r.code, 0);
}

TEST(r104_quote_initialize, DISABLED_state_monitor_branch_starts_threads)
{
	/*
	 * D2 allday:false 时 :193 g_stateMon.initialize(...) 需要 statemonitor 配置，
	 *    随后 :259 sleep(5ms) + :260 g_stateMon.run() 起状态机线程；不允许起线程/墙钟等待。
	 *    改用例需要「状态机不落地线程」的接缝，启用前不得放进覆盖率主跑。
	 *    需要回收的行：:192~194 + :258~262（约 7 行）。
	 */
	SUCCEED() << "state monitor branch intentionally left disabled";
}
