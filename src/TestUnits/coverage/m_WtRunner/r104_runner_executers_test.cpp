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

#include <memory>
#include <string>

#include "Includes/WTSVariant.hpp"
#include "Share/StdUtils.hpp"
#include "WtCore/WtHelper.h"
#include "WtRunner/WtRunner.h"

using namespace wtp;

namespace {

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

} // namespace

/* =========================================================================
 * 一、三个子初始化器的 active 体（WtRunner.cpp:440~455 / 476~544 / 563~571）
 * ====================================================================== */

TEST(r104_wtrunner_config, traders_parsers_and_all_three_executer_kinds)
{
	prepare_base_fixtures();
	std::unique_ptr<WtRunner> runner = make_runner();
	runner->init(fixture("logcfg.yaml"));

	/*
	 * config():196~226 traders 字符串/数组段都走数组分支 -> initTraders():557~575
	 *   :560 active 真 -> :565 new TraderAdapter -> :566 init(module 不存在, 内部返回 false)
	 *   -> :568 addAdapter -> :570 count++ -> :573 "{} traders loaded"
	 * config():189~192 parsers 数组 -> initParsers():434~459
	 *   :437 active 真 -> :440~452（第二条 id 为空 -> :444~448 自动 id）-> :454 count++
	 * config():231~268 executers 数组 -> initExecuters():470~548
	 *   :481~507 local（:494~500 能拿到 r104wt_t1，走 setTrader + addSink）
	 *   :508~534 diff （第一条 trader 也是 r104wt_t1 -> :521~528 的「找到」支；
	 *                   第二条 trader 不存在 -> :529~531 的错误支）
	 *   :535~542 dist
	 */
	const std::string cfg = basefiles_block() + env_block() + tail_block()
		+ "traders:\n"
		+ "-   id: r104wt_t1\n    active: true\n    module: R104NoSuchTraderModule\n"
		+ "parsers:\n"
		+ "-   id: r104wt_p1\n    active: true\n    module: R104NoSuchParserModule\n"
		+ "-   id: \"\"\n    active: true\n    module: R104NoSuchParserModule\n"
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
