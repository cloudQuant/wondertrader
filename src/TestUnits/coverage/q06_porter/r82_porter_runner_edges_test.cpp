/*
 * R82 - WtPorter / WtRtRunner 剩余行段补测
 *
 * 目标：
 *   src/WtPorter/WtPorter.cpp（missed 280/848）
 *     - config_porter 转发（L260-266）
 *     - CTA/SEL/HFT 真实上下文下的下单、订阅、切片取值与空切片分支
 *       （L420-487、L501-568、L673-687、L743-750、L872-875、L985-992、
 *        L1249-1263、L1275-1282、L1295-1322、L1333-1341、L1354-1381、
 *        L1391-1398、L1409-1412、L1668-1675、L1725-1732、L1797-1866、
 *        L1881-1905、L1920-1944、L1959-1983、L2027-2087、L2100-2211）
 *     - release_porter 导出（L286-289）
 *   src/WtPorter/WtRtRunner.cpp（missed 276/938）
 *     - 扩展 K 线加载器 KP_Minute1 与未知周期（L224、L241-242、L249）
 *     - ctx_on_session_event 的 EngineType default（L436-437）
 *     - basefiles 主力回填的 continue（L572）
 *     - config 各段失败臂：parser 文件缺失、traders/executers 文件坏 JSON 与
 *       非数组、data 段缺失、strategies.cta 缺失（L615-618、L640-646、
 *       L678-689、L724、L888、L936）
 *     - initHftStrategies 缺 hft 段与无工厂时的 continue（L799、L807-814）
 *     - initEngine 缺 env 段（L842）
 *     - initExecuters 空 name->local、缺 trader、diff/arbi 未知 trader（L952-964、
 *       L1000-1003、L1027-1030）
 *     - handleLogAppend（L1137-1138）
 *     - release（L1141-1155）
 *     - on_ext_parser_quote 命中支（L1247-1251）
 *
 * 边界：只用公开 API；无 private 宏、无类布局镜像；不启动行情/交易线程，
 *       不调 run()。真实上下文一律由进程单例 create_*_context 建立，
 *       runner 侧边界一律用「堆上 new、永不 delete」的实例规避 BUG-R46-001。
 *       临时文件只写系统临时目录并按 pid+tag 隔离，每个用例清理。
 *       release 用例固定写在文件最后（WTSLogger::stop 是进程级静默，见 BUG-R6-006）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

USING_NS_WTP;

/* WtPorter.cpp:34-38 的进程单例 */
extern WtRtRunner& getRunner();

namespace {

	std::string prepare_dir(const char* tag)
	{
		std::filesystem::path p = std::filesystem::temp_directory_path()
			/ (std::string("r82_porter_") + tag + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(p, ec);
		std::filesystem::create_directories(p, ec);
		return p.string();
	}

	void clean_dir(const std::string& dir)
	{
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	WtRtRunner* new_heap_runner(const std::string& dir)
	{
		/* 堆上构造、永不 delete：见 BUG-R46-001（构造列表漏初始化 _config/_engine）。 */
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r82_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\","
		"\"pricetick\":0.02,\"volscale\":1000,\"covermode\":0,\"pricemode\":0,"
		"\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"}}}";

	const char* kHotRules =
		"{\"SHFE\":{\"au\":[{\"from\":\"au2406\",\"to\":\"au2406\",\"date\":20240101,"
		"\"oldclose\":100.0,\"newclose\":100.0}]}}";

	const char* kActions = "{}";

	/* 真实上下文用的完整配置：session/commodity/contract/自定义主力规则 + 指定
	 * env + 一个只有 id 的 trader（TraderAdapter 入表失败但句柄可用，供 HFT 绑定）。 */
	std::string real_cfg(const std::string& dir, const char* envName)
	{
		return "{\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\","
			"\"rules\":{\"R82\":\"" + dir + "/hot.json\"}},"
			"\"env\":{\"name\":\"" + envName + "\"},"
			"\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\","
			"\"traders\":[{\"active\":true,\"id\":\"r82_trd\"}]}";
	}

