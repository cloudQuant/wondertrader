/*
 * r204_dtcore_test.cpp  (wave28 · lane R204)
 *
 * WtDtCore 四靶（UDPCaster 65 / StateMonitor.h 49 / StateMonitor.cpp 29 /
 * ParserAdapter 16）复核后可执行面：
 *  - StateMonitor.h:70-81、169-222 的 isInSections 与三个公开查询函数。
 *    一次 initialize 后即可在 ORIGINAL 状态上覆盖全部查询分支；再跑一个
 *    “连续交易 + 中间缺口”双会话，覆盖 isInSections 的 true/false 返回。
 *  - StateMonitor.cpp:491 夜盘交易时段在节假日次轮仍走
 *    “当前日期非交易日 + 夜盘后半夜”内层判断；用 offset=300、inittime=2000、
 *    今昨两日均休市的配置，让首轮进入 SS_Holiday、次轮进入该表达式。
 *  - StateMonitor.cpp:341-368/392-423/445-449 需要“已处于 RECEIVING/PAUSED”
 *    的持久状态或已处理收盘的 writer，进程内冷启动无法到达；UDPCaster 的
 *    热订阅 184-200 需要 DataManager 有已缓存 tick（writer 线程），均留白。
 *  - ParserAdapter.cpp:194-202 需要插件 api->init 返回 false，仓内无此插件。
 *
 * 边界：StateMonitor 线程由产品自身创建，观察后立即 stop（单测约 1.2s）；
 * 不建真实 shm、不网连、不加载解析插件；UDP 只做地址解析失败路径，不 bind。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/ParserAdapter.h>
#include <WtDtCore/StateMonitor.h>
#include <WtDtCore/UDPCaster.h>

#include <WTSTools/WTSBaseDataMgr.h>

#include <Includes/WTSVariant.hpp>
#include <TimeUtils.hpp>

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* tag)
{
	fs::path p = fs::temp_directory_path()
		/ (std::string("r204_dtcore_") + tag + "_" + std::to_string((long)::getpid()));
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

std::string today_window()
{
	const uint32_t today = TimeUtils::getCurDate();
	return std::string("[") + std::to_string(TimeUtils::getNextDate(today, -1))
		+ "," + std::to_string(today) + "]";
}

std::string gap_sections(uint32_t nowMin)
{
	std::string ret = "[";
	bool first = true;
	if (nowMin > 2)
	{
		ret += "{\"from\":1,\"to\":" + std::to_string(nowMin - 1) + "}";
		first = false;
	}
	if (nowMin < 2357)
	{
		if (!first)
			ret += ",";
		ret += "{\"from\":" + std::to_string(nowMin + 2) + ",\"to\":2359}";
		first = false;
	}
	if (first)
		ret += "{\"from\":1,\"to\":1}";
	ret += "]";
	return ret;
}

bool wait_state(StateMonitor& mon, const char* sid, SimpleState ss, int spins = 400)
{
	for (int i = 0; i < spins; i++)
	{
		if (mon.isInState(sid, ss))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

} // namespace

/*
 * 生产: WtDtCore/StateMonitor.h:70-81、169-222
 * 两个会话都停在 SS_ORIGINAL：isAny/isAll 走完整遍历的真/假返回，
 * isInState 分别命中、状态不符、键不存在。
 */
TEST(R204StateMonitor, QueryPredicatesOnOriginalStates)
{
	const fs::path dir = make_dir("query");
	write_file(dir / "sessions.json",
		R"JSON({
			"R204A": {"name": "连续", "offset": 0, "sections": [{"from": 1, "to": 2359}]},
			"R204B": {"name": "缺口", "offset": 0, "sections": [{"from": 1, "to": 1}]}
		})JSON");
	write_file(dir / "commodities.json",
		R"JSON({
			"R204": {
				"CA": {"name": "A", "session": "R204A", "holiday": "R204_NONE"},
				"CB": {"name": "B", "session": "R204B", "holiday": "R204_NONE"}
			}
		})JSON");
	write_file(dir / "holidays.json", R"JSON({"R204_NONE": [19990101]})JSON");
	write_file(dir / "state.json",
		R"JSON({
			"R204A": {"inittime": 0, "closetime": 2400, "proctime": 2400},
			"R204B": {"inittime": 0, "closetime": 2400, "proctime": 2400}
		})JSON");

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions((dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadHolidays((dir / "holidays.json").string().c_str()));

	DataManager dm;
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize((dir / "state.json").string().c_str(), &bdMgr, &dm));

	/* 未 run：两会话均为 ORIGINAL */
	EXPECT_TRUE(mon.isInState("R204A", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("R204B", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("R204A", SS_Holiday));
	EXPECT_FALSE(mon.isInState("R204_MISSING", SS_ORIGINAL));

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
 * 生产: WtDtCore/StateMonitor.h:70-81（isInSections 的循环）
 *       WtDtCore/StateMonitor.cpp:288-330（ORIGINAL/INITIALIZED 转换）
 * A 会话全天连续：分钟级第二轮进入 RECEIVING，isInSections 返回 true；
 * B 会话在“当前分钟”两侧留缺口：同一轮 isInSections 走完整循环后返回 false。
 * 断言只等 A 的状态，不依赖具体钟点。
 */
TEST(R204StateMonitor, RunCoversContinuousAndGapSections)
{
	/* isTradingDate 对周末恒为 false，本用例需要“今天可交易”才能进入 RECEIVING。 */
	const uint32_t wd = TimeUtils::getWeekDay();
	if (wd == 0 || wd == 6)
		GTEST_SKIP() << "weekend: trading session would go holiday on the first round";

	const uint32_t nowMin = TimeUtils::getCurMin() / 100;
	const fs::path dir = make_dir("run");

	write_file(dir / "sessions.json",
		std::string(R"JSON({
			"R204A": {"name": "连续", "offset": 0, "sections": [{"from": 1, "to": 2359}]},
			"R204B": {"name": "缺口", "offset": 0, "sections": )JSON")
		+ gap_sections(nowMin) + "}}");
	write_file(dir / "commodities.json",
		R"JSON({
			"R204": {
				"CA": {"name": "A", "session": "R204A", "holiday": "R204_NONE"},
				"CB": {"name": "B", "session": "R204B", "holiday": "R204_NONE"}
			}
		})JSON");
	write_file(dir / "holidays.json", R"JSON({"R204_NONE": [19990101]})JSON");
	write_file(dir / "state.json",
		R"JSON({
			"R204A": {"inittime": 0, "closetime": 2400, "proctime": 2400},
			"R204B": {"inittime": 0, "closetime": 2400, "proctime": 2400}
		})JSON");

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions((dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadHolidays((dir / "holidays.json").string().c_str()));

	DataManager dm;
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize((dir / "state.json").string().c_str(), &bdMgr, &dm));

	mon.run();
	const bool saw = wait_state(mon, "R204A", SS_RECEIVING);

	EXPECT_TRUE(saw);
	EXPECT_TRUE(mon.isAnyInState(SS_RECEIVING));
	EXPECT_FALSE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("R204A", SS_ORIGINAL));

	mon.stop();

	std::error_code ec;
	fs::remove_all(dir, ec);
}

