/*
 * lane T4 —— ParserCTPMini 收发族用例
 *
 * 生产 TU：src/ParserCTPMini/ParserCTPMini.cpp（与本目录用例一起编进
 * CoverageTests_t_ParserCTPMini），可直接 new ParserCTPMini 并调 extern "C" 工厂。
 *
 * 刻意不进入的面（见 bugs/T4_femas_ctploader_mini.md）：
 *  - init()：内部 DLLHelper::load_library 会 dlopen 厂商行情库，且拿不到符号后
 *    仍然 m_funcCreator(...) 空指针调用 → 必崩；
 *  - OnRspUserLogin(bIsLast=true)：m_pUserAPI->GetTradingDay() 空指针解引用；
 *  - 未 registerSpi 就触发回调：构造函数没有初始化 m_sink / m_pBaseDataMgr（野指针）。
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

#include "ParserCTPMini/ParserCTPMini.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

extern "C" {
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

uint32_t strToTime(const char* strTime);

namespace {

class MiniBaseDataMgr : public IBaseDataMgr
{
public:
	WTSContractInfo*	m_contract = nullptr;
	WTSCommodityInfo*	m_comm = nullptr;
	uint32_t			m_calls = 0;

	WTSCommodityInfo* getCommodity(const char* /*exchgpid*/) override { return m_comm; }
	WTSCommodityInfo* getCommodity(const char* /*exchg*/, const char* /*pid*/) override { return m_comm; }
	WTSContractInfo* getContract(const char* /*code*/, const char* /*exchg*/, uint32_t /*uDate*/) override
	{
		m_calls++;
		return m_contract;
	}
	WTSArray* getContracts(const char* /*exchg*/, uint32_t /*uDate*/) override { return nullptr; }
	WTSSessionInfo* getSession(const char* /*sid*/) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/) override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char* /*pid*/, uint32_t /*uDate*/, bool /*isTpl*/) override { return false; }
	uint32_t calcTradingDate(const char* /*stdPID*/, uint32_t /*uDate*/, uint32_t /*uTime*/, bool /*isSession*/) override { return 0; }
	uint64_t getBoundaryTime(const char* /*stdPID*/, uint32_t /*tDate*/, bool /*isSession*/, bool /*isStart*/) override { return 0; }
};

class MiniParserSpi : public IParserSpi
{
public:
	MiniBaseDataMgr	m_mgr;

	struct Event { WTSParserEvent e; int32_t ec; };
	std::vector<Event> events;
	std::vector<std::string> logs;
	std::vector<WTSLogLevel> logLevels;

	uint32_t tickCount = 0;
	WTSTickStruct lastTick{};

	void handleEvent(WTSParserEvent e, int32_t ec) override { events.push_back({e, ec}); }

	void handleSymbolList(const WTSArray* /*aySymbols*/) override { symbolListCalls++; }
	uint32_t symbolListCalls = 0;

	void handleQuote(WTSTickData* quote, uint32_t procFlag) override
	{
		tickCount++;
		lastQuoteFlag = procFlag;
		if (quote != nullptr)
			lastTick = quote->getTickStruct();
	}
	uint32_t lastQuoteFlag = 0;

	void handleParserLog(WTSLogLevel ll, const char* message) override
	{
		logLevels.push_back(ll);
		logs.emplace_back(message == nullptr ? "" : message);
	}

	IBaseDataMgr* getBaseDataMgr() override { return &m_mgr; }
};

