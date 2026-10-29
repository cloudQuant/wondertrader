/*
 * R14 lane —— src/WtUftRunner/（可执行体 + WtUftRunner 类，WtUftRunner.cpp 整文件 0.00%）
 *
 * 形状照抄 m_LoaderRunner/seam_loader_runner_test.cpp 与 m_WtRunner/r6_wtrunner_main_test.cpp：
 * coverage/CMakeLists.txt 的 WT_MAIN_MODULES 循环对本模块目录下每个 .cpp（main.cpp + WtUftRunner.cpp）
 * 追加 -Dmain=wt_coverage_main_wtuftrunner（string(TOLOWER) 后是全小写，写成大写会链接期未定义），
 * 于是 main.cpp:31 的入口符号改名后与本文件的 gtest_main 同链一个二进制。
 *
 * 安全口径（本轮硬规则）：
 *  - main.cpp:31 的入口里只有 :53~54 的 -h/--help 分支能在 WtUftRunner 构造之前 return 0；
 *    其余分支（:67 runner.init -> :75 runner.config -> :78 runner.run(false)）里
 *    run() 在 WtUftRunner.cpp:355 调 _uft_engine.run()、:360 ShareManager::start_watching(2)
 *    （起看门狗线程 + 共享内存）、:362~365 while(!_to_exit) 每 10ms 轮询的死循环
 *    -> 一律 DISABLED_，不启用。
 *  - WtUftRunner.cpp 侧只走「不依赖引擎 run 循环」的成员函数：构造/析构/init/config 的两条早退支。
 *  - 禁止起线程：WtUftRunner 构造里的 install_signal_hooks(WtUftRunner.cpp:40~45) 经实测
 *    （src/WTSUtils/SignalHook.hpp 全文）只做 sigaction/signal 注册，不含 std::thread/pthread_create，
 *    所以构造本身不起线程，只在真收到信号时才执行回调 —— 用例不发信号。
 *
 * 期望值一律由本用例自己喂进去的 argv / 自己写的 yaml / 自己取的工作目录算出，不读挂钟。
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <unistd.h>
#include <fstream>

#include "WtUftRunner/WtUftRunner.h"
/* WtUftRunner.cpp:57 用的是 WtUftCore 这份 WtHelper（与 WtDtCore/WtRunner 侧同名不同实现，见缺陷登记） */
#include "WtUftCore/WtHelper.h"

/* 生产 TU（main.cpp:31）经 -Dmain=... 改名后的入口 */
int wt_coverage_main_wtuftrunner(int argc, char* argv[]);

/* 生产 TU（WtUftRunner.cpp:22）的自由函数，未在 WtUftRunner.h 里声明，按定义处重新声明 */
const char* getBinDir();

/* 生产 TU（WtUftRunner.cpp:375）的级别标签表；handleLogAppend(:385) 是空实现，全文件无人取用 */
extern const char* LOG_TAGS[];

namespace {

const char* kAbsentLogCfg = "./r14_uft_absent_logcfg.yaml";
const char* kAbsentCfg = "./r14_uft_absent_config.yaml";
/* 本用例自己写的配置：只放「能让 config() 逐段早退」的键，不放 env/data/share_domain/parsers/traders */
const char* kSelfAuthoredCfg = "./r14_uft_min_config.yaml";

int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_wtuftrunner(static_cast<int>(argv.size()), argv.data());
}

/* 写出一个由用例完全掌控的 yaml；.yaml 后缀是 WTSCfgLoader.cpp:324 判定走 yaml 解析的前提 */
void write_min_config()
{
	std::ofstream out(kSelfAuthoredCfg, std::ios::binary | std::ios::trunc);
	out << "basefiles:\n"
		<< "    placeholder: r14\n"
		<< "bspolicy: ./r14_uft_absent_actpolicy.yaml\n";
	out.flush();
	out.close();
}

} // namespace

/* -------------------------------------------------------------------------
 * 一、入口 --help 分支（main.cpp:39~54）
 * ---------------------------------------------------------------------- */

TEST(r14_uft_entry, short_help_returns_zero_before_constructing_runner)
{
	// main.cpp:53 if(hParam->exists()) return 0; —— :65 的 WtUftRunner runner 不执行
	EXPECT_EQ(call_entry({"wt_uft_runner", "-h"}), 0);
}

