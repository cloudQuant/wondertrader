/*!
 * \file r45_runnerextra_test.cpp
 * \brief WtDtServo/WtDtRunner + WtDataManager 补测（迭代01 第十波 R45）
 *
 * 与同目录 q05/r8 用例互补，本文件只打 r8 之后仍缺的支路：
 *  - initialize 里 basefiles 的数组形态（commodity/contract 两个 VT_Array 循环）
 *  - 带真实 store 插件（libWtDataStorageAD.dylib，init 内不起线程）的
 *    WtDtRunner 查询入口：按数量/区间/日期/秒线/Tick 的全链路参数解析与委托
 *  - WtDataManager 侧对应的缓存装配入口（缓存首次装配遇到空库时提前返回）、
 *    subscribe_bar 的 times==1 / times>1 两条、clear_cache 的已初始化分支、
 *    get_exright_factor 的股票分支与热点规则因子分支
 *  - proc_tick 的「非分月平码」与「期货期权」两条标准化分支
 *  - trigger_tick 内部订阅表的 flag=1(qfq) / flag=2(hfq) 两条
 *
 * 接缝说明：WtDataManager 没有 reader 注入点，唯一合法的非线程 reader 是
 * libWtDataStorageAD.dylib（WtRdmDtReaderAD::init 只记录 path，不建线程，
 * 见 src/WtDataStorageAD/WtRdmDtReaderAD.cpp:60-74）。用例通过 dladdr 取
 * 测试可执行体所在目录的上一级（build_x64/Debug/bin/）拼出插件路径，
 * 并在 dlopen + dlsym 双符号确认后才调用 initStore；任一步不满足就 GTEST_SKIP，
 * 绝不让「插件缺失」退化成空指针对解引用（那是 BUG-Q05-001/BUG-R8-002）。
 * 所有取数都选在 AD 空库上「返回 NULL」的安全形态：
 * readTickSliceByDate / readTickSliceByCount / readKlineSliceByCount 都是 TODO
 * 直接 NULL；range 读在空库上会越界（BUG-R45-003），本文件一律不碰。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <filesystem>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- 录制回调
struct R45TickProbe
{
	static std::vector<std::string> codes;
	static std::vector<double> prices;
};
std::vector<std::string> R45TickProbe::codes;
std::vector<double> R45TickProbe::prices;

struct R45BarProbe
{
	static std::vector<std::string> codes;
	static std::vector<std::string> periods;
};
std::vector<std::string> R45BarProbe::codes;
std::vector<std::string> R45BarProbe::periods;

void r45_on_tick(const char* stdCode, WTSTickStruct* tick)
{
	R45TickProbe::codes.emplace_back(stdCode == nullptr ? "" : stdCode);
	R45TickProbe::prices.push_back(tick == nullptr ? 0.0 : tick->price);
}

void r45_on_bar(const char* stdCode, const char* period, WTSBarStruct* /*bar*/)
{
	R45BarProbe::codes.emplace_back(stdCode == nullptr ? "" : stdCode);
	R45BarProbe::periods.emplace_back(period == nullptr ? "" : period);
}

fs::path r45_make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r45_write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

/*!
 * 由测试可执行体自身路径推模块目录：<bin>/TestUnits/CoverageTests_dtservo
 * -> <bin>/（产品插件都落在这里）。
 */
std::string r45_bin_dir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&r45_bin_dir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();

	fs::path bin = fs::path(info.dli_fname).parent_path().parent_path();
	std::string s = bin.string();
	if (!s.empty() && s.back() != '/')
		s.push_back('/');
	return s;
}

/*!
 * 只有在 dylib 存在且导出 create/delete 两个符号时才认为可以交给 initStore，
 * 否则 initStore 会走「加载失败仍解引用 _reader」的崩溃路径（已登记缺陷）。
 */
bool r45_ad_store_ready(const std::string& binDir)
{
	if (binDir.empty())
		return false;

	const std::string path = binDir + "libWtDataStorageAD.dylib";
	std::error_code ec;
	if (!fs::exists(path, ec))
		return false;

	void* h = dlopen(path.c_str(), RTLD_NOW);
	if (h == nullptr)
		return false;

	const bool ok = dlsym(h, "createRdmDtReader") != nullptr
		&& dlsym(h, "deleteRdmDtReader") != nullptr;
	dlclose(h);
	return ok;
}

