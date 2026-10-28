/*
 * R22 - WtDtPorter.cpp 的 C 入口 + Exp* 单例硬绑证据
 *
 * 覆盖 src/WtDtPorter/WtDtPorter.cpp:48-52,54-60,67-81,83-93,97-100,102-105,107-110,
 * 116-119,121-124,126-129 与 ExpParser.cpp/ExpDumper.cpp 的单例绑定面。
 *
 * 说明：WtDtPorter.cpp:33-46（getModuleName）与 :56-58（CMiniDumper::Enable）在
 * #ifdef _MSC_VER 内，本机 macOS/arm64 不参与编译，本轮不主张覆盖。
 * start(bool)（:62-65）会进 WtDtRunner::start 的线程/状态机分支 -> 整条 DISABLED_。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "Includes/FasterDefs.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSTypes.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtDtCore/WtHelper.h"

#include "WtDtPorter/ExpDumper.h"
#include "WtDtPorter/ExpParser.h"
#include "WtDtPorter/PorterDefs.h"
#include "WtDtPorter/WtDtPorter.h"
#include "WtDtPorter/WtDtRunner.h"

USING_NS_WTP;

namespace fs = std::filesystem;

namespace {

std::string r22c_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	std::error_code ec;
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_r22c_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p, ec);
	return p.string();
}

void write_text_c(const fs::path& p, const char* body)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << body;
	ofs.close();
}

struct EvtHit
{
	uint32_t	_evt;
	std::string	_id;
};

std::vector<EvtHit>& evt_global() { static std::vector<EvtHit> v; return v; }
std::vector<EvtHit>& evt_local() { static std::vector<EvtHit> v; return v; }
std::vector<std::string>& bar_global() { static std::vector<std::string> v; return v; }
std::vector<std::string>& bar_local() { static std::vector<std::string> v; return v; }

void PORTER_FLAG evt_g(WtUInt32 e, const char* id) { evt_global().push_back(EvtHit { e, id ? id : "" }); }
void PORTER_FLAG evt_l(WtUInt32 e, const char* id) { evt_local().push_back(EvtHit { e, id ? id : "" }); }

bool PORTER_FLAG bar_g(const char*, const char*, const char*, WTSBarStruct*, WtUInt32)
{
	bar_global().push_back("g");
	return true;
}

bool PORTER_FLAG bar_l(const char*, const char*, const char*, WTSBarStruct*, WtUInt32)
{
	bar_local().push_back("l");
	return true;
}

bool PORTER_FLAG tick_none(const char*, const char*, WtUInt32, WTSTickStruct*, WtUInt32)
{
	return false;
}

std::vector<std::string>& sub_collector() { static std::vector<std::string> v; return v; }

void PORTER_FLAG rec_sub_c(const char* id, const char* code, bool isForSub)
{
	(void)id;
	(void)isForSub;
	sub_collector().push_back(code == NULL ? std::string() : std::string(code));
}

} // namespace

class R22PorterCapiTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = fs::path(r22c_tmp_dir("r22capi"));
		evt_global().clear();
		evt_local().clear();
		bar_global().clear();
		bar_local().clear();
		// 每个用例都从「全局槽位为空」开始，避免跨用例串扰
		register_parser_callbacks(NULL, NULL);
		register_extended_dumper(NULL, NULL);
		register_extended_hftdata_dumper(NULL, NULL, NULL);
	}

	void TearDown() override
	{
		register_parser_callbacks(NULL, NULL);
		register_extended_dumper(NULL, NULL);
		register_extended_hftdata_dumper(NULL, NULL, NULL);
		// 若有用例走过 C initialize()，把模块目录还给本用例的临时目录，
		// 免得 getBinDir() 泄漏给后续 lane
		WtHelper::set_module_dir(_dir.string().c_str());
	}

	fs::path _dir;
};

//////////////////////////////////////////////////////////////////////////
TEST_F(R22PorterCapiTest, VersionStringIsPlatformPrefixedAndBuiltOnce)
{
	const char* v1 = get_version();
	ASSERT_TRUE(v1 != NULL);
	// WtDtPorter.cpp:26 非 Windows 分支的 PLATFORM_NAME
	EXPECT_EQ(0, strncmp(v1, "UNIX ", 5));
	EXPECT_TRUE(strstr(v1, " Build@") != NULL);

	const char* v2 = get_version();
	// :69-79 的 static std::string 缓存：第二次走 _ver.empty() 假支，返回同一块内存
	EXPECT_EQ(v1, v2);
	EXPECT_GT(strlen(v1), 10U);
}

TEST_F(R22PorterCapiTest, WriteLogKeepsCallerBuffersUnchanged)
{
	char msg[] = "r22 plain message without braces";
	char cat[] = "parser";
	char msgCpy[sizeof(msg)], catCpy[sizeof(cat)];
	memcpy(msgCpy, msg, sizeof(msg));
	memcpy(catCpy, cat, sizeof(cat));

	const char* v = get_version();	// 顺带把 __DATE__/__TIME__ 分支再走一次
	(void)v;
	// 带类别 -> WtDtPorter.cpp:87；空类别 -> :91
	write_log((unsigned int)LL_FATAL, msg, cat);
	write_log((unsigned int)LL_FATAL, msg, "");

	EXPECT_EQ(0, memcmp(msg, msgCpy, sizeof(msg)));
	EXPECT_EQ(0, memcmp(cat, catCpy, sizeof(cat)));
}

TEST_F(R22PorterCapiTest, WriteLogBelowLoggerThresholdIsDropped)
{
	// WTSLogger.cpp::log_raw 的第一道守卫是 m_logLevel > ll 直接 return；
	// 未 init（日志配置文件不存在）时 m_logLevel 仍是默认值，
	// 所以数值低于默认阈值的 level（如 1/2）在 C 入口里其实是一次空转。
	char msg[] = "r22 below threshold";
	char cpy[sizeof(msg)];
	memcpy(cpy, msg, sizeof(msg));
	write_log(1u, msg, "");
	write_log(2u, msg, "parser");
	EXPECT_EQ(0, memcmp(msg, cpy, sizeof(msg)));
}

TEST_F(R22PorterCapiTest, RegisterExtendedDumperStoresBothBarAndTickSlots)
{
	register_extended_dumper(bar_g, tick_none);
	ExpDumper dumper("r22_slot");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));

	// bar 槽返回 true，tick 槽返回 false -> 证明两个槽位互不串扰且原样透出返回值
	EXPECT_TRUE(dumper.dumpHisBars("SHFE.rb2401", "m1", &bar, 1));
	EXPECT_FALSE(dumper.dumpHisTicks("SHFE.rb2401", 20240110, &tick, 1));
	ASSERT_EQ(1U, bar_global().size());

	register_extended_dumper(NULL, NULL);
	EXPECT_FALSE(dumper.dumpHisBars("SHFE.rb2401", "m1", &bar, 1));
	EXPECT_FALSE(dumper.dumpHisTicks("SHFE.rb2401", 20240110, &tick, 1));
	EXPECT_EQ(1U, bar_global().size());
}

TEST_F(R22PorterCapiTest, ExpParserEventsReachGlobalRunnerNotTheOwningRunner)
{
	WtDtRunner owner;
	owner.registerParserPorter(evt_l, NULL);
	register_parser_callbacks(evt_g, NULL);

	// 正交对照：直接调用 owner 的入口 -> 只有 owner 自己的槽位被命中
	owner.parser_init("manual");
	ASSERT_EQ(1U, evt_local().size());
	ASSERT_EQ(0U, evt_global().size());
	evt_local().clear();

	// 被测路径：createExtParser 在 owner 上建适配器，但 ExpParser::init
	// 走的是 ExpParser.cpp:21 的 extern getRunner()（WtDtPorter.cpp:48-52 的单例）
	ASSERT_TRUE(owner.createExtParser("r22_xbind"));
	EXPECT_EQ(0U, evt_local().size());
	ASSERT_EQ(1U, evt_global().size());
	EXPECT_EQ(EVENT_PARSER_INIT, evt_global()[0]._evt);
	EXPECT_EQ("r22_xbind", evt_global()[0]._id);
}

TEST_F(R22PorterCapiTest, ExpDumperAlwaysForwardsToGlobalRunnerNotOwningRunner)
{
	WtDtRunner owner;
	owner.registerExtDumper(bar_l, NULL);
	register_extended_dumper(NULL, NULL);

	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	// 控制组：owner 自己的 dumpHisBars 命中 owner 的槽位
	ASSERT_TRUE(owner.dumpHisBars("r22_x", "SHFE.rb2401", "m1", &bar, 1));
	ASSERT_EQ(1U, bar_local().size());

	// 被测路径：ExpDumper 不携带任何 runner 引用，只能落到单例上
	ExpDumper dumper("r22_x");
	EXPECT_FALSE(dumper.dumpHisBars("SHFE.rb2401", "m1", &bar, 1));
	EXPECT_EQ(1U, bar_local().size());
	EXPECT_EQ(0U, bar_global().size());
}

TEST_F(R22PorterCapiTest, CapiInitializeWithAbsentLogFileKeepsLoggerUninited)
{
	// logCfg 指向不存在的文件 -> WTSLogger::init 早退，不起日志线程、不建 sink；
	// 这里只断言 C 入口把 cfg 内容/日志配置/模块目录三件事透传给 WtDtRunner::initialize，
	// 并且模块目录被置成 getBinDir()（WtDtPorter.cpp:59）
	std::string cfg = "{\"basefiles\":{\"session\":\"" + (_dir / "sessions.json").string()
		+ "\",\"commodity\":\"" + (_dir / "comm.json").string()
		+ "\",\"contract\":\"" + (_dir / "ct.json").string() + "\"},"
		+ "\"allday\":true,\"writer\":{\"module\":\"r22_absent_writer\"}}";
	write_text_c(_dir / "sessions.json",
		R"({"DAY0":{"name":"day","offset":0,"sections":[{"from":900,"to":1500}]}})");
	write_text_c(_dir / "comm.json",
		R"({"SSE":{"STK":{"name":"stk","session":"DAY0","pricetick":0.01,"volscale":1,
		"covermode":0,"pricemode":0,"trademode":1}}})");
	write_text_c(_dir / "ct.json",
		R"({"SSE":{"600000":{"name":"a","exchg":"SSE","product":"STK",
		"opendate":20200101,"expiredate":20500101}}})");

	const std::string absentLog = (_dir / "absent_log.json").string();
	initialize(cfg.c_str(), absentLog.c_str(), false, false);

	// 单例是否真的装载了 basefiles，用 C 入口 create_ext_parser 的订阅扇出来说话
	// （WtDtRunner.cpp:349-357 -> ParserAdapter::initExt -> getContracts() 全集订阅）
	register_parser_callbacks(NULL, rec_sub_c);
	sub_collector().clear();
	create_ext_parser("r22_capi_init");
	std::set<std::string> got;
	for (size_t i = 0; i < sub_collector().size(); i++)
		got.insert(sub_collector()[i]);
	register_parser_callbacks(NULL, NULL);

	// 期望值由本用例自己的夹具文件经独立 WTSBaseDataMgr 算出（rule 5/6）
	WTSBaseDataMgr ref;
	ASSERT_TRUE(ref.loadSessions((_dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(ref.loadCommodities((_dir / "comm.json").string().c_str()));
	ASSERT_TRUE(ref.loadContracts((_dir / "ct.json").string().c_str()));
	std::set<std::string> expect;
	uint32_t n = ref.getContractSize();
	for (uint32_t i = 0; i < n; i++)
	{
		WTSContractInfo* c = ref.getContractByIndex(i);
		if (c != NULL)
			expect.insert(c->getFullCode());
	}
	ASSERT_FALSE(expect.empty());
	EXPECT_EQ(expect, got);

	// C initialize 把 modDir 置成了 getBinDir()（WtDtPorter.cpp:59）
	EXPECT_FALSE(std::string(WtHelper::get_module_dir()).empty());
}

TEST_F(R22PorterCapiTest, CreateExtDumperIsIdempotentAndReturnsTrue)
{
	EXPECT_TRUE(create_ext_dumper("r22_dup_dump"));
	// 同一 id 第二次：_dumpers[id] 被覆盖，但 add_ext_dumper 已经登记过旧指针，
	// 函数本身依旧返回 true（WtDtRunner.cpp:361-370）
	EXPECT_TRUE(create_ext_dumper("r22_dup_dump"));

	ExpDumper probe("r22_dup_dump");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	register_extended_dumper(bar_g, NULL);
	EXPECT_TRUE(probe.dumpHisBars("SSE.600000", "m1", &bar, 1));
	EXPECT_EQ(1U, bar_global().size());
}

//////////////////////////////////////////////////////////////////////////
TEST_F(R22PorterCapiTest, DISABLED_write_log_with_braces_escapes_format_string)
{
	// 触发条件：write_log 的 level >= WTSLogger 默认阈值且日志未 init 时，
	//   WTSLogger::log_raw_by_cat / log_raw 走 print_timetag + fmt::print(message) 一支，
	//   把调用方给的 message 当成格式串使用。消息里出现 "{}" / "{" 时 fmt 抛
	//   fmt::format_error，异常越过 extern "C" 边界 -> std::terminate，宿主进程直接没。
	//   parser/trader 插件里任何带大括号的原始报文（JSON 片段）都能触发。
	char msg[] = "r22 brace {not a format} message";
	write_log((unsigned int)LL_FATAL, msg, "parser");
	SUCCEED();
}

TEST_F(R22PorterCapiTest, DISABLED_write_log_with_null_category_hits_strlen_null)
{
	// 触发条件：write_log(level, msg, NULL) -> WtDtPorter.cpp:85 的 strlen(catName)
	// 对 NULL 取 strlen -> SIGSEGV。与 bugs/l05_dt_servo_porter.md 的 L05-005 同一条，
	// 本轮只做交叉引用登记（见 BUG-R22-005）。
	char msg[] = "r22 null category";
	write_log((unsigned int)LL_INFO, msg, NULL);
	SUCCEED();
}

TEST_F(R22PorterCapiTest, DISABLED_capi_start_always_spawns_state_monitor_thread)
{
	// 触发条件：C 入口 start(false) 或 start(true)。
	//   WtDtPorter.cpp:62-65 只把 bAsync 透传下去，形参里根本没有 allday，
	//   所以 WtDtRunner::start 的 bAlldayMode 恒为 false ->
	//   :63/:82 的 sleep_for(5ms) + _state_mon.run() 必然执行（StateMonitor 起线程）；
	//   bAsync=false 时还额外 install_signal_hooks + StdThread.join() 永久阻塞。
	//   见 BUG-R22-002。
	start(true);
	SUCCEED();
}
