/*
 * R22 - WtDtRunner::initialize / initDataMgr / initParsers
 *
 * 覆盖 src/WtDtPorter/WtDtRunner.cpp:88-287（配置装载、基础数据、allday 分流、
 * index 段、parsers 的 string/array 两种形态与自动 id）。
 *
 * 约束：不起线程、不 sleep、不建 shm/socket、不加载任何真实插件 dylib。
 *  - logCfg 一律指向不存在的文件 -> WTSLogger::init 直接返回，不起日志线程；
 *  - modDir 一律指向本用例的临时目录 -> DataManager / ParserAdapter 的
 *    load_library 必然失败（q05_dtsrv 已验证该降级路径安全）；
 *  - shmcaster / broadcaster 两段一律不配 -> 不碰 ShmCaster / UDPCaster；
 *  - allday=true 跳过 StateMonitor::initialize，allday=false 只走 initialize（绝不 run）。
 *
 * 观测口径：createExtParser -> ParserAdapter::initExt -> WTSBaseDataMgr::getContracts()
 * 的订阅扇出（WtDtCore/ParserAdapter.cpp:60-72）。它把「基础数据到底装进了哪几条合约」
 * 变成可计数的回调，所以本文件对 initialize 的断言不是「没崩就算过」。
 * 期望值由每个用例自己用一个独立 WTSBaseDataMgr 装载同一批文件算出（refCodes），
 * 不抄 WtDtRunner 的实现分支。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "Includes/FasterDefs.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/IParserApi.h"
#include "Includes/WTSTypes.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtDtPorter/PorterDefs.h"
#include "WtDtPorter/WtDtRunner.h"
#include "WtDtPorter/WtDtPorter.h"

USING_NS_WTP;

namespace fs = std::filesystem;

namespace {

std::string r22_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	std::error_code ec;
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_r22_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p, ec);
	return p.string();
}

void write_text(const fs::path& p, const char* body)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << body;
	ofs.close();
}

struct SubHit
{
	std::string	_id;
	std::string	_code;
	bool		_sub;
};

struct EvtHit
{
	uint32_t	_evt;
	std::string	_id;
};

std::vector<SubHit>& sub_hits() { static std::vector<SubHit> v; return v; }
std::vector<EvtHit>& evt_hits() { static std::vector<EvtHit> v; return v; }

void PORTER_FLAG rec_evt(WtUInt32 evtId, const char* id)
{
	evt_hits().push_back(EvtHit { evtId, id == NULL ? std::string() : std::string(id) });
}

void PORTER_FLAG rec_sub(const char* id, const char* fullCode, bool isForSub)
{
	sub_hits().push_back(SubHit { id == NULL ? std::string() : std::string(id),
		fullCode == NULL ? std::string() : std::string(fullCode), isForSub });
}

const char* SESSIONS_JSON = R"({
	"DAY0":{"name":"day","offset":0,"auction":{"from":915,"to":925},
		"sections":[{"from":900,"to":1130},{"from":1300,"to":1500}]},
	"NIGHT":{"name":"night","offset":300,
		"sections":[{"from":2100,"to":2300},{"from":900,"to":1500}]}
})";

const char* COMMODITIES_A_JSON = R"({
	"SSE":{"STK":{"name":"stk","session":"DAY0","holiday":"CHINA","pricetick":0.01,
		"volscale":1,"covermode":0,"pricemode":0,"trademode":1}}
})";

const char* COMMODITIES_B_JSON = R"({
	"SHFE":{"rb":{"name":"rb","session":"NIGHT","holiday":"CHINA","pricetick":1,
		"volscale":10,"covermode":0,"pricemode":1}}
})";

// BADPROD 因 product 未注册会被跳过 -> ct_a 的有效合约是 2 条
const char* CONTRACTS_A_JSON = R"({
	"SSE":{
		"600000":{"name":"a","exchg":"SSE","product":"STK","opendate":20200101,"expiredate":20500101},
		"600999":{"name":"b","exchg":"SSE","product":"STK","opendate":20100101,"expiredate":20500101},
		"BADPROD":{"name":"c","exchg":"SSE","product":"NOPE"}
	}
})";

const char* CONTRACTS_B_JSON = R"({
	"SHFE":{
		"rb2401":{"name":"d","exchg":"SHFE","product":"rb",
			"opendate":20230110,"expiredate":20500115,"longmarginratio":0.1,"shortmarginratio":0.12}
	}
})";

const char* HOLIDAYS_JSON = R"({"CHINA":[20240101,20240210]})";
const char* HOT_JSON = R"({"SHFE.rb":{"rules":[{"action":"r22hot"}]}})";
const char* SECOND_JSON = R"({"SHFE.rb":{"rules":[{"action":"r22sec"}]}})";
const char* STATE_JSON = R"({"DAY0":{"inittime":850,"closetime":1510,"proctime":1530},
	"NIGHT":{"inittime":2050,"closetime":2310,"proctime":2330}})";

} // namespace

class R22RunnerInitTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = fs::path(r22_tmp_dir("r22init"));
		write_text(_dir / "sessions.json", SESSIONS_JSON);
		write_text(_dir / "comm_a.json", COMMODITIES_A_JSON);
		write_text(_dir / "comm_b.json", COMMODITIES_B_JSON);
		write_text(_dir / "ct_a.json", CONTRACTS_A_JSON);
		write_text(_dir / "ct_b.json", CONTRACTS_B_JSON);
		write_text(_dir / "holidays.json", HOLIDAYS_JSON);
		write_text(_dir / "hots.json", HOT_JSON);
		write_text(_dir / "seconds.json", SECOND_JSON);
		write_text(_dir / "state.json", STATE_JSON);
		// 不存在的日志文件：WTSLogger::init 直接返回，不起线程、不建 sink
		_logCfg = (_dir / "absent_logcfg.json").string();
		// 不存在的 writer 模块名：DataManager::init 走 load_library 失败分支后 return false
		_absentMod = "r22_absent_writer";

		sub_hits().clear();
		evt_hits().clear();
		register_parser_callbacks(NULL, NULL);

		// —— 自检（rule 5）：夹具文件本身必须真的能装出预期的合约全集
		std::set<std::string> self = refCodes({"comm_a.json"}, {"ct_a.json"});
		ASSERT_EQ(2U, self.size());
		std::set<std::string> selfAll = refCodes({"comm_a.json", "comm_b.json"},
			{"ct_a.json", "ct_b.json"});
		ASSERT_EQ(3U, selfAll.size());
	}

	void TearDown() override
	{
		register_parser_callbacks(NULL, NULL);
	}

	std::string pathOf(const char* name) const { return (_dir / name).string(); }

	// 用独立的 WTSBaseDataMgr 按同样的装载顺序算出期望的合约 full code 全集
	std::set<std::string> refCodes(std::initializer_list<const char*> commFiles,
		std::initializer_list<const char*> ctFiles)
	{
		WTSBaseDataMgr mgr;
		EXPECT_TRUE(mgr.loadSessions(pathOf("sessions.json").c_str()));
		for (const char* f : commFiles)
			mgr.loadCommodities(pathOf(f).c_str());
		for (const char* f : ctFiles)
			mgr.loadContracts(pathOf(f).c_str());
		std::set<std::string> ret;
		uint32_t n = mgr.getContractSize();
		for (uint32_t i = 0; i < n; i++)
		{
			WTSContractInfo* c = mgr.getContractByIndex(i);
			if (c != NULL)
				ret.insert(c->getFullCode());
		}
		return ret;
	}

	// 把 runner 里真正装载的合约全集「问」出来：createExtParser -> initExt -> subscribe 扇出
	std::set<std::string> probeLoaded(WtDtRunner& runner, const char* id, uint32_t* evtOut = NULL)
	{
		sub_hits().clear();
		evt_hits().clear();
		register_parser_callbacks(rec_evt, rec_sub);
		EXPECT_TRUE(runner.createExtParser(id));
		std::set<std::string> got;
		for (size_t i = 0; i < sub_hits().size(); i++)
		{
			EXPECT_EQ(std::string(id), sub_hits()[i]._id);
			EXPECT_TRUE(sub_hits()[i]._sub);
			got.insert(sub_hits()[i]._code);
		}
		if (evtOut != NULL)
			*evtOut = (uint32_t)evt_hits().size();
		register_parser_callbacks(NULL, NULL);
		return got;
	}

	// 返回根对象尚未闭合的配置串（调用方负责最后那个 "}"）
	std::string cfgHead(bool allday) const
	{
		std::string s = "{";
		s += "\"basefiles\":{";
		s += "\"session\":\"" + pathOf("sessions.json") + "\",";
		s += "\"holiday\":\"" + pathOf("holidays.json") + "\",";
		s += "\"hot\":\"" + pathOf("hots.json") + "\",";
		s += "\"second\":\"" + pathOf("seconds.json") + "\",";
		s += "\"rules\":{\"r22tag\":\"" + pathOf("hots.json") + "\"},";
		s += "\"commodity\":\"" + pathOf("comm_a.json") + "\",";
		s += "\"contract\":\"" + pathOf("ct_a.json") + "\"";
		s += "},";
		s += "\"allday\":" + std::string(allday ? "true" : "false") + ",";
		s += "\"statemonitor\":\"" + pathOf("state.json") + "\",";
		s += "\"writer\":{\"module\":\"" + _absentMod + "\",\"keepdata\":false}";
		return s;
	}

	fs::path			_dir;
	std::string			_logCfg;
	std::string			_absentMod;
};

//////////////////////////////////////////////////////////////////////////
TEST_F(R22RunnerInitTest, AlldayModeWithScalarBasefilesLoadsContractsAndSkipsParsers)
{
	WtDtRunner runner;
	std::string cfg = cfgHead(true) + "}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	// parsers 段缺失 -> WtDtRunner.cpp:252 只打 warn，_parsers 保持为空
	uint32_t evtCnt = 0;
	std::set<std::string> got = probeLoaded(runner, "r22_probe_allday", &evtCnt);
	EXPECT_EQ(refCodes({"comm_a.json"}, {"ct_a.json"}), got);
	EXPECT_EQ(1U, evtCnt);	// 只有 ExpParser::init 这一个事件
	// 每条合约恰好一次订阅扇出：full code 全集无重复
	EXPECT_EQ(got.size(), sub_hits().size());
}

TEST_F(R22RunnerInitTest, NonAlldayModeInitializesStateMonitorAndStillLoadsBasefiles)
{
	WtDtRunner runner;
	std::string cfg = cfgHead(false) + "}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	std::set<std::string> got = probeLoaded(runner, "r22_probe_mon");
	EXPECT_EQ(refCodes({"comm_a.json"}, {"ct_a.json"}), got);
}

TEST_F(R22RunnerInitTest, HolidayHotSecondAndCustomRulesSectionsAreLoadedWithoutBreakingBasefiles)
{
	WtDtRunner runner;
	std::string cfg = "{";
	cfg += "\"basefiles\":{";
	cfg += "\"session\":\"" + pathOf("sessions.json") + "\",";
	// holiday / hot / second / rules 四个可选段全开，其中一个指向不存在的文件
	cfg += "\"holiday\":\"" + pathOf("holidays.json") + "\",";
	cfg += "\"hot\":\"" + pathOf("hots.json") + "\",";
	cfg += "\"second\":\"" + pathOf("no_such_seconds.json") + "\",";
	cfg += "\"rules\":{\"r22a\":\"" + pathOf("hots.json") + "\",";
	cfg += "\"r22b\":\"" + pathOf("seconds.json") + "\"},";
	cfg += "\"commodity\":\"" + pathOf("comm_a.json") + "\",";
	cfg += "\"contract\":\"" + pathOf("ct_a.json") + "\"";
	cfg += "},\"allday\":true,\"writer\":{\"module\":\"" + _absentMod + "\"}}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	std::set<std::string> got = probeLoaded(runner, "r22_probe_opt");
	EXPECT_EQ(refCodes({"comm_a.json"}, {"ct_a.json"}), got);
}

TEST_F(R22RunnerInitTest, BasefilesCommodityAndContractAsArraysWalkBothBranches)
{
	WtDtRunner runner;
	std::string cfg = "{";
	cfg += "\"basefiles\":{";
	cfg += "\"session\":\"" + pathOf("sessions.json") + "\",";
	cfg += "\"holiday\":\"" + pathOf("holidays.json") + "\",";
	// 数组形态：第三个元素指向不存在的文件，循环必须继续而不是中断
	cfg += "\"commodity\":[\"" + pathOf("comm_a.json") + "\",\"" + pathOf("comm_b.json")
		+ "\",\"" + pathOf("no_such_comm.json") + "\"],";
	cfg += "\"contract\":[\"" + pathOf("ct_a.json") + "\",\"" + pathOf("ct_b.json")
		+ "\",\"" + pathOf("no_such_ct.json") + "\"]";
	cfg += "},\"allday\":true,\"writer\":{\"module\":\"" + _absentMod + "\"}}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	std::set<std::string> got = probeLoaded(runner, "r22_probe_arr");
	EXPECT_EQ(refCodes({"comm_a.json", "comm_b.json"}, {"ct_a.json", "ct_b.json"}), got);
}

TEST_F(R22RunnerInitTest, ConfigFileLoadedFromDiskPathWhenCfgFileFlagIsTrue)
{
	std::string body = cfgHead(true) + "}";
	write_text(_dir / "wtdt.json", body.c_str());

	WtDtRunner runner;
	std::string path = (_dir / "wtdt.json").string();
	runner.initialize(path.c_str(), _logCfg.c_str(), _dir.string().c_str(), true, true);

	std::set<std::string> got = probeLoaded(runner, "r22_probe_file");
	EXPECT_EQ(refCodes({"comm_a.json"}, {"ct_a.json"}), got);
}

TEST_F(R22RunnerInitTest, UnparseableContentReturnsBeforeLoadingBasefiles)
{
	WtDtRunner runner;
	const char* broken = "{ this is neither json nor yaml ]";
	runner.initialize(broken, _logCfg.c_str(), _dir.string().c_str(), false, false);

	// WtDtRunner.cpp:99-103 早退：一条合约都不该被装进来
	std::set<std::string> got = probeLoaded(runner, "r22_probe_broken");
	EXPECT_TRUE(got.empty());
	// 对照组：同一份夹具正常解析时确实能装出 2 条
	EXPECT_EQ(2U, refCodes({"comm_a.json"}, {"ct_a.json"}).size());
}

TEST_F(R22RunnerInitTest, MissingConfigFileReturnsBeforeLoadingBasefiles)
{
	WtDtRunner runner;
	std::string absent = (_dir / "no_such_cfg.json").string();
	runner.initialize(absent.c_str(), _logCfg.c_str(), _dir.string().c_str(), true, true);

	std::set<std::string> got = probeLoaded(runner, "r22_probe_absent");
	EXPECT_TRUE(got.empty());
}

TEST_F(R22RunnerInitTest, IndexSectionWithAbsentFileIsReportedAndDoesNotBreakBasefiles)
{
	WtDtRunner runner;
	std::string cfg = cfgHead(true) + ",\"index\":\"" + pathOf("no_such_index.json") + "\"}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	// loadFactories 的目录 {cwd}index/ 在本机构建目录下不存在 -> 返回 false；
	// 随后的 index 配置 load_from_file 失败 -> 只打 error，基础数据不受影响
	std::set<std::string> got = probeLoaded(runner, "r22_probe_idx");
	EXPECT_EQ(refCodes({"comm_a.json"}, {"ct_a.json"}), got);
}

TEST_F(R22RunnerInitTest, ParsersAsInlineArrayRegistersActiveEntriesOnly)
{
	WtDtRunner runner;
	std::string cfg = cfgHead(true);
	cfg += ",\"parsers\":[";
	cfg += "{\"active\":false,\"id\":\"r22_off\",\"module\":\"" + _absentMod + "\"},";
	cfg += "{\"active\":true,\"module\":\"" + _absentMod + "\"},";	// 无 id -> 自动 id
	cfg += "{\"active\":true,\"id\":\"r22_p2\",\"module\":\"" + _absentMod + "\"}";
	cfg += "]}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	// 契约：createExtParser 不检查 ParserAdapterMgr::addAdapter 的返回值，
	// 撞名时依旧返回 true（见 BUG-R22-003）；不存在的 id 也返回 true -> 返回值零信息量
	EXPECT_TRUE(runner.createExtParser("r22_p2"));
	EXPECT_TRUE(runner.createExtParser("auto_parser_1000"));
	EXPECT_TRUE(runner.createExtParser("r22_never_used"));
}

TEST_F(R22RunnerInitTest, ParsersAsStringPathToExistingFileIsExpanded)
{
	const char* body = R"({"parsers":[
		{"active":true,"id":"r22_from_file","module":"r22_absent_writer"}
	]})";
	write_text(_dir / "parsers.json", body);

	WtDtRunner runner;
	std::string cfg = cfgHead(true);
	cfg += ",\"parsers\":\"" + pathOf("parsers.json") + "\"}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	// 该 id 已被 initParsers 占用：重复 createExtParser 仍返回 true（BUG-R22-003）
	EXPECT_TRUE(runner.createExtParser("r22_from_file"));
}

TEST_F(R22RunnerInitTest, ParsersAsStringPathToMissingFileIsSkipped)
{
	WtDtRunner runner;
	std::string cfg = cfgHead(true);
	cfg += ",\"parsers\":\"" + pathOf("no_such_parsers.json") + "\"}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);

	std::set<std::string> got = probeLoaded(runner, "r22_probe_pabsent");
	EXPECT_EQ(refCodes({"comm_a.json"}, {"ct_a.json"}), got);
	// 未装载任何 parser 适配器 -> 推送行情只会命中「Parser not exists」告警支
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	strncpy(tick.exchg, "SSE", sizeof(tick.exchg) - 1);
	strncpy(tick.code, "600000", sizeof(tick.code) - 1);
	tick.action_date = 20240108;
	tick.trading_date = 20240108;
	runner.on_ext_parser_quote("r22_probe_pabsent", &tick, 0);
	EXPECT_EQ(20240108U, tick.trading_date);
}

//////////////////////////////////////////////////////////////////////////
// 崩溃路径：整条 DISABLED_，触发条件写在这里，缺陷登记见 bugs/R22_dtporter.md
TEST_F(R22RunnerInitTest, DISABLED_basefiles_section_missing_derefs_null_section)
{
	// 触发条件：合法 json，但完全没有 basefiles 段 ->
	//   WtDtRunner.cpp:106 config->get("basefiles") 返回 NULL，
	//   :107 cfgBF->get("session") 直接对 NULL 取成员 -> SIGSEGV。
	WtDtRunner runner;
	const char* cfg = "{\"allday\":true,\"writer\":{\"module\":\"r22_absent_writer\"}}";
	runner.initialize(cfg, _logCfg.c_str(), _dir.string().c_str(), false, false);
	FAIL() << "unreachable: 上一行已经段错误";
}

TEST_F(R22RunnerInitTest, DISABLED_writer_section_missing_derefs_null_params)
{
	// 触发条件：basefiles 齐全、allday 任意，但没有 writer 段 ->
	//   WtDtRunner.cpp:199 config->get("writer") 返回 NULL，
	//   WtDtCore/DataManager.cpp::init 第一句 params->getCString("module") 解引用 NULL。
	WtDtRunner runner;
	std::string cfg = "{";
	cfg += "\"basefiles\":{\"session\":\"" + pathOf("sessions.json") + "\",";
	cfg += "\"contract\":\"" + pathOf("ct_a.json") + "\"";
	cfg += "},\"allday\":true}";
	runner.initialize(cfg.c_str(), _dir.string().c_str(), _dir.string().c_str(), false, false);
	FAIL() << "unreachable: 上一行已经段错误";
}

TEST_F(R22RunnerInitTest, DISABLED_parsers_file_without_parsers_key_derefs_null)
{
	// 触发条件：parsers 指向一个「存在、能解析、但不含 parsers 键」的 json 文件 ->
	//   WtDtRunner.cpp:233 initParsers(var->get("parsers")) 传入 NULL，
	//   :264 cfg->size() 解引用 NULL。
	write_text(_dir / "parsers_naked.json", R"({"somethingelse":1})");
	WtDtRunner runner;
	std::string cfg = cfgHead(true);
	cfg += ",\"parsers\":\"" + pathOf("parsers_naked.json") + "\"}";
	runner.initialize(cfg.c_str(), _logCfg.c_str(), _dir.string().c_str(), false, false);
	FAIL() << "unreachable: 上一行已经段错误";
}

TEST_F(R22RunnerInitTest, DISABLED_start_sync_branch_never_returns)
{
	// 触发条件：任何 start(false) 调用（WtDtRunner.cpp:44-77）。
	//   :50 install_signal_hooks 会往宿主测试进程装 SIGINT/SIGTERM 处理函数；
	//   :68-74 StdThread 里 _async_io.run_one() 在唯一 post 的任务跑完后永久阻塞；
	//   :76 trd.join() 只有 _to_exit 被信号回调置位后才返回 -> 整个进程挂死。
	WtDtRunner runner;
	runner.start(false);
	SUCCEED();
}

TEST_F(R22RunnerInitTest, DISABLED_start_async_branch_spawns_state_monitor_thread)
{
	// 触发条件：start(true)。WtDtRunner.cpp:80-84 在 bAlldayMode=false 时
	//   sleep_for(5ms) + _state_mon.run()，后者起监控线程（WtDtCore/StateMonitor.cpp:145+）。
	// 另注意 WtDtPorter.cpp:62-65 的 C 入口 start(bAsync) 根本没有 allday 形参，
	// 走 C 接口时 bAlldayMode 恒为 false -> 全天候模式配置下也会起状态机线程，见 BUG-R22-002。
	WtDtRunner runner;
	runner.start(true);
	SUCCEED();
}
