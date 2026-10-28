/*
 * R14 lane —— src/QuoteFactory/main.cpp（可执行体，整文件 0.00%）
 *
 * 形状照抄 m_LoaderRunner/seam_loader_runner_test.cpp：
 * coverage/CMakeLists.txt 的 WT_MAIN_MODULES 循环对本模块目录下每个 .cpp 追加
 * -Dmain=wt_coverage_main_quotefactory（全小写，实测大写会在链接期报未定义符号），
 * 于是 main.cpp:264 的入口符号改名后与本文件的 gtest_main 同链一个二进制。
 *
 * 安全口径（本轮硬规则）：
 *  - 只调用能自行终止、不起线程、不阻塞的路径；
 *  - main.cpp 里可启用的分支只有：
 *      B1 :271~276  -h/--help -> hParam->exists() -> return 0（在 WTSLogger::init 之前）
 *      B2 :278~283  日志配置分支（缺文件 -> WTSLogger::init 内部告警后返回）
 *      B3 :299~308  -c 指向不存在的配置 -> StdFile::exists 为假 -> fmt::print + return 0
 *    B3 是刻意用「显式 -c 不存在路径」而不是「不给 -c」：不给时默认 ./dtcfg.yaml，
 *    一旦运行目录恰好有该文件（产品仓库的 dist/ 里就有），:310 initialize() 就会
 *    在 :254 g_parsers.run() 起解析线程、:260 g_stateMon.run() 起状态机线程，
 *    并且末尾 :324 while(!bExit) 是每 10ms 一轮的死循环 —— 启用用例里等于挂死，
 *    所以 :310~327 全部只登记为 DISABLED_。
 *  - 自由函数 getBinDir()（:50~61）不依赖任何运行期服务，可直接断言。
 *
 * 期望值一律由本用例自己喂进去的 argv / 自己取的工作目录算出，不读挂钟。
 */
#include <gtest/gtest.h>

#include <string>
#include <unistd.h>
#include <vector>

/* initialize() 里 :98 用的是 WtDtCore 这份 WtHelper（与 WtUftCore/WtCore 侧同名不同实现） */
#include "WtDtCore/WtHelper.h"

/* 生产 TU（QuoteFactory/main.cpp:264）经 -Dmain=... 改名后的入口 */
int wt_coverage_main_quotefactory(int argc, char* argv[]);

/* 生产 TU（QuoteFactory/main.cpp:50）的自由函数，未在头文件里声明，按定义处重新声明 */
const char* getBinDir();

/* 生产 TU（QuoteFactory/main.cpp:96）的自由函数：配置缺失时在 :104 return，不碰任何 caster */
void initialize(const std::string& filename);

namespace {

const char* kAbsentLogCfg = "./r14_qf_definitely_absent_logcfg.yaml";
const char* kAbsentDtCfg = "./r14_qf_definitely_absent_dtcfg.yaml";

/* 用受控 argv 调入口；argv[0] 只是程序名，cppcli 不校验它 */
int call_entry(std::vector<const char*> words)
{
	std::vector<char*> argv;
	argv.reserve(words.size());
	for (const char* w : words)
		argv.push_back(const_cast<char*>(w));
	return wt_coverage_main_quotefactory(static_cast<int>(argv.size()), argv.data());
}

} // namespace