WTSTickData* r45_make_tick(const char* code, const char* exchg, double price = 100.0)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.code, code);
	wt_strcpy(ts.exchg, exchg);
	ts.price = price;
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	return WTSTickData::create(ts);
}

class R45RunnerExtraTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r45_make_tmp_dir("r45_runnerextra");
		_bin = r45_bin_dir();
		_storeReady = r45_ad_store_ready(_bin);

		R45TickProbe::codes.clear();
		R45TickProbe::prices.clear();
		R45BarProbe::codes.clear();
		R45BarProbe::periods.clear();

		const std::string session = R"JSON({"SD0930":{"name":"r45 day","offset":0,
			"auction":{"from":929,"to":930},
			"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}})JSON";
		r45_write_file(_dir / "sessions.json", session);
		r45_write_file(_dir / "holidays.json", R"JSON({"CHINA":["20240101","20240212"]})JSON");
		// oldclose/newclose = 100/80 => 复权因子 1.25（WTSHotMgr.cpp:257）
		r45_write_file(_dir / "hots.json",
			R"JSON({"CFFEX":{"IC":[{"date":20240101,"from":"IC2203","newclose":80.0,
				"oldclose":100.0,"to":"IC2204"}]}})JSON");
		r45_write_file(_dir / "seconds.json",
			R"JSON({"CFFEX":{"IC":[{"date":20240101,"from":"IC2203","newclose":80.0,
				"oldclose":100.0,"to":"IC2204"}]}})JSON");

		const std::string commIC = R"JSON({"CFFEX":{"IC":{"covermode":0,"pricemode":0,
			"category":1,"precision":1,"pricetick":0.2,"volscale":200,"name":"CSI500",
			"exchg":"CFFEX","session":"SD0930","holiday":"CHINA"}}})JSON";
		// category=2 => CC_FutOption（WTSTypes.h:24）
		const std::string commIO = R"JSON({"CFFEX":{"IO":{"covermode":0,"pricemode":0,
			"category":2,"precision":1,"pricetick":0.2,"volscale":100,"name":"CSI500OPT",
			"exchg":"CFFEX","session":"SD0930","holiday":"CHINA"}}})JSON";
		// category=0 => CC_Stock（WTSTypes.h:22），isStock() 为真
		const std::string commSSE = R"JSON({"SSE":{"600000":{"covermode":0,"pricemode":0,
			"category":0,"precision":2,"pricetick":0.01,"volscale":100,"name":"SPDB",
			"exchg":"SSE","session":"SD0930","holiday":"CHINA"}}})JSON";
		r45_write_file(_dir / "comm_ic.json", commIC);
		r45_write_file(_dir / "comm_io.json", commIO);
		r45_write_file(_dir / "comm_sse.json", commSSE);

		const std::string ctrIC = R"JSON({"CFFEX":{"IC2204":{"name":"CSI500 2204",
			"code":"IC2204","exchg":"CFFEX","product":"IC","maxlimitqty":20,"maxmarketqty":10}}})JSON";
		const std::string ctrIO = R"JSON({"CFFEX":{"IO2404-C-4000":{"name":"CSI500 2204 C4000",
			"code":"IO2404-C-4000","exchg":"CFFEX","product":"IO",
			"maxlimitqty":20,"maxmarketqty":10}}})JSON";
		const std::string ctrSSE = R"JSON({"SSE":{"600000":{"name":"SPDB",
			"code":"600000","exchg":"SSE","product":"600000",
			"maxlimitqty":100000,"maxmarketqty":1000}}})JSON";
		r45_write_file(_dir / "ctr_ic.json", ctrIC);
		r45_write_file(_dir / "ctr_io.json", ctrIO);
		r45_write_file(_dir / "ctr_sse.json", ctrSSE);

		// 合并形态（非数组）
		r45_write_file(_dir / "commodities.json",
			std::string("{") + R"("CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,)"
			+ R"("precision":1,"pricetick":0.2,"volscale":200,"name":"CSI500","exchg":"CFFEX",)"
			+ R"("session":"SD0930","holiday":"CHINA"},"IO":{"covermode":0,"pricemode":0,)"
			+ R"("category":2,"precision":1,"pricetick":0.2,"volscale":100,"name":"CSI500OPT",)"
			+ R"("exchg":"CFFEX","session":"SD0930","holiday":"CHINA"}},)"
			+ R"("SSE":{"600000":{"covermode":0,"pricemode":0,"category":0,"precision":2,)"
			+ R"("pricetick":0.01,"volscale":100,"name":"SPDB","exchg":"SSE",)"
			+ R"("session":"SD0930","holiday":"CHINA"}}})");
		r45_write_file(_dir / "contracts.json",
			std::string("{") + R"("CFFEX":{"IC2204":{"name":"CSI500 2204","code":"IC2204",)"
			+ R"("exchg":"CFFEX","product":"IC","maxlimitqty":20,"maxmarketqty":10},)"
			+ R"("IO2404-C-4000":{"name":"CSI500 2204 C4000","code":"IO2404-C-4000",)"
			+ R"("exchg":"CFFEX","product":"IO","maxlimitqty":20,"maxmarketqty":10}},)"
			+ R"("SSE":{"600000":{"name":"SPDB","code":"600000","exchg":"SSE",)"
			+ R"("product":"600000","maxlimitqty":100000,"maxmarketqty":1000}}})");

		_logCfg = (_dir / "no_such_logcfg.yaml").string();
		_modDir = _bin;
		if (!_bin.empty())
			WtHelper::set_module_dir(_bin.c_str());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	/*!
	 * withStore=true 时 data 段带 libWtDataStorageAD 的 store；
	 * arrayForms=true 时 commodity/contract 走 VT_Array 形态。
	 */
	std::string buildCfg(bool withStore, bool arrayForms)
	{
		const std::string d = _dir.string();
		std::string s = "{\"disable_dump\":true,\"basefiles\":{";
		s += "\"session\":\"" + d + "/sessions.json\",";
		if (arrayForms)
		{
			s += "\"commodity\":[\"" + d + "/comm_ic.json\",\"" + d
				+ "/comm_sse.json\",\"" + d + "/comm_io.json\"],";
			s += "\"contract\":[\"" + d + "/ctr_ic.json\",\"" + d
				+ "/ctr_sse.json\",\"" + d + "/ctr_io.json\"],";
		}
		else
		{
			s += "\"commodity\":\"" + d + "/commodities.json\",";
			s += "\"contract\":\"" + d + "/contracts.json\",";
		}
		s += "\"holiday\":\"" + d + "/holidays.json\",";
		s += "\"hot\":\"" + d + "/hots.json\",";
		s += "\"second\":\"" + d + "/seconds.json\"},";
		s += "\"data\":{\"align_by_section\":false";
		if (withStore)
		{
			s += ",\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\"" + d + "/store/\"}";
		}
		s += "}}";
		return s;
	}

	void initRunner(WtDtRunner& runner, bool withStore, bool arrayForms = false)
	{
		r45_write_file(_dir / "wtdt.json", buildCfg(withStore, arrayForms));
		runner.initialize((_dir / "wtdt.json").string().c_str(), true,
			_modDir.c_str(), _logCfg.c_str(), r45_on_tick, r45_on_bar);
	}

	WTSVariant* loadCfg(const std::string& body, const char* name)
	{
		r45_write_file(_dir / name, body);
		return WTSCfgLoader::load_from_file((_dir / name).string().c_str());
	}

	fs::path _dir;
	std::string _bin;
	std::string _modDir;
	std::string _logCfg;
	bool _storeReady = false;
};