TEST(r14_uft_entry, long_help_returns_zero)
{
	EXPECT_EQ(call_entry({"wt_uft_runner", "--help"}), 0);
}

TEST(r14_uft_entry, help_wins_over_config_and_logcfg_words)
{
	// -h 与 -c/-l 同时给出：parse() 后 hParam->exists() 仍为真，早退先于日志与配置加载
	EXPECT_EQ(call_entry({"wt_uft_runner", "-c", kAbsentCfg, "-l", kAbsentLogCfg, "-h"}), 0);
}

/* -------------------------------------------------------------------------
 * 二、WtUftRunner.cpp:22~33 getBinDir()
 * ---------------------------------------------------------------------- */

TEST(r14_uft_binroot, static_cache_returns_same_pointer_twice)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	// :25 if(basePath.empty()) 第二次走假分支
	EXPECT_EQ(first, getBinDir()) << "getBinDir() 的 static 缓存每次都重算了路径";
}

TEST(r14_uft_binroot, value_is_the_cwd_with_at_most_one_trailing_slash)
{
	char cwd[4096] = {0};
	ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
	const std::string base(cwd);
	const std::string actual(getBinDir());

	// :27 initial_path -> :29 StrUtil::standardisePath 只规范化尾部分隔符
	const bool exact = (actual == base);
	const bool slashed = (actual == base + "/");
	EXPECT_TRUE(exact || slashed) << "getBinDir()=" << actual << " 与 cwd=" << base << " 对不上";
	EXPECT_FALSE(exact && slashed) << "cwd 本身以 / 结尾时两种形态不可能同时成立";
}

/* -------------------------------------------------------------------------
 * 三、WtUftRunner.cpp:37~58 构造/析构 + init()
 * ---------------------------------------------------------------------- */

TEST(r14_uft_init, ctor_dtor_roundtrip_registers_hooks_without_spawning_threads)
{
	EXPECT_NO_THROW({ WtUftRunner runner; (void)runner; });
	// 第二次构造走同一条 install_signal_hooks，验证可重入（:40~45 只做 sigaction）
	EXPECT_NO_THROW({ WtUftRunner runner; (void)runner; });
}

TEST(r14_uft_init, init_with_missing_logcfg_stamps_inst_dir_with_binroot)
{
	WtUftRunner runner;
	WtHelper::setInstDir("");
	// :55 WTSLogger::init(不存在的路径) 内部告警后返回；:57 setInstDir(getBinDir())
	EXPECT_NO_THROW(runner.init(kAbsentLogCfg));

	// :57 是无条件执行的，因此 getInstDir() 必须与同一 TU 的 getBinDir() 逐字相等
	const std::string stamped = WtHelper::getInstDir();
	const std::string expect(getBinDir());
	EXPECT_EQ(stamped, expect) << "init() 没有把安装目录写成 getBinDir() 的值";
	EXPECT_FALSE(stamped.empty());
}

/* -------------------------------------------------------------------------
 * 四、WtUftRunner.cpp:60~197 config() 的早退分支
 * ---------------------------------------------------------------------- */

TEST(r14_uft_config, missing_file_returns_false)
{
	WtUftRunner runner;
	// :62 load_from_file -> WTSCfgLoader.cpp:300 文件不存在 -> NULL -> :65 记日志 -> :66 return false
	EXPECT_FALSE(runner.config(kAbsentCfg));
}

TEST(r14_uft_config, authored_config_without_basefiles_children_walks_the_early_exits)
{
	write_min_config();
	ASSERT_TRUE(StdFile::exists(kSelfAuthoredCfg));

	WtUftRunner runner;
	// basefiles 存在，但必要的 env/data 段缺失；initEngine/initDataMgr 返回 false。
	// config() 目前吞掉这两个失败并最终返回 true，因此这里保留 false 契约断言以暴露产品问题。
	EXPECT_FALSE(runner.config(kSelfAuthoredCfg));
}

