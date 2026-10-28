/*!
 * \file r8_wtdtrunner_test.cpp
 * \brief WtDtServo/WtDtRunner 覆盖用例（迭代01 第七波 R8）
 *
 * 覆盖目标（src/WtDtServo/WtDtRunner.cpp）：
 *  - 未初始化时 7 个查询入口的守卫分支（:204-207 / :244-247 / :286-289 / :302-305 /
 *    :313-316 / :353-356 / :369-372）
 *  - initialize 的配置加载失败早退（:58-64）、disable_dump 分支（:66）、
 *    basefiles 六段装载（:73-128）、热点码回写（:130-148）、
 *    无 data 段时 initDataMgr 早退（:192-195）、无 parsers 段告警（:182-183）、
 *    重复 initialize 拒绝（:46-50）
 *  - proc_tick 的合约查找 / 分月码标准化 / 热点转发（:410-459）
 *  - trigger_tick 的 flag=0 / flag=1(qfq) / flag=2(hfq 复制并复权) 三条外发分支
 *    与内部订阅表分支（:461-555）
 *  - sub_tick 的外/内两张表与 bReplace（:557-605）
 *  - trigger_bar 有无回调两条（:637-643）、clear_cache 转发（:645-648）
 * 观察手段：initialize 传入的两个录制回调（PorterDefs.h:62 / :72），
 * 因此所有断言都可证伪（回调未触发即失败）。
 * 无 data.store 段 -> _reader 恒为 NULL，因此所有会解引用 _reader 的入口
 * （get_bars_by_range / get_bars_by_count / get_ticks_* / sub_bar）
 * 只留 DISABLED_ 用例，见 bugs/R8_wtdtservo.md。不起线程、不加载插件、不碰网络。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include <filesystem>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <Includes/WTSVariant.hpp>
#include <Share/CodeHelper.hpp>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- 录制替身
struct TickProbe
{
	static std::vector<std::string> codes;
	static std::vector<double> prices;
};
std::vector<std::string> TickProbe::codes;
std::vector<double> TickProbe::prices;

struct BarProbe
{
	static std::vector<std::string> codes;
	static std::vector<std::string> periods;
	static std::vector<double> closes;
};
std::vector<std::string> BarProbe::codes;
std::vector<std::string> BarProbe::periods;
std::vector<double> BarProbe::closes;

void onTickProbe(const char* stdCode, WTSTickStruct* tick)
{
	TickProbe::codes.emplace_back(stdCode == nullptr ? "" : stdCode);
	TickProbe::prices.push_back(tick == nullptr ? 0.0 : tick->price);
}

void onBarProbe(const char* stdCode, const char* period, WTSBarStruct* bar)
{
	BarProbe::codes.emplace_back(stdCode == nullptr ? "" : stdCode);
	BarProbe::periods.emplace_back(period == nullptr ? "" : period);
	BarProbe::closes.push_back(bar == nullptr ? 0.0 : bar->close);
}

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

class R8WtDtRunnerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r8_runner");
		TickProbe::codes.clear();
		TickProbe::prices.clear();
		BarProbe::codes.clear();
		BarProbe::periods.clear();
		BarProbe::closes.clear();

		write_file(_dir / "sessions.json",
			R"JSON({"SD0930":{"name":"股指日盘","offset":0,"auction":{"from":929,"to":930},
				"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}})JSON");
		write_file(_dir / "commodities.json",
			R"JSON({"CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,"precision":1,
				"pricetick":0.2,"volscale":200,"name":"中证500","exchg":"CFFEX",
				"session":"SD0930","holiday":"CHINA"}}})JSON");
		write_file(_dir / "contracts.json",
			R"JSON({"CFFEX":{"IC2204":{"name":"中证500 2204","code":"IC2204","exchg":"CFFEX",
				"product":"IC","maxlimitqty":20,"maxmarketqty":10}}})JSON");
		write_file(_dir / "holidays.json", R"JSON({"CHINA":["20240101","20240212"]})JSON");
		// 最后一次切换的 "to" 决定 m_mapCustCodes["HOT"] = {CFFEX.IC2204}
		// (WTSHotMgr.cpp:263-264)，oldclose==newclose => 复权因子恒为 1.0 (:257)
		write_file(_dir / "hots.json",
			R"JSON({"CFFEX":{"IC":[{"date":20240101,"from":"IC2203","newclose":100.0,
				"oldclose":100.0,"to":"IC2204"}]}})JSON");
		write_file(_dir / "seconds.json",
			R"JSON({"CFFEX":{"IC":[{"date":20240101,"from":"IC2203","newclose":100.0,
				"oldclose":100.0,"to":"IC2204"}]}})JSON");

		_rootCfg = buildCfg(true);
		write_file(_dir / "wtdt.json", _rootCfg);
		// 不存在的日志配置 -> WTSLogger::init 在 WTSLogger.cpp:209-210 直接返回，
		// 不落地任何日志文件，也不起异步线程
		_logCfg = (_dir / "no_such_logcfg.yaml").string();
		_modDir = (_dir / "bin/").string();
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	static void write_file(const fs::path& p, const std::string& content)
	{
		std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
		ofs << content;
		ofs.close();
	}

	/*!
	 * 组装根配置：withData=false 时不含 "data" 段（覆盖 initDataMgr 的 NULL 早退），
	 * extraMember 用于追加 "parsers" 段；三种形态（无 parsers / 数组 / 外部文件）
	 * 分别覆盖 WtDtRunner.cpp:152-183 的不同分支
	 */
	std::string buildCfg(bool withData, const std::string& extraMember = "")
	{
		const std::string d = _dir.string();
		std::string s = std::string("{")
			+ "\"disable_dump\": true,"
			+ "\"basefiles\": {"
			+ "\"session\": \"" + d + "/sessions.json\","
			+ "\"commodity\": \"" + d + "/commodities.json\","
			+ "\"contract\": \"" + d + "/contracts.json\"";
		if (withData)
		{
			s += ",\"holiday\": \"" + d + "/holidays.json\"";
			s += ",\"hot\": \"" + d + "/hots.json\"";
			s += ",\"second\": \"" + d + "/seconds.json\"";
			s += "},\"data\": {\"align_by_section\": false}";
		}
		else
			s += "}";
		if (!extraMember.empty())
			s += "," + extraMember;
		s += "}";
		return s;
	}

	void initRunner(WtDtRunner& runner)
	{
		runner.initialize((_dir / "wtdt.json").string().c_str(), true,
			_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);
	}

	fs::path _dir;
	std::string _rootCfg;
	std::string _logCfg;
	std::string _modDir;
};