// ---------------------------------------------------------------------------
// initialize：basefiles 数组形态
// ---------------------------------------------------------------------------

TEST_F(R45RunnerExtraTest, ArrayCommodityAndContractFormsAreLoaded)
{
	WtDtRunner runner;
	// WtDtRunner.cpp:87-93 / :103-109 两个 VT_Array 循环
	initRunner(runner, false, true);

	WTSBaseDataMgr& bdm = runner.getBaseDataMgr();
	EXPECT_NE(bdm.getCommodity("CFFEX", "IC"), nullptr);
	EXPECT_NE(bdm.getCommodity("SSE", "600000"), nullptr);
	EXPECT_NE(bdm.getContract("IC2204", "CFFEX"), nullptr);
	EXPECT_NE(bdm.getContract("600000", "SSE"), nullptr);
	// 这条也证明 initialize 跑到了 :189 的 _is_inited=true（守卫不再拦截）
	WTSKlineSlice* slice = runner.get_bars_by_date("CFFEX.IC.2204", "d1", 20240102);
	EXPECT_EQ(slice, nullptr);
}

// ---------------------------------------------------------------------------
// 带真实 store 插件的查询委托
// ---------------------------------------------------------------------------

TEST_F(R45RunnerExtraTest, CountQueriesDelegateToStoreReader)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	// WtDtRunner.cpp:319-348 周期解析（m10 -> KP_Minute5 且 realTimes=2），
	// 委托到 WtDataManager.cpp:337-393：空库 readKlineSliceByCount 返回 NULL -> :392
	EXPECT_EQ(runner.get_bars_by_count("CFFEX.IC.2204", "m10", 5, 202401021500), nullptr);
	// m5 -> realTimes==1，走 WtDataManager.cpp:339-342 的基础周期直读（同样空库 NULL）
	EXPECT_EQ(runner.get_bars_by_count("CFFEX.IC.2204", "m5", 5), nullptr);
	// 非 5 倍数的 m2 -> KP_Minute1 且 realTimes=2（WtDtRunner.cpp:327-338）
	EXPECT_EQ(runner.get_bars_by_count("CFFEX.IC.2204", "m2", 5), nullptr);
	EXPECT_TRUE(R45TickProbe::codes.empty());
}

