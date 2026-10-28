/*
 * R95 lane —— src/WtUftRunner/WtUftRunner.cpp 的 config()/run()
 *
 * R14 只走了 config() 的两条早退支（文件不存在 / basefiles 只有占位键且 env、data 缺失）。
 * 本文件把 R14 标为 DISABLED 的「成功支」拆成可控输入：
 *   - basefiles 四类子键（session / commodity / contract / holiday）的 string 与 array 两种形态；
 *   - parsers / traders 既可以直接是数组，也可以是指向独立 yaml 的文件名；
 *     模块名写不存在，ParserAdapter/TraderAdapter::init 会在 dlopen 前失败并返回 false，
 *     不加载任何插件、不起线程（WtUftCore/ParserAdapter.cpp:60-75、WtUftCore/TraderAdapter.cpp:162-177）；
 *   - strategies.uft 里的 active=false 与「active=true 但工厂目录为空」两条分支；
 *   - run() 用「先 raise(SIGINT) 让 _to_exit=true」的方式跳过 :362-365 的 10ms 轮询，
 *     ShareManager 未初始化（无 share_domain）时 :360 的 start_watching 直接返回 false
 *     （WtUftCore/ShareManager.cpp:78-81），因此不会出现看门狗线程。
 *
 * 每个用例 chdir 到自己的系统临时目录，产品读写的相对路径（生成的日志/工厂目录）
 * 都落在临时目录内，用例退出时恢复并删除。日志不落仓库。
 */
#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

#include "WtUftRunner/WtUftRunner.h"

namespace {

namespace fs = std::filesystem;

/* 进入 <temp>/wt_r95_uft_<tag>_<pid>，析构时恢复 cwd 并整目录删除 */
class ScopedTempCwd
{
public:
	explicit ScopedTempCwd(const char* tag)
	{
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

/* WTSSessionInfo 的 0~2400 单段：任何 hhmm 都在交易时段内，ticker 线程不会落进 10s 休眠。
 * 2400 经 offsetTime(..., false) 仍是 2400，isLastOfSection 只在 24:00 边界命中。 */
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

/* A：basefiles 用 string，parsers/traders 直接给数组，strategies.uft 同时含 inactive 与 active */
const char* kConfigA =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
	"    holiday: holidays_absent.yaml\n"
	"bspolicy: actpolicy_absent.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers:\n"
	"    - active: false\n"
	"      id: p_off\n"
	"    - active: true\n"
	"      module: no_such_parser\n"
	"traders:\n"
	"    - active: false\n"
	"      id: t_off\n"
	"    - active: true\n"
	"      id: t_one\n"
	"      module: no_such_trader\n"
	"strategies:\n"
	"    uft:\n"
	"        - active: false\n"
	"          id: s_off\n"
	"          name: s_off\n"
	"        - active: true\n"
	"          id: s_one\n"
	"          name: s_one\n"
	"          trader: t_one\n";

/* B：basefiles 用 array，parsers/traders 指向独立文件 */
const char* kConfigB =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity:\n"
	"        - commodities.yaml\n"
	"        - commodities_absent.yaml\n"
	"    contract:\n"
	"        - contracts.yaml\n"
	"        - contracts_absent.yaml\n"
	"bspolicy: actpolicy_absent.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers: parsers.yaml\n"
	"traders: traders.yaml\n";

const char* kParsersYaml =
	"parsers:\n"
	"    - active: false\n"
	"      id: p_off\n"
	"    - active: true\n"
	"      module: no_such_parser\n";

const char* kTradersYaml =
	"traders:\n"
	"    - active: false\n"
	"      id: t_off\n"
	"    - active: true\n"
	"      id: t_one\n"
	"      module: no_such_trader\n";

/* C：parsers/traders 文件名不存在；strategies.uft 是对象而不是数组 */
const char* kConfigC =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers: parsers_absent.yaml\n"
	"traders: traders_absent.yaml\n"
	"strategies:\n"
	"    uft:\n"
	"        not_an_array: 1\n";

/* D：文件存在但后缀不是 yaml/json -> WTSCfgLoader::load_from_file 返回 NULL */
const char* kConfigD =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers: parsers.cfg\n"
	"traders: traders.cfg\n";

/* E：parser 文件里没有 parsers 键；trader 文件里 traders 是对象；strategies 里没有 uft */
const char* kConfigE =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"parsers: parsers_no_key.yaml\n"
	"traders: traders_object.yaml\n"
	"strategies:\n"
	"    other: 1\n";

/* run() 用的最小成功配置：只要 session + env + data；无 share_domain/parsers/traders/strategies */
const char* kConfigRun =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"data:\n"
	"    unused: 1\n"
	"bspolicy: actpolicy_absent.yaml\n";

bool prepare_common()
{
	return write_text("sessions.yaml", kSessionsYaml)
		&& write_text("commodities.yaml", kCommoditiesYaml)
		&& write_text("contracts.yaml", kContractsYaml);
}

} // namespace

/* -------------------------------------------------------------------------
 * 一、config() 的成功支（WtUftRunner.cpp:60-198）
 * ---------------------------------------------------------------------- */

TEST(r95_uft_config, string_basefiles_and_array_modules_return_true)
{
	ScopedTempCwd guard("cfg_a");
	ASSERT_TRUE(guard.ok()) << "无法进入临时工作目录";
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_a.yaml", kConfigA));

	WtUftRunner runner;
	// :70-107 四类 basefiles 子键（session 命中、commodity/contract 走 VT_String、holiday 文件缺失）
	// :110 initEngine(env 对象) / :113 initDataMgr(data 对象)
	// :115 无 share_domain -> 不碰共享内存
	// :124 bspolicy 缺失 -> init 返回 false 后继续
	// :130-160 parsers 直接数组：inactive 跳过；active 的空 id 生成 auto_parser_NNNN；
	//         UFT ParserAdapter 因 module 不存在在 dlopen 前失败，但 initParsers 仍 addAdapter
	// :163-193 traders 直接数组：inactive 跳过；active 条目的 TraderAdapter::init 同样在 dlopen 前失败
	// :195/:200-251 strategies.uft 数组：inactive 跳过；active 条目因工厂目录为空 createStrategy 返回 NULL
	EXPECT_TRUE(runner.config("cfg_a.yaml"));
}

TEST(r95_uft_config, array_basefiles_and_string_module_files_return_true)
{
	ScopedTempCwd guard("cfg_b");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("parsers.yaml", kParsersYaml));
	ASSERT_TRUE(write_text("traders.yaml", kTradersYaml));
	ASSERT_TRUE(write_text("cfg_b.yaml", kConfigB));

