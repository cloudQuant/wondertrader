/*
 * R95 lane —— src/WtLatencyHFT/HftLatencyTool.cpp 的 init()/run()
 *
 * R14 只覆盖了「config.yaml 不存在 -> init() 返回 false」；本文件用自己写的
 * config.yaml 进入成功支（HftLatencyTool.cpp:360-419），并继续覆盖
 * initEngine(:446-453)、initModules(:461-478)、initStrategies(:426-438) 与 run(:484-506)。
 *
 * 关键前提（全部来自代码，不靠运行目录的巧合）：
 *   - basefiles 段必须存在（:361-362 不判空），子键可以缺；缺子键只会跳过对应的 load。
 *   - env 段必须是对象（WtHftEngine::init 在 WtHftEngine.cpp:59-63 无条件 _cfg->retain()）。
 *   - run() 的 ticker 需要时段：env.product.session=ALLDAY，basefiles.session 指向自写文件。
 *   - _act_mgr.init("actpolicy.yaml") 是硬编码相对路径；用例 chdir 到临时目录，文件缺失 -> false 被忽略。
 *   - TestParser/TestTrader 的 init(NULL) 取接口默认实现返回 false，适配器只记日志、
 *     不 dlopen 插件、不起接收线程（WtCore/ParserAdapter.cpp:45-78、TraderAdapter.cpp:209-232）。
 *   - run() 只对 times=0/1 取样：times=0 走完 TestParser::run 的头尾（:97-104、:189-194）；
 *     times=1 且不加载合约，循环体在 :113-114 因 getContract 返回 NULL 立刻返回。
 *     不构造 tick，不触发策略回调与下单路径。
 *
 * 每个用例 chdir 到系统临时目录，产品写出的 generated/ 落到临时目录，用完删除。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

#include "WtLatencyHFT/HftLatencyTool.h"

/* 生产 TU HftLatencyTool.cpp:33 的自由函数（main.cpp:19 也是这样声明的） */
void test_hft();

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
		m_dir /= std::string("wt_r95_hft_") + tag + "_" + std::to_string(::getpid());

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

/* 0~2400 单段：任何 hhmm 都在交易时段内，ticker 线程只做 10ms 轮询，stop 后立刻 join */
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

/* 成功支 A：string 形态的 commodity/contract + 缺失的 hot 文件 */
const char* kConfigA =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"    commodity: commodities.yaml\n"
	"    contract: contracts.yaml\n"
	"    hot: hots_absent.yaml\n"
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
	"    hot: hots_absent.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 3\n"
	"core: 0\n";

/* 成功支 C：basefiles 只有一个无关子键；commodity/contract/hot 的 if 全为假 */
const char* kConfigC =
	"basefiles:\n"
	"    unused: 1\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：只加载 session，times=0 -> TestParser::run 头尾 */
const char* kConfigRunZero =
	"basefiles:\n"
	"    session: sessions.yaml\n"
	"env:\n"
	"    product:\n"
	"        session: ALLDAY\n"
	"times: 0\n"
	"core: 0\n";

/* run() 用：只加载 session，times=1 且没有合约 -> 循环体在 getContract 处早退 */
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
 * 一、init() 成功支（HftLatencyTool.cpp:347-419）
 * ---------------------------------------------------------------------- */

TEST(r95_hft_init, string_basefiles_reach_all_success_stages)
{
	ScopedTempCwd guard("init_a");
	ASSERT_TRUE(guard.ok()) << "无法进入临时工作目录";
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigA));

	hft::HftLatencyTool tool;
	// :363-364 session 有效；:366-380 commodity 走 VT_String；:382-396 contract 走 VT_String
	// :398-402 hot 文件缺失 -> loadHots 内部记日志（WTSHotMgr.cpp:117-122），仍继续
	// :405 actpolicy.yaml 不存在 -> init 返回 false 后被忽略
	// :407-411 times/core 读入；:413 initEngine(env 对象) -> WtHftEngine::init
	// :414 initModules()：TestParser/TestTrader 的 init(NULL) 取默认 false，不 dlopen 插件
	// :415 initStrategies()：HftStraContext 绑定 "trader" 适配器并加入引擎
	// :417 释放根配置后返回 true
	ASSERT_TRUE(tool.init());

	// 成功支的可见副作用：WtEngine::init 的 load_datas/init_outputs 与 TraderAdapter::initSaveData
	EXPECT_TRUE(stdfs::exists("generated/portfolio/trades.csv"));
	EXPECT_TRUE(stdfs::exists("generated/traders/trader/orders.csv"));
}