	/* 每族引擎在新进程里只初始化一次；C API 用例各自可先于其它用例执行。
	 * 注意 WtRtRunner::config 只 init 当前 env 对应的引擎，所以 SEL/HFT 用例
	 * 必须先用对应 env 再配置一次，否则各自引擎的 _base_data_mgr/_hot_mgr 为空。 */
	bool g_cta_configured = false;
	bool g_sel_configured = false;
	bool g_hft_configured = false;
	std::string g_current_env;

	void configure_porter(const std::string& dir, const char* envName, bool& flag)
	{
		getRunner().init((dir + "/r82_absent_logcfg.yaml").c_str(), true, dir.c_str());
		/* WtPorter.cpp:260-266 的 isFile=false 转发支 */
		config_porter(real_cfg(dir, envName).c_str(), false);
		g_current_env = envName;
		flag = true;
	}

	/* engine 是「当前 env 那一个」的指针；切换 env 必须重新 config，
	 * 否则 create_*_context 会拿到未 init 的兄弟引擎。 */
	bool need_config(bool flag, const char* envName)
	{
		return !flag || g_current_env != envName || getRunner().getEngine() == nullptr;
	}

	void ensure_cta_configured(const std::string& dir)
	{
		if (need_config(g_cta_configured, "cta"))
			configure_porter(dir, "cta", g_cta_configured);
	}

	void ensure_sel_configured(const std::string& dir)
	{
		if (need_config(g_sel_configured, "sel"))
			configure_porter(dir, "sel", g_sel_configured);
	}

	void ensure_hft_configured(const std::string& dir)
	{
		if (need_config(g_hft_configured, "hft"))
			configure_porter(dir, "hft", g_hft_configured);
	}

	void write_real_fixtures(const std::string& dir)
	{
		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodities.json", kCommodities);
		write_text(dir + "/contracts.json", kContracts);
		write_text(dir + "/hot.json", kHotRules);
		write_text(dir + "/actions.json", kActions);
	}

	const char* kMonthCode = "SHFE.au.2406";

	struct BarsRec
	{
		uint32_t	calls = 0;
		void clear() { *this = BarsRec(); }
	};

	BarsRec g_bars;
	BarsRec g_ticks;

	struct PosRec
	{
		uint32_t	calls = 0;
		std::string	last_code;
		double		last_qty = -1;
		bool		last_islast = false;
		void clear() { *this = PosRec(); }
	};

	PosRec g_pos;