	WtUftRunner runner;
	// :81-87 commodity 走 VT_Array（第一个文件有效、第二个缺失）
	// :97-103 contract 走 VT_Array
	// :133-145 parsers 是文件名：文件存在 -> 读入 -> initParsers(var->get("parsers"))
	// :166-177 traders 是文件名：同上
	EXPECT_TRUE(runner.config("cfg_b.yaml"));
}

TEST(r95_uft_config, missing_module_files_and_object_uft_shape_return_true)
{
	ScopedTempCwd guard("cfg_c");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("cfg_c.yaml", kConfigC));

	WtUftRunner runner;
	// :151-154 parsers 指向不存在的文件 -> 记 error 后继续
	// :184-187 traders 指向不存在的文件 -> 记 error 后继续
	// :206-208 strategies.uft 是对象 -> 类型不是 VT_Array，initUftStrategies 返回 false
	EXPECT_TRUE(runner.config("cfg_c.yaml"));
}

TEST(r95_uft_config, unparsable_module_files_hit_load_failure_paths)
{
	ScopedTempCwd guard("cfg_d");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("parsers.cfg", "not: a-supported-extension\n"));
	ASSERT_TRUE(write_text("traders.cfg", "not: a-supported-extension\n"));
	ASSERT_TRUE(write_text("cfg_d.yaml", kConfigD));

	WtUftRunner runner;
	// :136-149 文件存在但 WTSCfgLoader 因后缀不认识返回 NULL -> :148 记 error
	// :169-182 trader 配置文件同样 -> :181 记 error
	EXPECT_TRUE(runner.config("cfg_d.yaml"));
}

TEST(r95_uft_config, parser_without_key_and_traders_object_are_rejected)
{
	ScopedTempCwd guard("cfg_e");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_common());
	ASSERT_TRUE(write_text("parsers_no_key.yaml", "other: value\n"));
	ASSERT_TRUE(write_text("traders_object.yaml", "traders:\n    only: object\n"));
	ASSERT_TRUE(write_text("cfg_e.yaml", kConfigE));

	WtUftRunner runner;
	// :142 initParsers(var->get("parsers")) 收到 NULL -> :282-283 return false
	// :175 initTraders(var->get("traders")) 收到对象 -> :315-316 return false
	// :202-208 strategies 对象里没有 uft -> initUftStrategies 返回 false
	EXPECT_TRUE(runner.config("cfg_e.yaml"));
}

/* -------------------------------------------------------------------------
 * 二、run()（WtUftRunner.cpp:351-373）
 *
 * 先 raise(SIGINT)：构造函数在 :40-45 安装的回调把 _to_exit 置 true，
 * run() 里 engine/parsers/traders 仍会执行，但 :362-365 的 while 立即通过。
 * 无 share_domain 时 :360 start_watching(2) 因 ShareManager::_inited=false 直接返回 false。
 * ---------------------------------------------------------------------- */
TEST(r95_uft_run, signal_flag_makes_run_return_immediately)
{
	ScopedTempCwd guard("run");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("cfg_run.yaml", kConfigRun));

	WtUftRunner runner;
	ASSERT_TRUE(runner.config("cfg_run.yaml"));

	// :42-45 的信号回调，参数 int 隐式转 bool -> _to_exit=true
	ASSERT_EQ(std::raise(SIGINT), 0);

	// :355 _uft_engine.run()：无上下文，只创建并启动 UftRtTicker；
	// :357-358 空的 parser/trader 管理器；:360 start_watching 未初始化返回 false；
	// :362-365 因 _to_exit 已为 true 不进入轮询。runner 析构时 WtUftEngine 停止 ticker。
	EXPECT_NO_THROW(runner.run(false));

	// 还原默认 SIGINT 处理，避免析构后仍挂着指向本栈对象的回调
	std::signal(SIGINT, SIG_DFL);
}
