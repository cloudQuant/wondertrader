/*
 * r213_dtcore_test.cpp  (wave29 · lane R213)
 *
 * WtDtCore 四靶（UDPCaster 65 / StateMonitor.h 49 / StateMonitor.cpp 29 /
 * ParserAdapter 16）本轮复核后的可执行面。
 *
 * 关键测量事实：测试目标用 -isystem <repo>/src 引入生产头，clang 会把经由
 * 该路径命中的头文件当作系统头，不给其中的 inline 函数生成覆盖映射；因此
 * 既有的查询用例虽然 PASS，StateMonitor.h:70-81、169-222 仍记为 0。
 * 本文件把 StateMonitor.h 以相对路径（"../../../WtDtCore/StateMonitor.h"）
 * 放在所有 include 之前，使其按用户头编译，49 行 inline 代码才真正可计量。
 *
 * 覆盖内容：
 *  - StateInfo::isInSections：命中、区间右端开区间、两段之间、空表。
 *  - StateMonitor::isInState / isAnyInState / isAllInState：命中、状态不符、
 *    键不存在、全量遍历的真/假返回（两个会话都停在 SS_ORIGINAL）。
 *  - StateMonitor::initialize 的缺文件 / 空配置 / 未知会话早退。
 *  - WtDtCore::ParserAdapter 的 NULL 配置、空 module、缺模块、二次 init、
 *    initExt 空 API 与无 API run。
 *  - UDPCaster 的 inactive 配置、非法单播/组播地址、未 start 时四个
 *    broadcast 重载都是空操作。
 *
 * 边界：StateMonitor 不 run（不起线程）；不建 socket、不 bind、不加载任何
 * 解析插件、不写仓库内文件；临时配置只落在系统临时目录并在用例末尾清理。
 */

/* 必须最先包含：以相对路径命中 -> 用户头 -> inline 函数带覆盖映射 */
#include "../../../WtDtCore/StateMonitor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/ParserAdapter.h>
#include <WtDtCore/UDPCaster.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* tag)
{
	fs::path p = fs::temp_directory_path()
		/ (std::string("r213_dtcore_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

void write_file(const fs::path& p, const std::string& body)
{
	std::ofstream ofs(p.string(), std::ios::binary | std::ios::trunc);
	ofs << body;
}

void write_valid_config(const fs::path& dir)
{
	write_file(dir / "sessions.json",
		R"JSON({
			"R213A": {"name": "连续", "offset": 0, "sections": [{"from": 1, "to": 2359}]},
			"R213B": {"name": "缺口", "offset": 0, "sections": [{"from": 1, "to": 1}]}
		})JSON");
	write_file(dir / "commodities.json",
		R"JSON({
			"R213": {
				"CA": {"name": "A", "session": "R213A", "holiday": "R213_NONE"},
				"CB": {"name": "B", "session": "R213B", "holiday": "R213_NONE"}
			}
		})JSON");
	write_file(dir / "holidays.json", R"JSON({"R213_NONE": [19990101]})JSON");
}

} // namespace

/*
 * 生产: WtDtCore/StateMonitor.h:70-81、169-222
 * 直接构造 StateInfo 检查 isInSections 的四个返回点；initialize 后两个会话
 * 均为 SS_ORIGINAL，三个查询函数在原始状态上走完整遍历/命中/未命中分支。
 */
TEST(R213StateMonitor, HeaderQueriesAndSectionPredicates)
{
	StateInfo info;
	info._sections.push_back(StateInfo::Section{ 900, 1130 });
	info._sections.push_back(StateInfo::Section{ 1300, 1500 });

	EXPECT_TRUE(info.isInSections(900));   /* 左闭 */
	EXPECT_TRUE(info.isInSections(1000));
	EXPECT_FALSE(info.isInSections(1130)); /* 右开 */
	EXPECT_FALSE(info.isInSections(1200)); /* 两段之间 */
	EXPECT_TRUE(info.isInSections(1300));
	EXPECT_FALSE(info.isInSections(1500));
	EXPECT_FALSE(info.isInSections(0));

	StateInfo empty;
	EXPECT_FALSE(empty.isInSections(1000)); /* 空表走完循环返回 false */

	const fs::path dir = make_dir("query");
	write_valid_config(dir);
	write_file(dir / "state.json",
		R"JSON({
			"R213A": {"inittime": 0, "closetime": 2400, "proctime": 2400},
			"R213B": {"inittime": 0, "closetime": 2400, "proctime": 2400}
		})JSON");

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions((dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadHolidays((dir / "holidays.json").string().c_str()));

	DataManager dm;
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize((dir / "state.json").string().c_str(), &bdMgr, &dm));

	EXPECT_TRUE(mon.isInState("R213A", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("R213B", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("R213A", SS_Holiday));
	EXPECT_FALSE(mon.isInState("R213_MISSING", SS_ORIGINAL));

	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));

	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAllInState(SS_Holiday));
	EXPECT_FALSE(mon.isAllInState(SS_RECEIVING));

	std::error_code ec;
	fs::remove_all(dir, ec);
}