/* 造一个 CTPMini 深度行情包 */
CThostFtdcDepthMarketDataField makeDepth(const char* day, const char* time, uint32_t msec)
{
	CThostFtdcDepthMarketDataField d;
	memset(&d, 0, sizeof(d));
	strncpy(d.InstrumentID, "rb2401", sizeof(d.InstrumentID) - 1);
	strncpy(d.ExchangeID, "SHFE", sizeof(d.ExchangeID) - 1);
	strncpy(d.ActionDay, day, sizeof(d.ActionDay) - 1);
	strncpy(d.UpdateTime, time, sizeof(d.UpdateTime) - 1);
	d.UpdateMillisec = msec;
	d.LastPrice = 3521.0;
	d.OpenPrice = 3500.0;
	d.HighestPrice = 3550.0;
	d.LowestPrice = 3490.0;
	d.Volume = 120000;
	d.Turnover = 4.2e8;
	d.OpenInterest = 900000;
	d.UpperLimitPrice = 3700.0;
	d.LowerLimitPrice = 3300.0;
	d.PreClosePrice = 3495.0;
	d.PreSettlementPrice = 3499.0;
	d.PreOpenInterest = 880000;
	d.SettlementPrice = DBL_MAX;
	d.AskPrice1 = 3522.0; d.AskPrice2 = 3523.0; d.AskPrice3 = 3524.0; d.AskPrice4 = 3525.0; d.AskPrice5 = 3526.0;
	d.BidPrice1 = 3521.0; d.BidPrice2 = 3520.0; d.BidPrice3 = 3519.0; d.BidPrice4 = 3518.0; d.BidPrice5 = 3517.0;
	d.AskVolume1 = 10; d.AskVolume2 = 20; d.AskVolume3 = 30; d.AskVolume4 = 40; d.AskVolume5 = 50;
	d.BidVolume1 = 11; d.BidVolume2 = 22; d.BidVolume3 = 33; d.BidVolume4 = 44; d.BidVolume5 = 55;
	return d;
}

class ParserCTPMiniTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		comm = WTSCommodityInfo::create("rb", "Rebar", "SHFE", "SN001", "tpl");
		comm->setVolScale(10);
		contract = WTSContractInfo::create("rb2401", "Rebar2401", "SHFE", "rb");
		contract->setCommInfo(comm);
		sink.m_mgr.m_contract = contract;
		sink.m_mgr.m_comm = comm;
		parser.registerSpi(&sink);
	}

	void TearDown() override
	{
		parser.registerSpi(nullptr);
		sink.m_mgr.m_contract = nullptr;
		sink.m_mgr.m_comm = nullptr;
		if (contract != nullptr) { contract->release(); contract = nullptr; }
		if (comm != nullptr) { comm->release(); comm = nullptr; }
	}

	ParserCTPMini parser;
	MiniParserSpi sink;
	WTSCommodityInfo* comm = nullptr;
	WTSContractInfo* contract = nullptr;
};

}	// namespace

/* ------------------------------------------------------------------ */
/* 工厂生命周期                                                         */
/* ------------------------------------------------------------------ */

TEST(ParserCTPMiniFactory, create_and_delete_round_trip)
{
	IParserApi* api = createParser();
	ASSERT_NE(api, nullptr);
	ASSERT_NE(dynamic_cast<ParserCTPMini*>(api), nullptr);

	// 未 init 的工厂实例一定没有 API 句柄
	EXPECT_FALSE(api->isConnected());

	deleteParser(api);
	// deleteParser 会把入参置空（引用传递），这是防二次删除的关键契约
	EXPECT_EQ(api, nullptr);
	// 再删一次必须是安全的空操作
	deleteParser(api);
}

TEST(ParserCTPMiniFactory, lifecycle_calls_without_api_are_safe)
{
	ParserCTPMini parser;
	parser.registerSpi(nullptr);

	// m_pUserAPI 由构造函数初始化为 NULL，connect/disconnect 都只在非空时才解引用
	EXPECT_TRUE(parser.connect());
	EXPECT_TRUE(parser.disconnect());
	EXPECT_NO_THROW(parser.release());
	EXPECT_FALSE(parser.isConnected());
}

/* ------------------------------------------------------------------ */
/* strToTime                                                            */
/* ------------------------------------------------------------------ */

TEST(ParserCTPMiniStrToTime, strips_colons_and_keeps_digits)
{
	EXPECT_EQ(strToTime("09:30:15"), 93015u);
	EXPECT_EQ(strToTime("13:45:30"), 134530u);
	EXPECT_EQ(strToTime("00:00:00"), 0u);
	EXPECT_EQ(strToTime(""), 0u);
	EXPECT_EQ(strToTime("20:59:59"), 205959u);
}