TEST_F(R45RunnerExtraTest, RangeAndDateQueriesHitCacheAssemblyGuard)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	// WtDtRunner.cpp:210-239 -> WtDataManager.cpp:199-243 缓存首次装配，
	// 空库时 rawData==NULL 在 :241 提前返回
	EXPECT_EQ(runner.get_bars_by_range("CFFEX.IC.2204", "m10",
		202401020930, 202401021500), nullptr);
	// WtDtRunner.cpp:250-281 -> WtDataManager.cpp:191-197 用交易日边界 + 区间委托
	EXPECT_EQ(runner.get_bars_by_date("CFFEX.IC.2204", "m10", 20240102), nullptr);
	EXPECT_TRUE(R45BarProbe::codes.empty());
}

TEST_F(R45RunnerExtraTest, TickAndSecondQueriesUseStoreReader)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	// WtDtRunner.cpp:300-308 -> WtDataManager.cpp:119-122（AD readTickSliceByDate 为 TODO NULL）
	EXPECT_EQ(runner.get_ticks_by_date("CFFEX.IC.2204", 20240102), nullptr);
	// WtDtRunner.cpp:351-364 -> WtDataManager.cpp:484-488（readTickSliceByCount 为 TODO NULL）
	EXPECT_EQ(runner.get_ticks_by_count("CFFEX.IC.2204", 10, 202401021500), nullptr);
	// WtDtRunner.cpp:367-375 -> WtDataManager.cpp:158-181（秒线：Tick 为空即返回 NULL）
	EXPECT_EQ(runner.get_sbars_by_date("CFFEX.IC.2204", 30, 20240102), nullptr);
}

TEST_F(R45RunnerExtraTest, SubscribeBarCoversBaseAndResampledTimes)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	// WtDtRunner.cpp:607-635 -> WtDataManager.cpp:507-519：times==1 时空库直接返回
	runner.sub_bar("CFFEX.IC.2204", "m1");
	// 同链路 times>1：WtDataManager.cpp:539-557，空库只落一条 subscribed 日志
	runner.sub_bar("CFFEX.IC.2204", "m10");
	// sub_bar 只做内部订阅，不应触发外部 Tick 回调
	EXPECT_TRUE(R45TickProbe::codes.empty());
}

