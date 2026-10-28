/*
 * R95 lane —— src/WtLatencyUFT/UftLatencyTool.cpp 的 init()/run()
 *
 * R14 只覆盖了「config.yaml 不存在 -> init() 返回 false」；本文件用自写 config.yaml
 * 进入成功支（UftLatencyTool.cpp:211-260），并覆盖 initEngine(:276-283)、
 * initModules(:286-303)、initStrategies(:262-274) 与 run(:305-327)。
 *
 * 关键前提：
 *   - 两个硬编码相对路径 "logcfg.yaml" / "config.yaml"（:200/:202-204）由用例 chdir 到
 *     系统临时目录后自己写 config.yaml 决定；目录里不写 logcfg.yaml，日志器保持未初始化。
 *   - basefiles 段必须存在（:212-213 不判空），子键可缺；env 段必须是对象
 *     （WtUftEngine::init 只在 _cfg 非空时 retain，但 run() 的 ticker 需要 env.product.session）。
 *   - env.product.session=ALLDAY，basefiles.session 指向自写时段文件；
 *     WtUftRtTicker 的 0~2400 单段让线程只做 10ms 轮询，析构 stop 时立刻 join。
 *   - TestParser/TestTrader 的 init(NULL) 取接口默认 false；UFT TraderAdapter::initExt
 *     甚至不看返回值（WtUftCore/TraderAdapter.cpp:90-101），不会 dlopen 插件、不起线程。
 *   - run() 只对 times=0/1 取样：times=0 覆盖 TestParser::run 头尾（:41-44、:115-117）；
 *     times=1 且不加载合约，循环体在 :50-52 因 getContract 返回 NULL 早退。
 *
 * 每个用例 chdir 到系统临时目录，产品读写的相对路径全部落在临时目录，用完删除。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

#include "WtLatencyUFT/UftLatencyTool.h"

/* 生产 TU UftLatencyTool.cpp:28 的自由函数（main.cpp:19 也是这样声明的） */
void test_uft();

namespace {

namespace stdfs = std::filesystem;

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
		m_dir /= std::string("wt_r95_ufttool_") + tag + "_" + std::to_string(::getpid());

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

bool write_text(const char* name, const std::string& text)
{
	std::ofstream out(name, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out << text;
	out.close();
	return !out.fail();
}

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

bool prepare_base()
{
	return write_text("sessions.yaml", kSessionsYaml)
		&& write_text("commodities.yaml", kCommoditiesYaml)
		&& write_text("contracts.yaml", kContractsYaml);
}

/* 成功支 A：string 形态的 commodity/contract */
const char* kConfigA =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
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
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* 成功支 C：basefiles 只有无关子键；session/commodity/contract 的 if 全为假 */
const char* kConfigC =
	"basefiles:\n"
	"    unused: 1\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：只加载 session，times=0 */
const char* kConfigRunZero =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：只加载 session，times=1 且无合约 */
const char* kConfigRunOneWithoutContract =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 1\n"
	"core: 0\n";

} // namespace

/* -------------------------------------------------------------------------
 * 一、init() 成功支（UftLatencyTool.cpp:198-260）
 * ---------------------------------------------------------------------- */

TEST(r95_ufttool_init, string_basefiles_reach_all_success_stages)
{
	ScopedTempCwd guard("init_a");
	ASSERT_TRUE(guard.ok()) << "无法进入临时工作目录";
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigA));

	uft::UftLatencyTool tool;
	// :213-214 session 有效；:216-230 commodity 走 VT_String；:232-246 contract 走 VT_String
	// :248-252 times/core；:254 initEngine(env) -> WtUftEngine::init 只存指针并 retain
	// :255 initModules()：TestParser/TestTrader 只注册到内存适配器
	// :256 initStrategies()：UftStraContext 绑定 "trader" 并加入引擎
	// :258 释放根配置后返回 true
	EXPECT_TRUE(tool.init());
}

TEST(r95_ufttool_init, array_basefiles_for_commodity_and_contract_are_walked)
{
	ScopedTempCwd guard("init_b");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigB));

	uft::UftLatencyTool tool;
	// :223-229 commodity 是 VT_Array：两次 loadCommodities（第二个文件不存在）
	// :239-245 contract 是 VT_Array：同上
	EXPECT_TRUE(tool.init());
}

TEST(r95_ufttool_init, absent_optional_basefiles_children_are_skipped)
{
	ScopedTempCwd guard("init_c");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("config.yaml", kConfigC));

	uft::UftLatencyTool tool;
	// :213/:217/:233 三个 if 全假：不加载 session/commodity/contract
	// :254-256 仍要建引擎、适配器与策略上下文，:260 返回 true
	EXPECT_TRUE(tool.init());
}

/* -------------------------------------------------------------------------
 * 二、run()（UftLatencyTool.cpp:305-327）
 * ---------------------------------------------------------------------- */

TEST(r95_ufttool_run, zero_ticks_after_successful_init_exercises_engine_and_parser_tail)
{
	ScopedTempCwd guard("run_zero");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	uft::UftLatencyTool tool;
	ASSERT_TRUE(tool.init());

	// :307-313 core==0 -> 不绑定 CPU
	// :317/:318 parsers/traders 各一个内存适配器，connect 是接口默认空实现
	// :320 _engine.run()：策略 on_init、启动 UftRtTicker（析构时 stop/join）
	// :322 TestParser::run(0)：:41-44 初始化 + 跳过循环 + :115-117 计算/输出
	EXPECT_NO_THROW(tool.run());
}

TEST(r95_ufttool_run, one_tick_without_contract_returns_before_tick_creation)
{
	ScopedTempCwd guard("run_one");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunOneWithoutContract));

	uft::UftLatencyTool tool;
	ASSERT_TRUE(tool.init());

	// :45-52 循环进入一次，_bd_mgr->getContract("rb2205","SHFE") 为 NULL -> 直接 return，
	// 不会创建 WTSTickData，也不会触发策略 on_tick/下单。
	EXPECT_NO_THROW(tool.run());
}

/* -------------------------------------------------------------------------
 * 三、入口包装 test_uft()（UftLatencyTool.cpp:28-34）
 * ---------------------------------------------------------------------- */
TEST(r95_ufttool_entry, free_function_runs_init_and_run_with_valid_config)
{
	ScopedTempCwd guard("entry");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	// :30 栈上构造 UftLatencyTool；:31 init()（配置有效 -> true）；
	// :33 run()（times=0，引擎/解析器/交易器同上）；出作用域走工具析构，ticker stop/join。
	EXPECT_NO_THROW(test_uft());
}