/* ------------------------------------------------------------------ */
/* 行情回调 → WTSTickData 换算                                          */
/* ------------------------------------------------------------------ */

TEST_F(ParserCTPMiniTest, depth_market_data_is_converted_into_a_tick)
{
	CThostFtdcDepthMarketDataField d = makeDepth("20240105", "09:30:15", 250);

	parser.OnRtnDepthMarketData(&d);

	EXPECT_EQ(sink.tickCount, 1u);
	EXPECT_EQ(sink.lastQuoteFlag, 1u);
	EXPECT_STREQ(sink.lastTick.code, "rb2401");
	// exchg 取自品种信息而不是行情包
	EXPECT_STREQ(sink.lastTick.exchg, "SHFE");
	EXPECT_EQ(sink.lastTick.action_date, 20240105u);
	// 93015 * 1000 + 250
	EXPECT_EQ(sink.lastTick.action_time, 93015250u);
	EXPECT_DOUBLE_EQ(sink.lastTick.price, 3521.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.open, 3500.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.high, 3550.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.low, 3490.0);
	EXPECT_EQ(sink.lastTick.total_volume, 120000u);
	EXPECT_EQ(sink.lastTick.open_interest, 900000u);
	EXPECT_EQ(sink.lastTick.pre_interest, 880000u);
	EXPECT_DOUBLE_EQ(sink.lastTick.upper_limit, 3700.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.lower_limit, 3300.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.pre_close, 3495.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.pre_settle, 3499.0);
	// 非 CZCE 成交额原样透传
	EXPECT_DOUBLE_EQ(sink.lastTick.total_turnover, 4.2e8);
	// 未登录时 m_uTradingDate 仍是 0
	EXPECT_EQ(sink.lastTick.trading_date, 0u);
	// SettlementPrice == DBL_MAX -> 不覆盖结算价（保持结构体默认 0）
	EXPECT_DOUBLE_EQ(sink.lastTick.settle_price, 0.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.ask_prices[0], 3522.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.ask_prices[4], 3526.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.bid_prices[4], 3517.0);
	EXPECT_EQ(sink.lastTick.ask_qty[2], 30u);
	EXPECT_EQ(sink.lastTick.bid_qty[1], 22u);
	EXPECT_EQ(sink.m_mgr.m_calls, 1u);
}

TEST_F(ParserCTPMiniTest, invalid_prices_become_zero)
{
	CThostFtdcDepthMarketDataField d = makeDepth("20240105", "10:00:00", 0);
	d.LastPrice = DBL_MAX;
	d.OpenPrice = FLT_MAX;
	d.HighestPrice = DBL_MAX;
	d.LowestPrice = FLT_MAX;

	parser.OnRtnDepthMarketData(&d);

	ASSERT_EQ(sink.tickCount, 1u);
	EXPECT_DOUBLE_EQ(sink.lastTick.price, 0.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.open, 0.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.high, 0.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.low, 0.0);
	EXPECT_DOUBLE_EQ(sink.lastTick.ask_prices[0], 0.0);
}

TEST_F(ParserCTPMiniTest, czce_turnover_is_scaled_by_volume_multiple)
{
	// exchg 取自品种信息，所以换一个 CZCE 品种即可切进 turnover*volScale 分支
	WTSCommodityInfo* czceComm = WTSCommodityInfo::create("CF", "Cotton", "CZCE", "SN001", "tpl");
	czceComm->setVolScale(15);
	WTSContractInfo* czceCont = WTSContractInfo::create("CF2401", "Cotton2401", "CZCE", "CF");
	czceCont->setCommInfo(czceComm);
	sink.m_mgr.m_contract = czceCont;
	sink.m_mgr.m_comm = czceComm;

	CThostFtdcDepthMarketDataField d = makeDepth("20240105", "09:30:15", 0);
	d.Turnover = 1000.0;

	parser.OnRtnDepthMarketData(&d);

	ASSERT_EQ(sink.tickCount, 1u);
	EXPECT_STREQ(sink.lastTick.code, "CF2401");
	EXPECT_STREQ(sink.lastTick.exchg, "CZCE");
	// volScale = 15 -> 1000 * 15
	EXPECT_DOUBLE_EQ(sink.lastTick.total_turnover, 15000.0);

	sink.m_mgr.m_contract = contract;
	sink.m_mgr.m_comm = comm;
	czceCont->release();
	czceComm->release();
}