TEST(r95_hft_init, array_basefiles_for_commodity_and_contract_are_walked)
{
	ScopedTempCwd guard("init_b");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(prepare_base());
	ASSERT_TRUE(write_text("config.yaml", kConfigB));

	hft::HftLatencyTool tool;
	// :373-379 commodity 是 VT_Array：两次 loadCommodities（第二个文件不存在）
	// :389-395 contract 是 VT_Array：同上
	EXPECT_TRUE(tool.init());
}

TEST(r95_hft_init, absent_optional_basefiles_children_are_skipped)
{
	ScopedTempCwd guard("init_c");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("config.yaml", kConfigC));

	hft::HftLatencyTool tool;
	// :363/:367/:383/:398 四个 if 全假：session/commodity/contract/hot 都不加载
	// :413-415 仍要建引擎、适配器与策略上下文，:417 返回 true
	EXPECT_TRUE(tool.init());
}

/* -------------------------------------------------------------------------
 * 二、run()（HftLatencyTool.cpp:484-506）
 * ---------------------------------------------------------------------- */

TEST(r95_hft_run, zero_ticks_after_successful_init_exercises_engine_and_parser_tail)
{
	ScopedTempCwd guard("run_zero");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	hft::HftLatencyTool tool;
	ASSERT_TRUE(tool.init());

	// :486-492 core==0 -> 不绑定 CPU
	// :496/:497 parsers/traders 各只有一个内存适配器，connect 是接口默认空实现
	// :499 _engine.run()：策略 on_init、写 marker.json、启动 HftRtTicker（析构时 stop/join）
	// :501 TestParser::run(0)：:97-104 初始化 + 跳过循环 + :189-194 计算/输出
	EXPECT_NO_THROW(tool.run());

	EXPECT_TRUE(stdfs::exists("generated/marker.json")) << "engine.run() 没有走到写 marker 的段";
}

TEST(r95_hft_run, one_tick_without_contract_returns_before_tick_creation)
{
	ScopedTempCwd guard("run_one");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunOneWithoutContract));

	hft::HftLatencyTool tool;
	ASSERT_TRUE(tool.init());

	// :104-114 循环进入一次，_bd_mgr->getContract("rb2205","SHFE") 为 NULL -> 直接 return，
	// 因此不会创建 WTSTickData，也不会触发策略 on_tick/下单。
	EXPECT_NO_THROW(tool.run());

	EXPECT_TRUE(stdfs::exists("generated/marker.json"));
}

/* -------------------------------------------------------------------------
 * 三、入口包装 test_hft()（HftLatencyTool.cpp:33-42）
 * ---------------------------------------------------------------------- */
TEST(r95_hft_entry, free_function_runs_init_and_run_with_valid_config)
{
	ScopedTempCwd guard("entry");
	ASSERT_TRUE(guard.ok());
	ASSERT_TRUE(write_text("sessions.yaml", kSessionsYaml));
	ASSERT_TRUE(write_text("config.yaml", kConfigRunZero));

	// :36 栈上构造 HftLatencyTool；:38 init()（配置有效 -> true）；
	// :41 run()（times=0，引擎/解析器/交易器同上）；出作用域走工具析构，ticker stop/join。
	EXPECT_NO_THROW(test_hft());
	EXPECT_TRUE(stdfs::exists("generated/marker.json"));
}
