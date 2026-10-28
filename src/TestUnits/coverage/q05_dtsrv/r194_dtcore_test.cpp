/*
 * r194_dtcore_test.cpp  (wave27 · lane R194)
 *
 * 靶子（WtDtCore 残余：UDPCaster 65、StateMonitor.h 49、StateMonitor 29、
 * ParserAdapter 16）中仍可由公开 API 安全触达的部分：
 *  - StateMonitor.cpp:380-405/418-423  SS_Holiday 分支的第二轮检查。
 *    既有 r145 只跑到“进入 SS_Holiday”就立即 stop；本用例让监控线程多跑一轮
 *    （>1s 的秒级节流），命中“节假日品种仍为节假日”的逐品种循环与结尾的
 *    SS_Holiday 重写。断言只看仍在节假日，不依赖具体钟点。
 *  - StateMonitor.h:169-222 三个公开查询函数的空表/命中路径（功能面）。
 *  - UDPCaster.cpp:220-250 两个 add*Recver 的地址解析异常分支（无 socket）。
 *  - ParserAdapter.cpp:60-140 缺模块早退（不加载任何真实解析插件）。
 *
 * 边界：StateMonitor 线程由产品自身创建，观察后立即 stop（总生命周期约 1.2s）；
 * 不建 socket、不连柜台、不读写 shm，临时目录仅系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
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

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <TimeUtils.hpp>

namespace {

namespace fs = std::filesystem;

fs::path make_dir(const char* tag)
{
	fs::path p = fs::temp_directory_path()
		/ (std::string("r194_dtcore_") + tag + "_" + std::to_string((long)::getpid()));
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
		+ "," + std::to_string(today)
		+ "," + std::to_string(TimeUtils::getNextDate(today, 1)) + "]";
}

bool wait_holiday(StateMonitor& mon, const char* sid, int spins = 2000000)
{
	for (int i = 0; i < spins; i++)
	{
		if (mon.isInState(sid, SS_Holiday))
			return true;
		std::this_thread::yield();
	}
	return false;
}

} // namespace

/*
 * 生产: StateMonitor.cpp:376-405、418-423（SS_Holiday 下一轮）
 * 节假日模板包含今天 ±1 天，品种 HLC 的会话 HLM 无行情；首轮进入 SS_Holiday，
 * 第二轮仍走“逐品种 isTradingDate==false”的日志支与结尾的 SS_Holiday 重写。
 */
TEST(R194StateMonitor, HolidayCommodityStaysHolidayOnSecondRound)
{
	const fs::path dir = make_dir("holiday");
	write_file(dir / "sessions.json",
		R"JSON({
			"HLM": {"name": "节假日会话", "offset": 0,
				"sections": [{"from": 1, "to": 2359}]}
		})JSON");
	write_file(dir / "commodities.json",
		R"JSON({
			"R194": {"HLC": {"name": "节假日品种", "session": "HLM", "holiday": "R194_HOL"}}
		})JSON");
	write_file(dir / "holidays.json", std::string("{\"R194_HOL\":") + today_window() + "}");

	WTSBaseDataMgr bdMgr;
	ASSERT_TRUE(bdMgr.loadSessions((dir / "sessions.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadCommodities((dir / "commodities.json").string().c_str()));
	ASSERT_TRUE(bdMgr.loadHolidays((dir / "holidays.json").string().c_str()));

	DataManager dm;	// 未 init：_writer==NULL，不会转存任何行情
	write_file(dir / "state.json",
		R"JSON({"HLM": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");

	StateMonitor mon;
	ASSERT_TRUE(mon.initialize((dir / "state.json").string().c_str(), &bdMgr, &dm));

	mon.run();
	const bool saw = wait_holiday(mon, "HLM");
	/* 第二轮由产品自身 >=1s 的节流决定；这里等足一个周期后再观察。 */
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));

	EXPECT_TRUE(saw);
	EXPECT_TRUE(mon.isInState("HLM", SS_Holiday));
	EXPECT_TRUE(mon.isAnyInState(SS_Holiday));
	EXPECT_TRUE(mon.isAllInState(SS_Holiday));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("R194_MISSING", SS_Holiday));

	mon.stop();

	std::error_code ec;
	fs::remove_all(dir, ec);
}

/*
 * 生产: UDPCaster.cpp:220-250（addBRecver/addMRecver 的地址解析 catch）
 * "r194_not_an_ip" 在 make_address_v4 抛异常，两个入口都应在建 socket 之前返回 false；
 * 随后 stop() 只停 io_service，不 join 任何线程。
 */
TEST(R194UdpCaster, InvalidReceiverAddressesAreRejected)
{
	UDPCaster caster;
	EXPECT_FALSE(caster.addBRecver("r194_not_an_ip", 12345, 0));
	EXPECT_FALSE(caster.addMRecver("r194_not_an_ip", 12345, 12346, 0));
	EXPECT_FALSE(caster.addBRecver("256.256.256.256", 12345, 0));
	caster.stop();
}

/*
 * 生产: ParserAdapter.cpp:60-140（module 缺失/装载失败早退）
 * 不存在的模块名只会让 DLLHelper::load_library 失败并返回 false；
 * 该早退发生在任何 base/DataManager 使用之前，因此两者可传空。
 */
TEST(R194DtParserAdapter, MissingModuleIsRejectedBeforeAnyDataAccess)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r194_missing_parser_module");

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	ParserAdapter pa(&bdMgr, &dm, nullptr);

	EXPECT_FALSE(pa.init("r194", cfg));
	EXPECT_STREQ("r194", pa.id());

	cfg->release();
}

/*
 * 生产: ParserAdapter.cpp:301-309（合约/写手守卫与 _idx_fact 分支前的返回）
 * 未装载写手且合约未知时，handleQuote 必须在 writeTick 前返回，不触碰 idxFactory。
 */
TEST(R194DtParserAdapter, QuoteWithoutWriterOrContractReturnsBeforeIndex)
{
	WTSBaseDataMgr bdMgr;
	DataManager dm;	// _writer==NULL
	ParserAdapter pa(&bdMgr, &dm, nullptr);

	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	strcpy(ts.code, "R194");
	strcpy(ts.exchg, "NONE");
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	WTSTickData* tick = WTSTickData::create(ts);

	pa.handleQuote(tick, 1);

	tick->release();
}