TEST_F(R45RunnerExtraTest, ClearCacheUsesInitializedReaderBranch)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	// WtDtRunner.cpp:645-648 -> WtDataManager.cpp:607-616：_reader!=NULL 时
	// 调用 clearCache（AD 用 IRdmDtReader 默认空实现）并打印 "All cache cleared"
	EXPECT_NO_THROW(runner.clear_cache());
	EXPECT_EQ(runner.get_ticks_by_date("CFFEX.IC.2204", 20240102), nullptr);
}

TEST_F(R45RunnerExtraTest, OrderFlowSliceQueriesUseStoreReader)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	const std::string d = _dir.string();
	WTSVariant* dataCfg = loadCfg(
		"{\"align_by_section\":false,\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\""
		+ d + "/orderstore/\"}}", "ordercfg.json");
	ASSERT_NE(dataCfg, nullptr);

	WtDataManager dm;
	ASSERT_TRUE(dm.init(dataCfg, &runner));

	// WtDataManager.cpp:124-129 / :131-136 / :138-143：
	// 订单队列、订单明细、成交明细三条 level2 取数入口；
	// AD reader 的三个 read*SliceByRange 均为直接 NULL（WtRdmDtReaderAD.h:68/78/88）
	EXPECT_EQ(dm.get_order_queue_slice("CFFEX.IC.2204", 202401020930, 202401021500), nullptr);
	EXPECT_EQ(dm.get_order_detail_slice("CFFEX.IC.2204", 202401020930, 202401021500), nullptr);
	EXPECT_EQ(dm.get_transaction_slice("CFFEX.IC.2204", 202401020930, 202401021500), nullptr);

	dataCfg->release();
}

TEST_F(R45RunnerExtraTest, ExrightFactorCoversStockAndHotRuleBranches)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);

	WTSCommodityInfo* icComm = runner.getBaseDataMgr().getCommodity("CFFEX", "IC");
	WTSCommodityInfo* stkComm = runner.getBaseDataMgr().getCommodity("SSE", "600000");
	ASSERT_NE(icComm, nullptr);
	ASSERT_NE(stkComm, nullptr);
	ASSERT_TRUE(stkComm->isStock());

	const std::string d = _dir.string();
	WTSVariant* dataCfg = loadCfg(
		"{\"align_by_section\":false,\"store\":{\"module\":\"WtDataStorageAD\",\"path\":\""
		+ d + "/dmstore/\"}}", "datacfg.json");
	ASSERT_NE(dataCfg, nullptr);

	WtDataManager dm;
	ASSERT_TRUE(dm.init(dataCfg, &runner));

	// WtDataManager.cpp:495-496 股票分支 -> _reader->getAdjFactorByDate（默认 1.0）
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("SSE.600000", stkComm), 1.0);
	// WtDataManager.cpp:499-501 非股票 + "HOT" 规则：100/80 = 1.25
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("CFFEX.IC.HOT", icComm), 1.25);
	// WtDataManager.cpp:492-493 commInfo==NULL 的基准分支
	EXPECT_DOUBLE_EQ(dm.get_exright_factor("CFFEX.IC.2204"), 1.0);

	dataCfg->release();
}

// ---------------------------------------------------------------------------
// proc_tick：平码 / 期货期权
// ---------------------------------------------------------------------------

TEST_F(R45RunnerExtraTest, ProcTickNormalizesFlatStockCode)
{
	WtDtRunner runner;
	initRunner(runner, false);

	// 订阅表用标准化后的平码
	runner.sub_tick("SSE.600000", true, false);
	WTSTickData* tick = r45_make_tick("600000", "SSE", 12.5);
	ASSERT_NE(tick, nullptr);

	// WtDtRunner.cpp:437-441 非分月码 -> rawFlatCodeToStdCode -> "SSE.600000"
	runner.proc_tick(tick);
	ASSERT_EQ(R45TickProbe::codes.size(), 1u);
	EXPECT_EQ(R45TickProbe::codes[0], "SSE.600000");
	EXPECT_DOUBLE_EQ(R45TickProbe::prices[0], 12.5);
	EXPECT_STREQ(tick->code(), "SSE.600000");
	tick->release();
}