/*
 * 生产: WtDtCore/StateMonitor.cpp:38-74（initialize 的三个早退/跳过点）
 * 缺文件必须在 load_from_file 之前返回 false；空配置返回 true 且不建条目；
 * 配置里出现基础数据不存在的会话必须跳过，不影响其余条目。
 */
TEST(R213StateMonitor, InitializeGuardsAndUnknownSessionSkip)
{
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	StateMonitor mon;

	EXPECT_FALSE(mon.initialize("/r213/definitely/not/here/state.json", &bdMgr, &dm));

	const fs::path dir = make_dir("init");
	write_valid_config(dir);
	write_file(dir / "empty.json", R"JSON({})JSON");
	ASSERT_TRUE(mon.initialize((dir / "empty.json").string().c_str(), &bdMgr, &dm));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL)); /* 空表 */

	write_file(dir / "state.json",
		R"JSON({
			"R213A": {"inittime": 0, "closetime": 2400, "proctime": 2400},
			"R213_MISSING": {"inittime": 0, "closetime": 2400, "proctime": 2400}
		})JSON");
	ASSERT_TRUE(bdMgr.loadSessions((dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadHolidays((dir / "holidays.json").string().c_str()));

	StateMonitor mon2;
	ASSERT_TRUE(mon2.initialize((dir / "state.json").string().c_str(), &bdMgr, &dm));
	EXPECT_TRUE(mon2.isInState("R213A", SS_ORIGINAL));
	EXPECT_FALSE(mon2.isInState("R213_MISSING", SS_ORIGINAL));

	std::error_code ec;
	fs::remove_all(dir, ec);
}

/*
 * 生产: WtDtCore/ParserAdapter.cpp:88-116、150-204
 * cfg==NULL、module 缺失、模块装载失败、二次 init（_cfg 已占用）、
 * initExt 空 API、无 API 时 run 全部返回 false；id 在模块检查之前已写入。
 */
TEST(R213DtParserAdapter, GuardsBeforeAnyModuleIsLoaded)
{
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	ParserAdapter pa(&bdMgr, &dm, nullptr);

	EXPECT_FALSE(pa.init("r213_null_cfg", nullptr));

	WTSVariant* cfgEmpty = WTSVariant::createObject();
	EXPECT_FALSE(pa.init("r213_empty", cfgEmpty));
	EXPECT_STREQ("r213_empty", pa.id());
	/* _cfg 已被第一次 init 占用，二次 init 在模块检查之前就返回 false */
	EXPECT_FALSE(pa.init("r213_second", cfgEmpty));
	EXPECT_STREQ("r213_empty", pa.id());
	cfgEmpty->release();

	WTSVariant* cfgMissing = WTSVariant::createObject();
	cfgMissing->append("module", "r213_no_such_parser_module");
	ParserAdapter pb(&bdMgr, &dm, nullptr);
	EXPECT_FALSE(pb.init("r213_missing", cfgMissing));
	EXPECT_STREQ("r213_missing", pb.id());
	cfgMissing->release();

	ParserAdapter pc(&bdMgr, &dm, nullptr);
	EXPECT_FALSE(pc.initExt("r213_ext", nullptr));
	EXPECT_FALSE(pc.run());
	EXPECT_EQ(&bdMgr, pc.getBaseDataMgr());
	pc.handleParserLog(LL_INFO, "r213 parser log without api");
}

/*
 * 生产: WtDtCore/UDPCaster.cpp:52-96、212-250
 * inactive 配置在注册接收者之前返回 false；非法地址在地址解析处被捕获，
 * 两个 add*Recver 都不会创建 socket。
 */
TEST(R213UdpCaster, InactiveConfigAndAddressGuards)
{
	WTSBaseDataMgr bdMgr;
	DataManager dm;

	WTSVariant* cfg = WTSVariant::createObject();
	UDPCaster inactive;
	EXPECT_FALSE(inactive.init(cfg, &bdMgr, &dm));
	cfg->release();

	UDPCaster caster;
	EXPECT_TRUE(caster.addBRecver("127.0.0.1", 12345, 0));
	EXPECT_FALSE(caster.addBRecver("r213_not_an_ip", 12345, 0));
	EXPECT_FALSE(caster.addBRecver("256.256.256.256", 12345, 0));
	EXPECT_FALSE(caster.addMRecver("r213_not_an_ip", 12345, 12346, 0));
	caster.stop();
}

/*
 * 生产: WtDtCore/UDPCaster.cpp:268-296
 * 未 start（m_sktBroadcast==NULL）时四个 broadcast 重载都必须在 do_broadcast
 * 的守卫处直接返回：不建队列、不起发送线程、不解引用数据对象。
 */
TEST(R213UdpCaster, BroadcastBeforeStartIsNoop)
{
	UDPCaster caster;
	caster.broadcast(static_cast<WTSTickData*>(nullptr));
	caster.broadcast(static_cast<WTSOrdDtlData*>(nullptr));
	caster.broadcast(static_cast<WTSOrdQueData*>(nullptr));
	caster.broadcast(static_cast<WTSTransData*>(nullptr));
	caster.stop();
}