/*!
 * 造一条 CFFEX / IC2204 的 tick，日期已填好（proc_tick 不会因日期为 0 而丢）
 */
WTSTickData* make_ic_tick()
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.code, "IC2204");
	wt_strcpy(ts.exchg, "CFFEX");
	ts.price = 100.0;
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	return WTSTickData::create(ts);
}

// ---------------------------------------------------------------- 未初始化守卫

TEST_F(R8WtDtRunnerTest, UninitializedRunnerGuardsEveryQueryEntry)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());

	// 7 个入口都应在 "WtDtServo not initialized" 分支返回 NULL
	EXPECT_EQ(runner->get_bars_by_range("CFFEX.IC.2204", "m1", 202401020930, 202401021500), nullptr);
	EXPECT_EQ(runner->get_bars_by_date("CFFEX.IC.2204", "m1", 20240102), nullptr);
	EXPECT_EQ(runner->get_bars_by_count("CFFEX.IC.2204", "m1", 10), nullptr);
	EXPECT_EQ(runner->get_ticks_by_range("CFFEX.IC.2204", 202401020930, 202401021500), nullptr);
	EXPECT_EQ(runner->get_ticks_by_date("CFFEX.IC.2204", 20240102), nullptr);
	EXPECT_EQ(runner->get_ticks_by_count("CFFEX.IC.2204", 10), nullptr);
	EXPECT_EQ(runner->get_sbars_by_date("CFFEX.IC.2204", 30, 20240102), nullptr);

	// start 只遍历空解析器表；clear_cache 走 _reader==NULL 的取消分支
	runner->start();
	runner->clear_cache();
	EXPECT_TRUE(TickProbe::codes.empty());
}