TEST_F(R45RunnerExtraTest, ProcTickNormalizesFutOptionCode)
{
	WtDtRunner runner;
	initRunner(runner, false);

	// WtDtRunner.cpp:428-431 CC_FutOption -> rawFutOptCodeToStdCode
	// "CFFEX.IO2404-C-4000" 标准化为 "CFFEX.IO2404.C.4000"（CodeHelper.hpp:443-454）
	runner.sub_tick("CFFEX.IO2404.C.4000", true, false);
	WTSTickData* tick = r45_make_tick("IO2404-C-4000", "CFFEX", 66.0);
	ASSERT_NE(tick, nullptr);

	runner.proc_tick(tick);
	ASSERT_EQ(R45TickProbe::codes.size(), 1u);
	EXPECT_EQ(R45TickProbe::codes[0], "CFFEX.IO2404.C.4000");
	EXPECT_STREQ(tick->code(), "CFFEX.IO2404.C.4000");
	tick->release();
}

// ---------------------------------------------------------------------------
// trigger_tick 内部订阅的复权后缀
// ---------------------------------------------------------------------------

TEST_F(R45RunnerExtraTest, InnerSubscriptionWithSuffixBranchesStaySilent)
{
	WtDtRunner runner;
	initRunner(runner, false);

	// WtDtRunner.cpp:557-578 内部表 + 后缀解析（'-' => flag 1，'+' => flag 2）
	runner.sub_tick("CFFEX.IC.2204-", true, true);
	WTSTickData* qfqTick = r45_make_tick("IC2204", "CFFEX", 100.0);
	ASSERT_NE(qfqTick, nullptr);
	// WtDtRunner.cpp:516-530：flag==1 直接把当前 tick 的 code 改成 qfq 码，
	// 再交给 update_bars；_rt_bars 为 NULL（WtDataManager.cpp:569-570）立即返回
	runner.proc_tick(qfqTick);
	EXPECT_STREQ(qfqTick->code(), "CFFEX.IC.2204-");
	qfqTick->release();

	runner.sub_tick("CFFEX.IC.2204+", false, true);
	WTSTickData* hfqTick = r45_make_tick("IC2204", "CFFEX", 100.0);
	ASSERT_NE(hfqTick, nullptr);
	// WtDtRunner.cpp:531-551：flag==2 复制 tick、乘复权因子（1.0）后更新缓存
	runner.proc_tick(hfqTick);
	EXPECT_STREQ(hfqTick->code(), "CFFEX.IC.2204+");
	EXPECT_DOUBLE_EQ(hfqTick->getTickStruct().price, 100.0);
	hfqTick->release();

	// 内部订阅不产生外部回调
	EXPECT_TRUE(R45TickProbe::codes.empty());
	EXPECT_TRUE(R45BarProbe::codes.empty());
}

/*!
 * 触发条件（启用即崩溃，禁止在 CI 启用）：
 * store 是空库时，WtDataManager::get_kline_slice_by_range 的 times==1 分支
 * （WtDataManager.cpp:201-204）直接交给 WtRdmDtReaderAD::readKlineSliceByRange，
 * 后者在空向量上取 &barsList._bars[0]（WtDataStorageAD/WtRdmDtReaderAD.cpp:353-388），
 * 对空 vector 的 operator[] 是 UB，实测表现为段错误。同理 times==1 的 tick 区间读
 * 走 WtRdmDtReaderAD.cpp:216-249。见 BUG-R45-003。
 */
TEST_F(R45RunnerExtraTest, DISABLED_EmptyStoreSinglePeriodRangeReadSegfaults)
{
	if (!_storeReady)
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createRdmDtReader 不可用";

	WtDtRunner runner;
	initRunner(runner, true);
	// "m5" -> realTimes==1 -> 基础周期直读；空库上应返回 NULL，实际会崩
	EXPECT_EQ(runner.get_bars_by_range("CFFEX.IC.2204", "m5",
		202401020930, 202401021500), nullptr);
}

}  // namespace
