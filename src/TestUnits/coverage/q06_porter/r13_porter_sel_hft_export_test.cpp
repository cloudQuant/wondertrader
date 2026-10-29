/*
 * R13 - WtPorter C 导出族：SEL 与 HFT 分支（真实句柄）
 *
 * 目标：src/WtPorter/WtPorter.cpp（missed=435/848）里 SEL 段（1120-1660）
 * 与 HFT 段（1650-2255）的「命中上下文」支；以及 create_sel_context /
 * create_hft_context 两个 C 入口（WtPorter.cpp:1135、:1652）。
 *
 * 已确认的安全边界（逐行读过实现）：
 *  - SelStraBaseCtx 的 stra_get_position/avgpx/position_profit/detail_cost/detail_profit
 *    （WtCore/SelStraBaseCtx.cpp:1144/1176/1196/1225/1244）在 _pos_map miss 时 return 0；
 *  - StraFundInfo 的构造函数 memset 全 0（SelStraBaseCtx.h:909-913、HftStraBaseCtx.h:790-794），
 *    所以资金族任意 flag 的取值都是精确 0.0；
 *  - _user_datas 是纯 map（SelStraBaseCtx.cpp:1066-1079）；
 *  - HFT 侧 trader-dependent 入口解引用 _trader；createHftContext 在拿不到交易通道时
 *    不设置也不初始化 _trader（HftStraBaseCtx.h:673 无默认初值，构造函数
 *    HftStraBaseCtx.cpp:41-48 也没列它）。本文件只在注册 dummy trader 的隔离子进程
 *    中验证需要绑定的 getter；API-dependent 入口不在此夹具的活动范围内，见 BUG-R13-001。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>
#include <stdlib.h>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	const CtxHandler kBadHandle = 0x7FFF0001UL;
	/* SEL fixture only uses its code as a map key; the HFT code is loaded by its isolated config. */
	const char* kSelCode = "SSE.STK.600000";
	const char* kHftCode = "SHFE.au.2406";

	struct PosRec
	{
		uint32_t	calls = 0;
		std::string	last_code;
		double		last_qty = -1;
		bool		last_islast = false;
		void clear() { *this = PosRec(); }
	};

	PosRec g_pos;

	void pos_cb(CtxHandler /*cHandle*/, const char* stdCode, double position, bool isLast)
	{
		g_pos.calls++;
		g_pos.last_code = (stdCode == NULL) ? "<null>" : stdCode;
		g_pos.last_qty = position;
		g_pos.last_islast = isLast;
	}

	bool write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		if (!ofs)
			return false;
		ofs << body;
		return ofs.good();
	}

	const char* kHftSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	const char* kHftCommodities =
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\","
		"\"pricetick\":0.02,\"volscale\":1000,\"covermode\":0,\"pricemode\":0,"
		"\"trademode\":0,\"category\":1}}}";

	const char* kHftContracts =
		"{\"SHFE\":{\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"}}}";

	const char* kHftRules =
		"{\"SHFE\":{\"au\":[{\"from\":\"au2406\",\"to\":\"au2406\",\"date\":20240101,"
		"\"oldclose\":100.0,\"newclose\":100.0}]}}";

	std::string hft_config(const std::string& dir, const std::string& traderId)
	{
		return "{\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\","
			"\"rules\":{\"R13\":\"" + dir + "/hot.json\"}},"
			"\"env\":{\"name\":\"hft\"},\"data\":{},"
			"\"bspolicy\":\"" + dir + "/actions.json\","
			"\"traders\":[{\"active\":true,\"id\":\"" + traderId + "\"}]}";
	}

	int fail_hft_probe(int code, const char* reason)
	{
		std::fprintf(stderr, "R13 HFT registered-trader probe failed: %s\n", reason);
		std::fflush(stderr);
		return code;
	}

	struct HftProbeDir
	{
		std::string path;
		bool preserve = false;

		bool cleanup(std::error_code& ec) const
		{
			if (preserve)
			{
				ec = std::make_error_code(std::errc::operation_canceled);
				return false;
			}
			if (path.empty())
			{
				ec.clear();
				return true;
			}
			if (!std::filesystem::exists(path, ec))
				return !ec;
			const std::filesystem::path target = std::filesystem::canonical(path, ec);
			if (ec)
				return false;
			const std::filesystem::path current = std::filesystem::current_path(ec);
			if (ec)
				return false;
			const std::filesystem::path cwd = std::filesystem::canonical(current, ec);
			if (ec)
				return false;
			for (std::filesystem::path p = cwd; !p.empty();)
			{
				if (p == target)
				{
					ec = std::make_error_code(std::errc::device_or_resource_busy);
					return false;
				}
				const std::filesystem::path parent = p.parent_path();
				if (parent == p)
					break;
				p = parent;
			}
			std::filesystem::remove_all(path, ec);
			return !ec;
		}

		~HftProbeDir()
		{
			std::error_code ec;
			if (!cleanup(ec))
			{
				std::fprintf(stderr, "R13 HFT probe cleanup failed or was skipped for %s: %s\n",
					path.c_str(), ec.message().c_str());
				std::fflush(stderr);
			}
		}
	};

	struct HftProbeCwd
	{
		std::filesystem::path original;
		HftProbeDir* temp = nullptr;
		bool changed = false;

		bool enter(const std::filesystem::path& target, HftProbeDir& tempDir, std::error_code& ec)
		{
			temp = &tempDir;
			original = std::filesystem::current_path(ec);
			if (ec)
				return false;
			std::filesystem::current_path(target, ec);
			if (ec)
				return false;
			changed = true;
			return true;
		}

		bool restore(std::error_code& ec)
		{
			if (!changed)
			{
				ec.clear();
				return true;
			}
			std::filesystem::current_path(original, ec);
			if (!ec)
				changed = false;
			else if (temp != nullptr)
				temp->preserve = true;
			return !ec;
		}

		~HftProbeCwd()
		{
			if (changed)
			{
				std::error_code ec;
				if (!restore(ec))
				{
					std::fprintf(stderr, "R13 HFT probe cwd restore failed: %s\n",
						ec.message().c_str());
					std::fflush(stderr);
				}
			}
		}
	};

	/*
	 * Configure the R82 id-only local trader seam in a threadsafe death-test child.
	 * initTraders registers the adapter object even though its empty module cannot
	 * initialize a trading API; createHftContext then binds that object. The
	 * hft_get_position probe only reads its empty in-memory position map. This does
	 * not establish that a real trading connection is ready.
	 */
	int run_registered_hft_getter_probe()
	{
		std::error_code ec;
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
		if (ec)
			return fail_hft_probe(2, ("could not locate temp directory: " + ec.message()).c_str());
		const std::filesystem::path tempPath = std::filesystem::absolute(tempRoot
			/ "r13_porter_hft_XXXXXX", ec);
		if (ec)
			return fail_hft_probe(2, ("could not resolve absolute temp directory: " + ec.message()).c_str());
		std::string dirTemplate = tempPath.string();
		char* createdDir = ::mkdtemp(dirTemplate.data());
		if (createdDir == nullptr)
			return fail_hft_probe(2, ("could not atomically create temp directory: "
				+ std::string(std::strerror(errno))).c_str());
		HftProbeDir temp{ createdDir };
		const std::string& dir = temp.path;

		HftProbeCwd cwd;
		if (!cwd.enter(dir, temp, ec))
			return fail_hft_probe(3, ("could not switch cwd to temp directory: " + ec.message()).c_str());

		const int probeResult = [&]() -> int
		{

		if (!write_text(dir + "/sessions.json", kHftSessions)
			|| !write_text(dir + "/commodities.json", kHftCommodities)
			|| !write_text(dir + "/contracts.json", kHftContracts)
			|| !write_text(dir + "/hot.json", kHftRules)
			|| !write_text(dir + "/actions.json", "{}"))
			return fail_hft_probe(4, "could not write temporary config files");

		const std::string traderId = "r13_registered_trader";
		getRunner().init((dir + "/r13_absent_logcfg.yaml").c_str(), true, dir.c_str());
		const std::string cfg = hft_config(dir, traderId);
		config_porter(cfg.c_str(), false);
		if (getRunner().getEngine() == NULL)
			return fail_hft_probe(5, "HFT engine config did not initialize");

		const CtxHandler h = create_hft_context("r13_hft_registered", traderId.c_str(), false, 1);
		if (h == 0 || getRunner().getHftContext(h) == NULL)
			return fail_hft_probe(6, "registered trader context was not created");

		/* This getter traverses _trader->getPosition; zero proves the empty adapter is bound. */
		if (hft_get_position(h, kHftCode, true) != 0.0)
			return fail_hft_probe(7, "empty registered adapter did not report zero position");
		if (hft_get_position_profit(h, kHftCode) != 0.0
			|| hft_get_position_avgpx(h, kHftCode) != 0.0
			|| hft_get_position_profit(kBadHandle, kHftCode) != 0.0)
			return fail_hft_probe(8, "map-only HFT getter returned an unexpected value");

		hft_save_userdata(h, "k1", "r13-hft-value");
		hft_save_userdata(h, "k2", "");
		if (std::strcmp(hft_load_userdata(h, "k1", "DEF"), "r13-hft-value") != 0
			|| std::strcmp(hft_load_userdata(h, "k2", "DEF"), "") != 0
			|| std::strcmp(hft_load_userdata(h, "absent", "DEF"), "DEF") != 0)
			return fail_hft_probe(9, "HFT userdata roundtrip failed");
		hft_save_userdata(h, "k1", "r13-hft-second");
		if (std::strcmp(hft_load_userdata(h, "k1", "DEF"), "r13-hft-second") != 0)
			return fail_hft_probe(10, "HFT userdata overwrite failed");

		hft_save_userdata(h, "state", "before");
		hft_log_text(h, LOG_LEVEL_DEBUG, "r13 hft debug");
		hft_log_text(h, LOG_LEVEL_INFO, "r13 hft info");
		hft_log_text(h, LOG_LEVEL_WARN, "r13 hft warn");
		hft_log_text(h, LOG_LEVEL_ERROR, "r13 hft error");
		hft_log_text(h, LOG_LEVEL_ERROR + 5, "r13 hft unmapped");
		if (std::strcmp(hft_load_userdata(h, "state", "AFTER"), "before") != 0)
			return fail_hft_probe(11, "HFT log changed userdata");

		return 0;
		}();

		if (!cwd.restore(ec))
			return fail_hft_probe(12, ("could not restore original cwd: " + ec.message()).c_str());
		if (!temp.cleanup(ec))
			return fail_hft_probe(13, ("could not safely clean temp directory: " + ec.message()).c_str());
		return probeResult;
	}

}