TEST_F(R8WtDtRunnerTest, InitializeWithMissingConfigFileStopsBeforeBasefiles)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	// WtDtRunner.cpp:58-64  文件不存在 -> load_from_file 返回 NULL -> 早退，_is_inited 仍 false
	runner->initialize((_dir / "no_such_cfg.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);

	// 守卫分支仍在（若 _is_inited 被误置 true，get_ticks_by_date 会解引用 NULL _reader 而崩）
	EXPECT_EQ(runner->get_ticks_by_date("CFFEX.IC.2204", 20240102), nullptr);
	EXPECT_EQ(runner->get_bars_by_date("CFFEX.IC.2204", "d1", 20240102), nullptr);

	// 回调已绑定（:52-53 在配置装载之前），但基础数据为空 -> proc_tick 在 :419 早退
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	EXPECT_TRUE(TickProbe::codes.empty());
	tick->release();
}

// ---------------------------------------------------------------- 全量 initialize

TEST_F(R8WtDtRunnerTest, InitializedRunnerForwardsStdAndHotTick)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	runner->sub_tick("CFFEX.IC.2204,CFFEX.IC.HOT", true, false);

	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);

	// proc_tick 命中合约 -> 合约信息回写 + 分月码标准化
	// CodeHelper::rawMonthCodeToStdCode("IC2204","CFFEX") == "CFFEX.IC.2204"（CodeHelper.hpp:303-332）
	ASSERT_EQ(TickProbe::codes.size(), 2u);
	EXPECT_EQ(TickProbe::codes[0], "CFFEX.IC.2204");
	EXPECT_EQ(std::string(tick->code()), "CFFEX.IC.2204");
	EXPECT_NE(tick->getContractInfo(), nullptr);
	// 第二条来自热点码：initialize 的 :142-145 用 getFullPid()+"."+tag 生成 "CFFEX.IC.HOT"
	EXPECT_EQ(TickProbe::codes[1], "CFFEX.IC.HOT");
	EXPECT_DOUBLE_EQ(TickProbe::prices[0], 100.0);
	EXPECT_DOUBLE_EQ(TickProbe::prices[1], 100.0);
	tick->release();

	// 已初始化后，get_bars_by_date 的非分钟周期分支在解引用 _reader 之前就返回 NULL（:270-274）
	EXPECT_EQ(runner->get_bars_by_date("CFFEX.IC.2204", "d1", 20240102), nullptr);
	runner->clear_cache();
}

TEST_F(R8WtDtRunnerTest, QfqFlagSubscribesWrappedCodeAndReusesSameTick)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	// SUFFIX_QFQ == '-'（CodeHelper.hpp:28）-> flag=1，键去掉尾字符（:593-600）
	runner->sub_tick("CFFEX.IC.2204-", true, false);

	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);

	ASSERT_EQ(TickProbe::codes.size(), 1u);
	// trigger_tick :478 拼接 stdCode + SUFFIX_QFQ
	EXPECT_EQ(TickProbe::codes[0], "CFFEX.IC.2204-");
	EXPECT_DOUBLE_EQ(TickProbe::prices[0], 100.0);
	tick->release();
}

TEST_F(R8WtDtRunnerTest, HfqFlagGoesThroughCopyAndKeepsSourceTickUntouched)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	// SUFFIX_HFQ == '+'（CodeHelper.hpp:31）-> flag=2，走 :485-502 的复制+复权分支
	runner->sub_tick("CFFEX.IC.2204+", true, false);

	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);

	ASSERT_EQ(TickProbe::codes.size(), 1u);
	EXPECT_EQ(TickProbe::codes[0], "CFFEX.IC.2204+");
	// hots.json 里 oldclose==newclose => getRuleFactor 为 1.0（WTSHotMgr.cpp:257,61-72），
	// 且 :485 是在 newTick 副本上乘因子，原始 tick 的价格必须保持 100.0
	EXPECT_DOUBLE_EQ(TickProbe::prices[0], 100.0);
	EXPECT_DOUBLE_EQ(tick->getTickStruct().price, 100.0);
	tick->release();
}

TEST_F(R8WtDtRunnerTest, InnerSubscribeDoesNotFireCallbackAndReplaceIsPerTable)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);

	// 外部订阅一次 -> 1 条回调
	runner->sub_tick("CFFEX.IC.2204", true, false);
	runner->proc_tick(tick);
	ASSERT_EQ(TickProbe::codes.size(), 1u);

	// 内部订阅（:559-578）不会动外表，所以外发仍为 1 条
	runner->sub_tick("CFFEX.IC.2204", false, true);
	runner->proc_tick(tick);
	ASSERT_EQ(TickProbe::codes.size(), 2u);

	// 内部表 bReplace（:562-563）+ 外部表 bReplace（:582-583）：
	// 用 replace 清外表后，不应再有外部回调
	runner->sub_tick("CFFEX.IC.2204", true, false);
	runner->proc_tick(tick);
	EXPECT_EQ(TickProbe::codes.size(), 2u);

	tick->release();
}