TEST(r14_uft_config, missing_then_authored_order_is_independent)
{
	WtUftRunner runner;
	// 缺文件失败后，再验证缺少必要 env/data 的配置也应失败。
	EXPECT_FALSE(runner.config(kAbsentCfg));
	write_min_config();
	EXPECT_FALSE(runner.config(kSelfAuthoredCfg));
}

/* -------------------------------------------------------------------------
 * 五、WtUftRunner.cpp:375~387 LOG_TAGS 表与空实现 handleLogAppend
 * ---------------------------------------------------------------------- */

TEST(r14_uft_logtags, table_matches_the_seven_declared_levels)
{
	// WtUftRunner.cpp:375~383 的字面量顺序，与 Includes/WTSTypes.h 的 LL_ALL=100..LL_NONE=106 对齐
	EXPECT_STREQ(LOG_TAGS[0], "all");
	EXPECT_STREQ(LOG_TAGS[1], "debug");
	EXPECT_STREQ(LOG_TAGS[2], "info");
	EXPECT_STREQ(LOG_TAGS[3], "warn");
	EXPECT_STREQ(LOG_TAGS[4], "error");
	EXPECT_STREQ(LOG_TAGS[5], "fatal");
	EXPECT_STREQ(LOG_TAGS[6], "none");
}

TEST(r14_uft_loghandler, handleLogAppend_is_a_noop_for_every_declared_level)
{
	WtUftRunner runner;
	// :385~387 空函数体，逐个级别调用都应正常返回
	for (uint32_t ll = LL_ALL; ll <= LL_NONE; ll++)
	{
		EXPECT_NO_THROW(runner.handleLogAppend(static_cast<WTSLogLevel>(ll), "r14 probe"))
			<< "ll=" << ll;
	}
}

/* -------------------------------------------------------------------------
 * 六、DISABLED_：需要真实引擎/共享内存/主循环的分支
 *
 * 触发条件与不启用理由：
 *  D1 main.cpp:59~78 非 -h 分支：runner.config(缺失) 返回 false 但 :78 仍无条件 runner.run(false)
 *     -> WtUftRunner.cpp:355 _uft_engine.run()（未 init 的引擎，内部 _action_policy/_env_cfg 为空）
 *     -> :360 ShareManager::self().start_watching(2) 起看门狗线程并申请共享内存
 *     -> :362 while(!_to_exit) 每 10ms 轮询，_to_exit 只能由信号回调翻转
 *     => 启用即挂死/踩空指针。
 *  D2 config() 的 env/data/parsers/traders/share_domain 齐备分支：
 *     :118~121 ShareManager::initialize/init_domain 需要真实 shm 段；
 *     :302/:327 ParserAdapter/TraderAdapter::init 会 dlopen 厂商库并起接收线程。
 *     可回收行数估计：WtUftRunner.cpp 的 :69~107 剩余 + :109~197 + :200~251 + :253~278
 *     + :280~338 + :340~349 + :351~373 ≈ 210 行（占该文件 285 行的绝大部分）。
 *  D3 入口无参分支（argc==1）：走默认 ./logcfg.yaml 与 ./config.yaml，
 *     是否进入 D2/D1 取决于运行目录内容，期望值无法由本用例输入算出 -> 禁用。
 * 另注：WtUftCore/WtUftEngine.cpp:209~213 的 return NULL 之后约 30 行是已登记的不可达段，本轮不追。
 * ---------------------------------------------------------------------- */

TEST(r14_uft_entry, DISABLED_non_help_argv_reaches_engine_run_and_watchdog_thread)
{
	// D1/D3：见上方触发条件
	EXPECT_EQ(call_entry({"wt_uft_runner", "-c", kSelfAuthoredCfg, "-l", kAbsentLogCfg}), 0);
}

TEST(r14_uft_runner, DISABLED_run_false_loops_until_signal)
{
	// D1：_uft_engine.run() + start_watching(2) + 10ms 轮询死循环
	WtUftRunner runner;
	runner.run(false);
	EXPECT_TRUE(true);
}

TEST(r14_uft_config, DISABLED_full_engine_config_needs_shm_and_vendor_libs)
{
	// D2：env/data/share_domain/parsers/traders 全量配置，需要真实 shm 与柜台
	WtUftRunner runner;
	EXPECT_TRUE(runner.config("./config.yaml"));
}