class R13PorterSelExport : public ::testing::Test
{
protected:
	CtxHandler	_h = 0;

	void SetUp() override
	{
		g_pos.clear();
		_h = create_sel_context("r13_sel_export", 20240601, 153000, "d",
			"CHINA", "TRADING", 2);
		ASSERT_NE(_h, 0U);
		ASSERT_TRUE(getRunner().getSelContext(_h) != NULL);
	}

	void TearDown() override { g_pos.clear(); }
};

/*
 * 这里只验证 runner 当前识别的 period token 会注册出可查找的独立上下文，
 * 不验证调度触发。WtPorter.h 的 d1/w1/m1 周期示例与 runner 当前识别的
 * d/w/m/y/min 写法不一致；仅检查句柄不能证明这些文档示例实际参与调度。
 */
TEST_F(R13PorterSelExport, create_sel_context_registers_contexts_for_supported_period_tokens)
{
	const char* periods[] = { "w", "m", "y", "min", "d" };
	CtxHandler prev = _h;
	for (const char* p : periods)
	{
		const CtxHandler h = create_sel_context("r13_sel_period", 20240602, 93000, p,
			"CHINA", "TRADING", 0);
		ASSERT_NE(h, 0U) << p;
		EXPECT_NE(h, prev) << p;
		EXPECT_TRUE(getRunner().getSelContext(h) != NULL) << p;
		prev = h;
	}

	// 非法句柄仍无法查找，作为有效上下文句柄的对照。
	EXPECT_TRUE(getRunner().getSelContext(kBadHandle) == NULL);
}