TEST_F(R8WtDtRunnerTest, UnmatchedCodeIsFilteredOutBySubTable)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	runner->sub_tick("SSE.600000", true, false);
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	// 订阅表里没有 CFFEX.IC.2204 -> trigger_tick :467 未命中，:513 内部表也未命中
	EXPECT_TRUE(TickProbe::codes.empty());
	tick->release();
}

TEST_F(R8WtDtRunnerTest, TriggerBarDispatchesOnlyWhenCallbackBound)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = 20240102;
	bar.close = 105.5;

	runner->trigger_bar("CFFEX.IC.2204", "m1", &bar);
	ASSERT_EQ(BarProbe::codes.size(), 1u);
	EXPECT_EQ(BarProbe::codes[0], "CFFEX.IC.2204");
	EXPECT_EQ(BarProbe::periods[0], "m1");
	EXPECT_DOUBLE_EQ(BarProbe::closes[0], 105.5);

	// 另一个 runner 只绑 tick 回调 -> _cb_bar==NULL -> :639-640 直接返回
	std::unique_ptr<WtDtRunner> noBar(new WtDtRunner());
	noBar->initialize((_dir / "wtdt.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, nullptr);
	noBar->trigger_bar("CFFEX.IC.2204", "m5", &bar);
	EXPECT_EQ(BarProbe::codes.size(), 1u);
}

TEST_F(R8WtDtRunnerTest, MissingDataSectionStillCompletesInitialization)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	// 根配置不含 "data" -> initDataMgr(:192-195) config==NULL 早退，
	// 也不含 "parsers" -> :182-183 告警分支
	write_file(_dir / "nodata.json", buildCfg(false));
	runner->initialize((_dir / "nodata.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);

	// 能走到 proc_tick 并触发回调，说明 initialize 跑到了 :189 的 _is_inited=true
	runner->sub_tick("CFFEX.IC.2204", true, false);
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	EXPECT_EQ(TickProbe::codes.size(), 1u);
	tick->release();
}

TEST_F(R8WtDtRunnerTest, SecondInitializeIsRejectedAndStatePreserved)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);

	// :46-50  _is_inited 已为 true -> 第二次 initialize 只能记日志后返回，
	// 即使传的是不存在的配置文件也不得改变已绑定的回调
	runner->initialize((_dir / "no_such_cfg.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), nullptr, nullptr);

	runner->sub_tick("CFFEX.IC.2204", true, false);
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	// 若第二次 initialize 未被拒绝，_cb_tick 会被置 NULL -> 这里就是 0 条
	EXPECT_EQ(TickProbe::codes.size(), 1u);
	tick->release();
}

// ---------------------------------------------------------------- parsers 段（不加载真实插件）

TEST_F(R8WtDtRunnerTest, UnloadableParserArrayStillCompletesStartup)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	// parsers 数组 -> initParsers(:378-403)；模块名在空模块目录里必然加载失败
	// (ParserAdapter.cpp:110-113)，适配器仍会登记，start() 只会让 run() 返回 false
	// （ParserAdapter.cpp:221-222），绝不触发 connect()
	const std::string parsers = std::string("\"parsers\": [")
		+ "{\"id\":\"r8p1\",\"active\":true,\"module\":\"r8_no_such_parser\",\"filter\":\"CFFEX\"}"
		+ "]";
	write_file(_dir / "wparsers.json", buildCfg(true, parsers));
	runner->initialize((_dir / "wparsers.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);

	runner->sub_tick("CFFEX.IC.2204", true, false);
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	// 只有跑到 :189 才算 initialize 完成，回调可用即证明 start() 已安全返回
	EXPECT_EQ(TickProbe::codes.size(), 1u);
	EXPECT_EQ(runner->get_bars_by_date("CFFEX.IC.2204", "d1", 20240102), nullptr);
	tick->release();
}

TEST_F(R8WtDtRunnerTest, InactiveAndAutoIdParserEntriesAreBothHandled)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	// 第 1 项 active=false -> :383-384 continue；
	// 第 2 项没有 id -> :390-395 生成 auto_parser_<n>
	const std::string parsers = std::string("\"parsers\": [")
		+ "{\"id\":\"r8off\",\"active\":false,\"module\":\"r8_no_such_parser\"},"
		+ "{\"active\":true,\"module\":\"r8_no_such_parser2\"}"
		+ "]";
	write_file(_dir / "autoid.json", buildCfg(true, parsers));
	runner->initialize((_dir / "autoid.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);

	runner->sub_tick("CFFEX.IC.2204", true, false);
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	EXPECT_EQ(TickProbe::codes.size(), 1u);
	tick->release();
}

TEST_F(R8WtDtRunnerTest, ParserConfigGivenAsExternalFileOrMissingFile)
{
	// 分支一：parsers 是字符串且文件存在 -> :157-171（读取后再 initParsers）
	write_file(_dir / "parsers.json",
		R"JSON({"parsers":[{"id":"r8ext","active":true,"module":"r8_no_such_parser"}]})JSON");
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	const std::string parsers = std::string("\"parsers\": \"")
		+ (_dir / "parsers.json").string() + "\"";
	write_file(_dir / "extparsers.json", buildCfg(true, parsers));
	runner->initialize((_dir / "extparsers.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);

	runner->sub_tick("CFFEX.IC.2204", true, false);
	WTSTickData* tick = make_ic_tick();
	ASSERT_NE(tick, nullptr);
	runner->proc_tick(tick);
	ASSERT_EQ(TickProbe::codes.size(), 1u);
	tick->release();

	// 分支二：parsers 是字符串但文件不存在 -> :172-175 报错后继续
	TickProbe::codes.clear();
	std::unique_ptr<WtDtRunner> runner2(new WtDtRunner());
	const std::string badParsers = std::string("\"parsers\": \"")
		+ (_dir / "no_such_parsers.json").string() + "\"";
	write_file(_dir / "badextparsers.json", buildCfg(true, badParsers));
	runner2->initialize((_dir / "badextparsers.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);
	runner2->sub_tick("CFFEX.IC.2204", true, false);
	WTSTickData* tick2 = make_ic_tick();
	ASSERT_NE(tick2, nullptr);
	runner2->proc_tick(tick2);
	EXPECT_EQ(TickProbe::codes.size(), 1u);
	tick2->release();
}

// ---------------------------------------------------------------- 需要接缝的形态

/*!
 * 触发条件（启用即段错误，禁止在 CI 启用）：
 * 根配置缺少 basefiles 段 -> WtDtRunner.cpp:73 config->get("basefiles") 返回 NULL，
 * :74 紧接着 cfgBF->get("session") 解引用空指针。
 * 证据：WTSVariant::get 未命中即返回 NULL（同文件 :152-153 与
 * WtDataManager.cpp:104 都靠这个语义判空）。
 */
TEST_F(R8WtDtRunnerTest, DISABLED_InitializeWithoutBasefilesSectionSegfaults)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	write_file(_dir / "nobase.json", R"JSON({"disable_dump": true})JSON");
	runner->initialize((_dir / "nobase.json").string().c_str(), true,
		_modDir.c_str(), _logCfg.c_str(), onTickProbe, onBarProbe);
	EXPECT_EQ(runner->get_bars_by_date("CFFEX.IC.2204", "d1", 20240102), nullptr);
}

/*!
 * 触发条件（启用即段错误，禁止在 CI 启用）：
 * runner 正常 initialize（data 段无 store -> WtDataManager.cpp:104 -> initStore(NULL)
 * 返回 false，_reader 保持 NULL）后调用 sub_bar：
 * WtDtRunner.cpp:633 -> WtDataManager.cpp:516 无条件 _reader->readKlineSliceByCount。
 */
TEST_F(R8WtDtRunnerTest, DISABLED_SubBarWithoutReaderSegfaults)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);
	runner->sub_bar("CFFEX.IC.2204", "m1");
	EXPECT_TRUE(TickProbe::codes.empty());
}

/*!
 * 触发条件（启用即段错误，禁止在 CI 启用）：
 * 同上一条根因，凡是把请求转交给 _data_mgr 的查询入口都直接解引用 _reader：
 * WtDtRunner.cpp:239 -> WtDataManager.cpp:203。因此本波里
 * get_bars_by_range、get_bars_by_count、get_ticks_by_date、get_sbars_by_date 的
 * 周期解析与缓存代码（约 120 行）在测试二进制内不可达。
 */
TEST_F(R8WtDtRunnerTest, DISABLED_GetBarsByRangeWithoutReaderSegfaults)
{
	std::unique_ptr<WtDtRunner> runner(new WtDtRunner());
	initRunner(*runner);
	EXPECT_EQ(runner->get_bars_by_range("CFFEX.IC.2204", "m1", 202401020930, 202401021500), nullptr);
}

}  // namespace