TEST_F(ParserCTPMiniTest, missing_action_day_falls_back_to_trading_date)
{
	CThostFtdcDepthMarketDataField d = makeDepth("", "09:30:15", 0);

	parser.OnRtnDepthMarketData(&d);

	// ActionDay 解析成 0 -> 用交易日（未登录就是 0）兜底；
	// 小时数 9 < 20，夜盘纠偏分支不会被触发，结果与挂钟无关
	ASSERT_EQ(sink.tickCount, 1u);
	EXPECT_EQ(sink.lastTick.action_date, 0u);
	EXPECT_EQ(sink.lastTick.trading_date, 0u);
}

TEST_F(ParserCTPMiniTest, unknown_contract_drops_the_tick)
{
	sink.m_mgr.m_contract = nullptr;
	CThostFtdcDepthMarketDataField d = makeDepth("20240105", "09:30:15", 0);

	parser.OnRtnDepthMarketData(&d);

	EXPECT_EQ(sink.tickCount, 0u);
	EXPECT_EQ(sink.m_mgr.m_calls, 1u);
}

TEST_F(ParserCTPMiniTest, quote_is_dropped_when_sink_unregistered)
{
	CThostFtdcDepthMarketDataField d = makeDepth("20240105", "09:30:15", 0);
	// registerSpi(nullptr) 只清 sink，m_pBaseDataMgr 仍然有效
	parser.registerSpi(nullptr);

	EXPECT_NO_THROW(parser.OnRtnDepthMarketData(&d));
	// 换算照常发生，只是最后没有投递对象
	EXPECT_EQ(sink.tickCount, 0u);
}

/* ------------------------------------------------------------------ */
/* 连接/登录/订阅回调                                                   */
/* ------------------------------------------------------------------ */

TEST_F(ParserCTPMiniTest, front_connected_publishes_event_and_survives_missing_api)
{
	parser.OnFrontConnected();

	ASSERT_EQ(sink.events.size(), 1u);
	EXPECT_EQ(sink.events[0].e, WPE_Connect);
	EXPECT_EQ(sink.events[0].ec, 0);
	ASSERT_FALSE(sink.logs.empty());
	EXPECT_NE(sink.logs.back().find("Market data server connected"), std::string::npos);
	// ReqUserLogin 在 m_pUserAPI == NULL 时直接 return，因此没有请求号自增，也不崩
	EXPECT_TRUE(sink.events.size() == 1u);
}

TEST_F(ParserCTPMiniTest, login_response_is_ignored_until_last_packet)
{
	CThostFtdcRspInfoField ok;
	memset(&ok, 0, sizeof(ok));
	ok.ErrorID = 0;

	CThostFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));

	// IsErrorRspInfo 是个恒返回 false 的桩函数，所以只有 bIsLast=false 才走得通
	parser.OnRspUserLogin(&login, &ok, 1, false);
	EXPECT_TRUE(sink.events.empty());
	EXPECT_EQ(sink.tickCount, 0u);
}

TEST_F(ParserCTPMiniTest, logout_and_close_events)
{
	CThostFtdcUserLogoutField logout;
	memset(&logout, 0, sizeof(logout));
	parser.OnRspUserLogout(&logout, nullptr, 1, true);

	ASSERT_EQ(sink.events.size(), 1u);
	EXPECT_EQ(sink.events[0].e, WPE_Logout);

	parser.OnFrontDisconnected(8);
	ASSERT_EQ(sink.events.size(), 2u);
	EXPECT_EQ(sink.events[1].e, WPE_Close);
	ASSERT_EQ(sink.logs.size(), 1u);
	EXPECT_EQ(sink.logLevels[0], LL_ERROR);
	EXPECT_NE(sink.logs[0].find("disconnected"), std::string::npos);
}