TEST_F(R13PorterSelExport, sel_userdata_roundtrips_exact_value_and_default)
{
	sel_save_userdata(_h, "k1", "r13-sel-value");
	sel_save_userdata(_h, "k2", "");
	EXPECT_STREQ(sel_load_userdata(_h, "k1", "DEF"), "r13-sel-value");
	EXPECT_STREQ(sel_load_userdata(_h, "k2", "DEF"), "");
	EXPECT_STREQ(sel_load_userdata(_h, "absent", "DEF"), "DEF");

	sel_save_userdata(_h, "k1", "r13-sel-second");
	EXPECT_STREQ(sel_load_userdata(_h, "k1", "DEF"), "r13-sel-second");
}

TEST_F(R13PorterSelExport, sel_virgin_position_getters_are_exactly_zero)
{
	EXPECT_DOUBLE_EQ(sel_get_position(_h, kSelCode, true, ""), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position(_h, kSelCode, false, "tag-a"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_profit(_h, kSelCode), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_position_avgpx(_h, kSelCode), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_cost(_h, kSelCode, "tag-a"), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_detail_profit(_h, kSelCode, "tag-a", 0), 0.0);
	EXPECT_EQ(sel_get_first_entertime(_h, kSelCode), 0ULL);
	// StraFundInfo 由 memset 归零，任意 flag 的组合都是 0
	EXPECT_DOUBLE_EQ(sel_get_fund_data(_h, 0), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(_h, 1), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_fund_data(_h, 2), 0.0);
}

/*
 * sel_get_all_position 的真分支：ctx->enum_position 只遍历 _pos_map/_sig_map
 * （SelStraBaseCtx.cpp:659-680，两个表都空 -> 0 次内层调用），
 * 随后 WtPorter.cpp:1258 补一次 ("",0,true) 终止回调。
 * 期望次数与内容由本用例自己喂的「空上下文」算出：恰好 1 次、isLast 为真。
 */
TEST_F(R13PorterSelExport, sel_all_position_emits_only_the_terminator_for_an_empty_context)
{
	sel_get_all_position(_h, pos_cb);
	EXPECT_EQ(g_pos.calls, 1U);
	EXPECT_EQ(g_pos.last_code, "");
	EXPECT_DOUBLE_EQ(g_pos.last_qty, 0.0);
	EXPECT_TRUE(g_pos.last_islast);
}

/*
 * sel_log_text 的四个级别 + default（WtPorter.cpp:1183-1206）。
 * 用「状态不变」来证伪：日志不许改用户数据表。
 */
TEST_F(R13PorterSelExport, sel_log_text_passes_all_levels_without_touching_state)
{
	sel_save_userdata(_h, "state", "before");
	sel_log_text(_h, LOG_LEVEL_DEBUG, "r13 sel debug");
	sel_log_text(_h, LOG_LEVEL_INFO, "r13 sel info");
	sel_log_text(_h, LOG_LEVEL_WARN, "r13 sel warn");
	sel_log_text(_h, LOG_LEVEL_ERROR, "r13 sel error");
	sel_log_text(_h, LOG_LEVEL_ERROR + 5, "r13 sel unmapped");
	EXPECT_STREQ(sel_load_userdata(_h, "state", "AFTER"), "before");
}

/* 崩溃面：sel_set_position/sel_sub_ticks 进 WtEngine::do_set_position 与 sub_tick，
 * 依赖 _data_mgr/_hot_mgr；本夹具没有（也拿不到）config() 成功的路径。 */
TEST_F(R13PorterSelExport, DISABLED_sel_mutation_and_subscription_need_engine_seam)
{
	sel_set_position(_h, kSelCode, 100, "r13");
	sel_sub_ticks(_h, kSelCode);
}

/*
 * The id-only adapter from R82 is registered without a trading API. hft_get_position
 * confirms _trader points to that object by reading its empty position map; this
 * does not prove a real trading connection is ready. The remaining getters inspect
 * only the HFT context's own maps. Run in a threadsafe death-test child so
 * configuring the process-global Porter runner cannot alter other q06 suites.
 *
 * Trader API-dependent order, cancellation and subscription calls remain outside
 * this fixture; BUG-R13-001 still tracks the production behavior when binding fails.
 */
TEST(R13PorterHftExport, map_getters_userdata_and_logging_use_registered_dummy_trader)
{
	const std::string oldStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(std::_Exit(run_registered_hft_getter_probe()),
		::testing::ExitedWithCode(0), ".*");
	GTEST_FLAG_SET(death_test_style, oldStyle);
}

/*
 * BUG-R13-001 regression candidate. Keep DISABLED_: a missing trader still
 * produces a context handle, while HftStraBaseCtx::_trader is not initialized
 * on the failed binding path. The calls below reach that unbound pointer and
 * must not be executed until the production defect is fixed and the test is
 * redesigned to assert the repaired contract.
 */
TEST(R13PorterHftExport, DISABLED_hft_trader_bound_entries_deref_unbound_pointer)
{
	const CtxHandler h = create_hft_context("r13_hft_unbound_candidate",
		"R13_TRADER_NOT_EXISTS", false, 1);
	ASSERT_NE(h, 0U);
	ASSERT_TRUE(getRunner().getHftContext(h) != NULL);

	hft_get_position(h, kHftCode, true);
	hft_get_undone(h, kHftCode);
	hft_cancel(h, 1);
	hft_cancel_all(h, kHftCode, true);
	hft_buy(h, kHftCode, 3500.0, 1, "r13", 0);
	hft_sell(h, kHftCode, 3500.0, 1, "r13", 0);
	hft_sub_ticks(h, kHftCode);
	hft_sub_order_queue(h, kHftCode);
	hft_sub_order_detail(h, kHftCode);
	hft_sub_transaction(h, kHftCode);
}

/*
 * 崩溃面（BUG-R6-003/Q06-002 同族）：cta/sel/hft 三族的 getEngine() 直取入口
 * （WtPorter.cpp:857/872/883/894/905、1215/1226/1237/1421、1743/1755/1767/1779）
 * 全部无条件解引用 getRunner().getEngine()，未 config 时是 NULL。
 * 这 14 个入口约 42 行在「进程内 + 不 config」的口径下取不到样。
 */
TEST(R13PorterEngineSeam, DISABLED_engine_direct_getters_deref_null_engine)
{
	EXPECT_DOUBLE_EQ(cta_get_price(kHftCode), 0.0);
	EXPECT_DOUBLE_EQ(sel_get_price(kSelCode), 0.0);
	EXPECT_DOUBLE_EQ(hft_get_price(kHftCode), 0.0);
}