/*
 * 生产: WtDtCore/StateMonitor.cpp:468-513（SS_Holiday 次轮）
 * offset=300 的夜盘会话不覆盖当前分钟；今昨两日都休市，首轮必进 SS_Holiday。
 * inittime=2000 使次轮满足 offTime<offInitTime，逐品种检查执行 489-491 的
 * “当前日非交易日 + 夜盘后半夜”内层表达式，并保持节假日。
 */
TEST(R204StateMonitor, HolidayNightSessionSecondRound)
{
	const fs::path dir = make_dir("holiday");
	write_file(dir / "sessions.json",
		R"JSON({
			"R204H": {"name": "夜盘节假日", "offset": 300,
				"sections": [{"from": 1, "to": 1}]}
		})JSON");
	write_file(dir / "commodities.json",
		R"JSON({
			"R204": {"HN": {"name": "夜盘品种", "session": "R204H", "holiday": "R204_HOL"}}
		})JSON");
	write_file(dir / "holidays.json", std::string("{\"R204_HOL\":") + today_window() + "}");
	write_file(dir / "state.json",
		R"JSON({"R204H": {"inittime": 2000, "closetime": 2400, "proctime": 2400}})JSON");

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions((dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadHolidays((dir / "holidays.json").string().c_str()));

	DataManager dm;
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize((dir / "state.json").string().c_str(), &bdMgr, &dm));

	/* 未 run 时状态为 ORIGINAL；isAllInState 会忽略节假日条目，故此处仍为真 */
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));

	mon.run();
	const bool saw = wait_state(mon, "R204H", SS_Holiday);
	/* 第二轮由产品自身 >=1s 的节流决定；等足一个周期后再观察。 */
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));

	EXPECT_TRUE(saw);
	EXPECT_TRUE(mon.isInState("R204H", SS_Holiday));
	EXPECT_TRUE(mon.isAnyInState(SS_Holiday));
	EXPECT_TRUE(mon.isAllInState(SS_Holiday));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("R204H", SS_RECEIVING));

	mon.stop();

	std::error_code ec;
	fs::remove_all(dir, ec);
}

/*
 * 生产: WtDtCore/ParserAdapter.cpp:92-116、194-204
 * cfg==NULL、module 缺失、initExt 空 API、无 API 时 run 都必须返回 false；
 * 模块缺失早退发生在任何 base/DataManager 访问之前。
 */
TEST(R204DtParserAdapter, NullAndMissingInputsAreRejected)
{
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	ParserAdapter pa(&bdMgr, &dm, nullptr);

	EXPECT_FALSE(pa.init("r204_null", nullptr));

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r204_missing_parser_module");
	EXPECT_FALSE(pa.init("r204_null", cfg));
	EXPECT_STREQ("r204_null", pa.id());

	EXPECT_FALSE(pa.initExt("r204_null_api", nullptr));
	EXPECT_FALSE(pa.run());

	cfg->release();
}

/*
 * 生产: WtDtCore/UDPCaster.cpp:52-96、220-250
 * inactive 配置在注册任何接收者前返回 false；非法地址在 make_address_v4
 * 抛出后由两个 add*Recver 捕获，未创建 socket、未启动线程。
 */
TEST(R204UdpCaster, InactiveInitAndInvalidAddressesAreRejected)
{
	WTSBaseDataMgr bdMgr;
	DataManager dm;
	WTSVariant* cfg = WTSVariant::createObject();

	UDPCaster inactive;
	EXPECT_FALSE(inactive.init(cfg, &bdMgr, &dm));
	cfg->release();

	UDPCaster caster;
	EXPECT_FALSE(caster.addBRecver("r204_not_an_ip", 12345, 0));
	EXPECT_FALSE(caster.addBRecver("256.256.256.256", 12345, 0));
	EXPECT_FALSE(caster.addMRecver("r204_not_an_ip", 12345, 12346, 0));
	caster.stop();
}