	void bars_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, const char* /*period*/,
		WTSBarStruct* /*bar*/, WtUInt32 /*count*/, bool /*isLast*/)
	{
		g_bars.calls++;
	}

	void ticks_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, WTSTickStruct* /*tick*/,
		WtUInt32 /*count*/, bool /*isLast*/)
	{
		g_ticks.calls++;
	}

	void pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double position, bool isLast)
	{
		g_pos.calls++;
		g_pos.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_pos.last_qty = position;
		g_pos.last_islast = isLast;
	}

	/* 扩展 K 线加载器的取样记录 */
	struct LoadRec
	{
		uint32_t	calls = 0;
		std::string	code;
		std::string	period;
		void clear() { *this = LoadRec(); }
	};

	LoadRec g_fnl;
	LoadRec g_raw;
	LoadRec g_fct;

	bool fnl_loader(const char* stdCode, const char* period)
	{
		g_fnl.calls++;
		g_fnl.code = (stdCode == NULL) ? "<null>" : stdCode;
		g_fnl.period = (period == NULL) ? "<null>" : period;
		return true;
	}

	bool raw_loader(const char* stdCode, const char* period)
	{
		g_raw.calls++;
		g_raw.code = (stdCode == NULL) ? "<null>" : stdCode;
		g_raw.period = (period == NULL) ? "<null>" : period;
		return true;
	}

	bool fct_loader(const char* stdCode)
	{
		g_fct.calls++;
		g_fct.code = (stdCode == NULL) ? "<null>" : stdCode;
		return true;
	}

	void bar_sink(void* /*obj*/, WTSBarStruct* /*bars*/, uint32_t /*count*/) {}
	void fct_sink(void* /*obj*/, const char* /*code*/, uint32_t* /*dates*/,
		double* /*factors*/, uint32_t /*count*/) {}

} // namespace

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:420-487 + 501-568 + 673-687 + 743-750 + 872-875 + 985-992
 * 真实 CTA 上下文：四个开平入口、目标仓位、tick 订阅、K线/tick 空切片、
 * 日价格与持仓枚举的终止符。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, CtaEntriesAndSliceMisses)
{
	const std::string dir = prepare_dir("cta");
	write_real_fixtures(dir);
	ensure_cta_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	const CtxHandler h = create_cta_context("r82_cta_real", 0);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getCtaContext(h) != NULL);

	/* Enter/exit 只改动本地信号表；stra_get_position 优先读信号量，可精确断言。 */
	cta_enter_long(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 1.0);

	cta_exit_long(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 0.0);

	cta_enter_short(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), -1.0);

	cta_exit_short(h, kMonthCode, 1, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 0.0);

	cta_set_position(h, kMonthCode, 2, "r82", 0, 0);
	EXPECT_DOUBLE_EQ(cta_get_position(h, kMonthCode, false, ""), 2.0);
	cta_set_position(h, kMonthCode, 0, "r82", 0, 0);

	/* L985-992：订阅只写本地集合与引擎订阅表，不启动线程。 */
	EXPECT_NO_THROW(cta_sub_ticks(h, kMonthCode));

	/* L501-531 / L544-568 的 kData/tData == NULL 支：data 段为空 -> reader 为空。 */
	g_bars.clear();
	g_ticks.clear();
	EXPECT_EQ(cta_get_bars(h, kMonthCode, "m1", 5, false, bars_cb), 0U);
	EXPECT_EQ(cta_get_ticks(h, kMonthCode, 5, ticks_cb), 0U);
	EXPECT_EQ(g_bars.calls, 0U);
	EXPECT_EQ(g_ticks.calls, 0U);

	/* L872-875：data mgr 的实时 tick 缓存为空 -> 0。 */
	EXPECT_DOUBLE_EQ(cta_get_day_price(kMonthCode, 0), 0.0);

	/* L673-687：空持仓表只发终止符。 */
	g_pos.clear();
	cta_get_all_position(h, pos_cb);
	ASSERT_EQ(g_pos.calls, 1U);
	EXPECT_TRUE(g_pos.last_islast);
	EXPECT_EQ(g_pos.last_code, "");

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:1249-1263 + 1275-1282 + 1295-1322 + 1333-1341 + 1354-1381
 * + 1391-1398 + 1409-1412：真实 SEL 上下文的置仓、订阅、K线/tick 空切片、
 * 空持仓枚举。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, SelEntriesAndSliceMisses)
{
	const std::string dir = prepare_dir("sel");
	write_real_fixtures(dir);
	ensure_sel_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	const CtxHandler h = create_sel_context("r82_sel_real", 20240101, 93000, "d",
		"CHINA", "TRADING", 0);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getSelContext(h) != NULL);

	/* 空持仓表 -> set_position 只追加本地信号，读回仍是 0。 */
	sel_set_position(h, kMonthCode, 3, "r82");
	EXPECT_DOUBLE_EQ(sel_get_position(h, kMonthCode, false, ""), 0.0);
	sel_set_position(h, kMonthCode, 0, "r82");

	EXPECT_NO_THROW(sel_sub_ticks(h, kMonthCode));

	g_bars.clear();
	g_ticks.clear();
	EXPECT_EQ(sel_get_bars(h, kMonthCode, "m1", 5, bars_cb), 0U);
	EXPECT_EQ(sel_get_ticks(h, kMonthCode, 5, ticks_cb), 0U);
	EXPECT_EQ(g_bars.calls, 0U);
	EXPECT_EQ(g_ticks.calls, 0U);

	EXPECT_DOUBLE_EQ(sel_get_day_price(kMonthCode, 3), 0.0);

	g_pos.clear();
	sel_get_all_position(h, pos_cb);
	ASSERT_EQ(g_pos.calls, 1U);
	EXPECT_TRUE(g_pos.last_islast);

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:1668-1675 + 1725-1732 + 1797-1866 + 1881-1983 + 2027-2087
 * + 2100-2211：绑定真实 TraderAdapter 的 HFT 上下文。qty=0 让 TraderAdapter
 * 在写 _stat_map 之前返回，从而覆盖 buy/sell 全段而不触碰未初始化的风控表。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, HftEntriesAndMicroSliceMisses)
{
	const std::string dir = prepare_dir("hft");
	write_real_fixtures(dir);
	ensure_hft_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	/* r82_trd 在 config 里以无 module 的 TraderAdapter 入表：id 可用但 API 为空。 */
	const CtxHandler h = create_hft_context("r82_hft_real", "r82_trd", false, 0);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getHftContext(h) != NULL);

	/* 持仓/未完成量都走 TraderAdapter 的 map，空表 -> 0。 */
	EXPECT_DOUBLE_EQ(hft_get_position(h, kMonthCode, true), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_profit(h, kMonthCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_position_avgpx(h, kMonthCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_undone(h, kMonthCode), 0.0);

	/* L2100-2140：空订单表 -> false / 空串。 */
	EXPECT_FALSE(hft_cancel(h, 1));
	EXPECT_STREQ(hft_cancel_all(h, kMonthCode, true), "");

	/* L2157-2211：qty=0 时 TraderAdapter::buy/sell 首行即返回空 OrderIDs。 */
	EXPECT_STREQ(hft_buy(h, kMonthCode, 100.0, 0, "r82", 0), "");
	EXPECT_STREQ(hft_sell(h, kMonthCode, 100.0, 0, "r82", 0), "");

	EXPECT_NO_THROW(hft_sub_ticks(h, kMonthCode));
	EXPECT_NO_THROW(hft_sub_order_queue(h, kMonthCode));
	EXPECT_NO_THROW(hft_sub_order_detail(h, kMonthCode));
	EXPECT_NO_THROW(hft_sub_transaction(h, kMonthCode));

	/* L1797-1983：五种切片的 NULL 支（reader 为空）。 */
	g_bars.clear();
	g_ticks.clear();
	EXPECT_EQ(hft_get_bars(h, kMonthCode, "m1", 5, bars_cb), 0U);
	EXPECT_EQ(hft_get_ticks(h, kMonthCode, 5, ticks_cb), 0U);
	EXPECT_EQ(hft_get_ordque(h, kMonthCode, 5, NULL), 0U);
	EXPECT_EQ(hft_get_orddtl(h, kMonthCode, 5, NULL), 0U);
	EXPECT_EQ(hft_get_trans(h, kMonthCode, 5, NULL), 0U);
	EXPECT_EQ(g_bars.calls, 0U);
	EXPECT_EQ(g_ticks.calls, 0U);

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:1247-1251：先 createExtParser（需要已配置 engine），再推一条
 * 注册合约的合法 tick，走 adapter->handleQuote -> engine->handle_push_quote。
 * ------------------------------------------------------------------------- */
TEST(R82PorterCapiReal, ExtParserQuoteSuccess)
{
	const std::string dir = prepare_dir("quote");
	write_real_fixtures(dir);
	ensure_cta_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	EXPECT_TRUE(create_ext_parser("r82_parser"));

	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	strncpy(tick.code, "au2406", sizeof(tick.code) - 1);
	strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
	tick.trading_date = 20240101;
	tick.action_date = 20240101;
	tick.action_time = 93000;
	tick.price = 100.0;
	tick.open = 100.0;
	tick.high = 100.0;
	tick.low = 100.0;

	/* ParserAdapter 要求 actiondate/tradingdate 非 0，且能查到合约。 */
	EXPECT_NO_THROW(parser_push_quote("r82_parser", &tick, 0));

	/* 未注册 id 的告警支（既有 lane 已覆盖，这里只做对照）。 */
	EXPECT_NO_THROW(parser_push_quote("r82_parser_absent", &tick, 0));

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:203-251 + 436-437 + 1137-1138
 * 扩展数据加载器的周期映射矩阵（含 Minute1 与未知周期）、session 事件的
 * EngineType default、日志回调转发。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, LoaderPeriodMatrixAndDispatchDefaults)
{
	const std::string dir = prepare_dir("loader");
	ASSERT_FALSE(dir.empty());

	WtRtRunner* r = new_heap_runner(dir);
	ASSERT_NE(r, nullptr);
	r->registerExtDataLoader(fnl_loader, raw_loader, fct_loader, NULL);

	g_fnl.clear();
	EXPECT_TRUE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_DAY, bar_sink));
	EXPECT_EQ(g_fnl.period, "d1");
	EXPECT_TRUE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_Minute1, bar_sink));
	EXPECT_EQ(g_fnl.period, "m1");
	EXPECT_TRUE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_Minute5, bar_sink));
	EXPECT_EQ(g_fnl.period, "m5");
	EXPECT_FALSE(r->loadFinalHisBars(NULL, "SHFE.au.2406", KP_Hour, bar_sink));
	EXPECT_EQ(g_fnl.calls, 3U);

	g_raw.clear();
	EXPECT_TRUE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_DAY, bar_sink));
	EXPECT_EQ(g_raw.period, "d1");
	EXPECT_TRUE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_Minute1, bar_sink));
	EXPECT_EQ(g_raw.period, "m1");
	EXPECT_TRUE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_Minute5, bar_sink));
	EXPECT_EQ(g_raw.period, "m5");
	EXPECT_FALSE(r->loadRawHisBars(NULL, "SHFE.au.2406", KP_Hour, bar_sink));
	EXPECT_EQ(g_raw.calls, 3U);

	/* L253-275：复权因子加载器对空合约与指定合约两路转发。 */
	g_fct.clear();
	EXPECT_TRUE(r->loadAllAdjFactors(NULL, fct_sink));
	EXPECT_EQ(g_fct.code, "");
	EXPECT_TRUE(r->loadAdjFactors(NULL, "SHFE.au.2406", fct_sink));
	EXPECT_EQ(g_fct.code, "SHFE.au.2406");
	EXPECT_EQ(g_fct.calls, 2U);

	/* L436-437：EngineType 取枚举外的值走 default。 */
	EXPECT_NO_THROW(r->ctx_on_session_event(1, 20240101, true, (EngineType)0));

	/* L1137-1138：notifier 未 init（_mq_sid==0）时只做 tag 取值后返回。 */
	EXPECT_NO_THROW(r->handleLogAppend(LL_INFO, "r82 log route"));

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:594-704 + 724 + 888 + 936 + 1068
 * config 各段的失败臂：parser 文件缺失；traders 文件为对象/坏 JSON；
 * executers 文件为对象/坏 JSON；data 段缺失；strategies 缺 cta。
 * 每个 runner 独立构造，互不共享 _config。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, ConfigFailureArmsAcrossSections)
{
	const std::string dir = prepare_dir("cfgerr");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);
	write_text(dir + "/bad.json", "{ this is not a json document");
	write_text(dir + "/traders_obj.json", "{\"traders\":{}}");
	write_text(dir + "/executers_obj.json", "{\"executers\":{}}");

	const std::string tail = "\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"";

	{
		/* L615-618：parser 配置文件不存在。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"parsers\":\"" + dir
			+ "/absent_parsers.json\"," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L630-647 + L1068：traders 文件内容不是数组 -> initTraders false。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"traders\":\""
			+ dir + "/traders_obj.json\"," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L643-646：traders 文件坏 JSON。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"traders\":\""
			+ dir + "/bad.json\"," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L678 + L936：executers 文件存在但 executers 段不是数组。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"executers\":\""
			+ dir + "/executers_obj.json\"," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L686-689：executers 文件坏 JSON。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"executers\":\""
			+ dir + "/bad.json\"," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L884-896：缺 data 段，config 不检查 initDataMgr 返回值（BUG-R69-003）。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"env\":{\"name\":\"cta\"},"
			"\"bspolicy\":\"" + dir + "/actions.json\"}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	{
		/* L716-724：strategies 是对象但没有 cta 数组。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{},"
			+ tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:791-814 + 838-854 + 884-896
 * HFT 模式：缺 hft 段；active 策略名不含 '.' 时 createStrategy 返回空指针并
 * continue；env 段缺失时 initEngine 返回 false（config 忽略返回值继续 true）。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, HftStrategyListAndEngineWithoutEnv)
{
	const std::string dir = prepare_dir("hftcfg");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	const std::string tail = "\"env\":{\"name\":\"hft\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"";

	{
		/* L797-799：strategies 对象缺 hft 数组。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"strategies\":{},"
			+ tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
		EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);
	}

	{
		/* L807-814：active 项 name 无 '.' -> HftStrategyMgr::createStrategy 为空。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},"
			"\"strategies\":{\"hft\":[{\"active\":true,\"id\":\"r82_hft_off\","
			"\"name\":\"nodot\"}]}," + tail + "}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
		EXPECT_NE(dynamic_cast<WtHftEngine*>(r->getEngine()), nullptr);
	}

	{
		/* L838-854：env 缺失 -> initEngine false，config 仍返回 true。
		 * 只断言返回值，不读未初始化的 _engine（BUG-R46-001/002）。 */
		WtRtRunner* r = new_heap_runner(dir);
		const std::string cfg = "{\"basefiles\":{},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\"}";
		EXPECT_TRUE(r->config(cfg.c_str(), false));
	}

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtRtRunner.cpp:933-1052
 * initExecuters 的 kind 分支与 trader 绑定失败支：空 name -> local；
 * local/diff/arbi 指向不存在的 trader；dist 走独立分支。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, ExecuterKindFallbacksAndMissingTraders)
{
	const std::string dir = prepare_dir("exec");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	WtRtRunner* r = new_heap_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg = "{\"basefiles\":{},"
		"\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"executers\":["
		"{\"active\":true,\"id\":\"r82_exec_noname\"},"
		"{\"active\":true,\"id\":\"r82_exec_local_badtrd\",\"name\":\"local\","
		"\"trader\":\"r82_absent_trader\"},"
		"{\"active\":true,\"id\":\"r82_exec_diff_badtrd\",\"name\":\"diff\","
		"\"trader\":\"r82_absent_trader\"},"
		"{\"active\":true,\"id\":\"r82_exec_arbi_badtrd\",\"name\":\"arbi\","
		"\"trader\":\"r82_absent_trader\"},"
		"{\"active\":true,\"id\":\"r82_exec_dist\",\"name\":\"dist\"},"
		"{\"active\":false,\"id\":\"r82_exec_skipped\"}"
		"]}";

	EXPECT_TRUE(r->config(cfg.c_str(), false));
	EXPECT_NE(r->getEngine(), nullptr);

	clean_dir(dir);
}

/* ---------------------------------------------------------------------------
 * WtPorter.cpp:286-289 + WtRtRunner.cpp:1141-1155
 * release_porter 转发到 runner.release：置退出标记、释放 parser/trader、
 * 释放当前引擎，最后 WTSLogger::stop。
 *
 * 本用例必须排在文件最后：WTSLogger::stop 会进程级静默日志（BUG-R6-006）。
 * ------------------------------------------------------------------------- */
TEST(R82RunnerEdges, ReleasePorterStopsConfiguredRunner)
{
	const std::string dir = prepare_dir("release");
	write_real_fixtures(dir);
	ensure_cta_configured(dir);
	ASSERT_NE(getRunner().getEngine(), nullptr);

	EXPECT_NO_THROW(release_porter());

	clean_dir(dir);
}