/* -------------------------------------------------------------------------
 * 一、B1 —— help 分支（main.cpp:266~276）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_entry, short_help_returns_zero_before_any_logging_init)
{
	// main.cpp:275 if(hParam->exists()) return 0; —— 后面的 WTSLogger::init/initialize 都不执行
	EXPECT_EQ(call_entry({"quote_factory", "-h"}), 0);
}

TEST(r14_quote_entry, long_help_returns_zero)
{
	EXPECT_EQ(call_entry({"quote_factory", "--help"}), 0);
}

TEST(r14_quote_entry, help_wins_over_config_and_logcfg_words)
{
	// -h 与 -c/-l 同时给出：parse() 后 hParam->exists() 仍为真，早退先于任何文件访问
	EXPECT_EQ(call_entry({"quote_factory", "-c", kAbsentDtCfg, "-l", kAbsentLogCfg, "-h"}), 0);
}

/* -------------------------------------------------------------------------
 * 二、B2+B3 —— 日志初始化后配置缺失早退（main.cpp:278~308）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_entry, missing_config_path_returns_zero_via_explicit_c)
{
	// lParam 不存在 -> :282 filename="./logcfgdt.yaml"；cParam 存在 -> :300 取显式路径
	// :304 StdFile::exists(不存在的路径) 为假 -> :306 打印 -> :307 return 0
	int rc = call_entry({"quote_factory", "-c", kAbsentDtCfg});
	EXPECT_EQ(rc, 0);
}

TEST(r14_quote_entry, both_c_and_l_given_still_returns_zero_when_files_absent)
{
	// 两条分支都取「存在」一侧：:280 filename=lParam->get<std::string>()，:300 同理
	int rc = call_entry({"quote_factory", "-c", kAbsentDtCfg, "-l", kAbsentLogCfg});
	EXPECT_EQ(rc, 0);
}

/* -------------------------------------------------------------------------
 * 三、getBinDir()（main.cpp:50~61）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_binroot, static_cache_returns_same_pointer_twice)
{
	const char* first = getBinDir();
	ASSERT_NE(first, nullptr);
	const char* second = getBinDir();
	// :53 if(basePath.empty()) 第二次走假分支，返回同一块内存
	EXPECT_EQ(first, second) << "getBinDir() 的 static 缓存每次都重算了路径";
}

TEST(r14_quote_binroot, value_is_the_cwd_with_at_most_one_trailing_slash)
{
	char cwd[4096] = {0};
	ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
	const std::string base(cwd);
	const std::string actual(getBinDir());

	// :55 initial_path -> :57 StrUtil::standardisePath 只补/去尾部分隔符，目录主体不变
	const std::string with_slash = base + "/";
	const bool exact = (actual == base);
	const bool slashed = (actual == with_slash);
	EXPECT_TRUE(exact || slashed) << "getBinDir()=" << actual << " 与 cwd=" << base << " 对不上";
	// 两种形态只可能有一种成立，反证 standardisePath 没有把路径改写（例如去掉 ./ 前缀之外再动刀）
	EXPECT_NE(exact && slashed, true);
}

/* -------------------------------------------------------------------------
 * 四、initialize() 的配置缺失早退（main.cpp:96~105）
 * ---------------------------------------------------------------------- */

TEST(r14_quote_initialize, missing_config_stamps_module_dir_then_returns)
{
	WtHelper::set_module_dir("");
	// :100 load_from_file(不存在的路径) -> WTSCfgLoader.cpp:300 exists() 为假 -> NULL
	// -> :103 记 error -> :104 return，后面的 basefiles/caster/stateMonotor/writer/parsers 全不执行
	EXPECT_NO_THROW(initialize(kAbsentDtCfg));

	// :98 在早退之前无条件执行，因此可作为「确实进了 initialize()」的可证伪观测点
	const std::string expect(getBinDir());
	EXPECT_EQ(std::string(WtHelper::get_module_dir()), expect)
		<< "initialize() 没有把 module_dir 写成 getBinDir() 的值";

	// 第二次调用走 :53/:25 的假分支（缓存已建），结果必须与第一次一致
	initialize(kAbsentDtCfg);
	EXPECT_EQ(std::string(WtHelper::get_module_dir()), expect);
}

/* -------------------------------------------------------------------------
 * 五、DISABLED_：需要真实服务/线程的分支
 *
 * 触发条件与不启用理由：
 *  D1 main.cpp:304 为假分支的反面（配置存在）-> :310 initialize(filename)，
 *     里面 :109 cfgBF->get("session") 在 basefiles 缺失时对 NULL 解引用（空指针崩宿主），
 *     配置齐备时则 :254 g_parsers.run() / :260 g_stateMon.run() 起线程，
 *     且 :324 while(!bExit) 每 10ms 轮询永不退出 -> 启用即挂死。
 *     可回收行数估计：:96~262（initialize）+ :310~327 ≈ 185 行。
 *  D2 :271 之后不给出 -c/-l/-h 的裸 argv（argc==1）走默认 ./dtcfg.yaml，
 *     是否进入 D1 完全取决于运行目录里有没有该文件 —— 结论不可证伪，禁用。
 *  D3 :313 install_signal_hooks 会注册 SIGINT/SIGTERM 处理器并可能起监听线程，
 *     违反「禁止起线程」，只能等主控裁定进程外采样。
 * ---------------------------------------------------------------------- */

TEST(r14_quote_entry, DISABLED_existing_config_enters_parsers_run_and_endless_loop)
{
	// D1：需要一个真实可加载的 dtcfg.yaml + shm 目录；见上方触发条件
	EXPECT_EQ(call_entry({"quote_factory", "-c", "./dtcfg.yaml"}), 0);
}

TEST(r14_quote_entry, DISABLED_bare_argv_depends_on_cwd_contents)
{
	// D2：默认 ./dtcfg.yaml 是否存在取决于运行目录，断言无法由本用例输入算出
	EXPECT_EQ(call_entry({"quote_factory"}), 0);
}