TEST_F(ParserCTPMiniTest, heartbeat_and_sub_response_paths_log_only)
{
	CThostFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));
	rsp.ErrorID = 42;
	CThostFtdcSpecificInstrumentField spec;
	memset(&spec, 0, sizeof(spec));
	strncpy(spec.InstrumentID, "rb2401", sizeof(spec.InstrumentID) - 1);

	parser.OnHeartBeatWarning(7);
	ASSERT_EQ(sink.logs.size(), 1u);
	EXPECT_EQ(sink.logLevels[0], LL_INFO);
	EXPECT_NE(sink.logs[0].find("elapse: 7"), std::string::npos);

	EXPECT_NO_THROW(parser.OnRspError(&rsp, 3, true));
	EXPECT_NO_THROW(parser.OnRspSubMarketData(&spec, &rsp, 4, true));
	EXPECT_NO_THROW(parser.OnRspUnSubMarketData(&spec, &rsp, 5, true));
	// 这三个回调都不该产生登录/订阅事件
	EXPECT_TRUE(sink.events.empty());
	EXPECT_EQ(sink.logs.size(), 1u);
}

TEST_F(ParserCTPMiniTest, subscribe_before_login_only_caches_the_filter)
{
	CodeSet codes;
	codes.insert("SHFE.rb2401");
	codes.insert("DCE.i2405");

	EXPECT_NO_THROW(parser.subscribe(codes));
	// m_uTradingDate == 0 -> 只缓存，不发订阅请求，因此没有任何日志/事件
	EXPECT_TRUE(sink.logs.empty());
	EXPECT_TRUE(sink.events.empty());
	EXPECT_FALSE(parser.isConnected());

	CodeSet empty;
	EXPECT_NO_THROW(parser.subscribe(empty));
	EXPECT_NO_THROW(parser.unsubscribe(codes));
}

/* ------------------------------------------------------------------ */
/* 崩溃路径：整条 DISABLED，说明见 bugs/T4_femas_ctploader_mini.md       */
/* ------------------------------------------------------------------ */

/*
 * BUG-T4-010：IsErrorRspInfo 是恒返回 false 的桩函数，
 * 于是 OnRspUserLogin 把任何应答（含错误应答）都当成功，
 * 紧接着 m_pUserAPI->GetTradingDay()（ParserCTPMini.cpp:183）→ 未 connect 必崩。
 */
TEST(ParserCTPMiniDisabled, DISABLED_login_success_branch_dereferences_null_api)
{
	ParserCTPMini parser;
	MiniParserSpi sink;
	parser.registerSpi(&sink);

	CThostFtdcRspInfoField err;
	memset(&err, 0, sizeof(err));
	err.ErrorID = 40;
	strncpy(err.ErrorMsg, "connect failed", sizeof(err.ErrorMsg) - 1);
	CThostFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));

	parser.OnRspUserLogin(&login, &err, 1, true);
	// 期望（修复后）：错误应答不该进入成功分支，因此不该有 WPE_Login
	EXPECT_TRUE(sink.events.empty());
}

/*
 * BUG-T4-008：构造函数没有初始化 m_sink / m_pBaseDataMgr，
 * 未 registerSpi 就触发任何回调都是野指针解引用 → 无法稳定断言，只能禁用。
 */
TEST(ParserCTPMiniDisabled, DISABLED_callback_before_register_spi_uses_wild_sink)
{
	ParserCTPMini parser;
	parser.OnFrontConnected();
}

/*
 * BUG-T4-009：init() 里 dlopen 厂商行情库；拿不到符号时 m_funcCreator 仍被直接调用
 * （ParserCTPMini.cpp:129-131）→ 空函数指针调用。与「不 dlopen」约束冲突，不启用。
 */
TEST(ParserCTPMiniDisabled, DISABLED_init_dlopens_vendor_module)
{
	ParserCTPMini parser;
	MiniParserSpi sink;
	parser.registerSpi(&sink);

	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"front: tcp://127.0.0.1:4444\nbroker: brk\nuser: usr\npass: pwd\n", true);
	ASSERT_NE(cfg, nullptr);
	parser.init(cfg);
	cfg->release();
}
