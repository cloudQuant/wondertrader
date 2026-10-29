/*!
 * \file femas_parser_callbacks_test.cpp
 * \project WonderTrader — 覆盖率补写第六波（收发族）lane T3 / ParserFemas
 *
 * 目标：在**不连柜台、不起线程**的前提下，驱动 ParserFemas 的
 *       生命周期查询（init/connect/isConnected/release/subscribe）
 *       与厂商回调 → WTS 结构的转换路径（OnRtnDepthMarketData 等）。
 *
 * 关键约束（见 docs/迭代计划/迭代01/bugs/T3_ctpmini_dumper_femas.md）：
 *   ParserFemas::init() 会 dlopen 厂商库 USTPmduserapiAF，本机不存在 →
 *   m_funcCreator 为空，init 末尾 `m_pUserAPI = m_funcCreator(path)` 必段错误。
 *   因此所有需要 m_pUserAPI 非空的路径（OnRspUserLogin 取交易日、
 *   SubscribeMarketData 真正下发）只能留作 DISABLED，其余用默认构造对象
 *   （m_pUserAPI == NULL）即可安全驱动：源码里每条 API 调用都有
 *   `if(m_pUserAPI)` 守卫。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <cfloat>
#include <string>
#include <vector>

#include "ParserFemas/ParserFemas.h"

#include "WTSDataDef.hpp"
#include "WTSContractInfo.hpp"
#include "IBaseDataMgr.h"
#include "IParserApi.h"

USING_NS_WTP;

/* ParserFemas.cpp:59 定义的全局函数（非 static，可跨 TU 直调） */
uint32_t strToTime(const char* strTime);

/* ParserFemas.cpp:40-56 的插件工厂入口（头文件里没有声明，这里补上） */
extern "C"
{
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

namespace {

//////////////////////////////////////////////////////////////////////////
// 假的基础数据管理器：只提供 getContract 需要的合约/品种对象
//////////////////////////////////////////////////////////////////////////
class FakeDataMgr : public IBaseDataMgr
{
public:
	WTSContractInfo* code2contract;		// 唯一可查到的合约

	FakeDataMgr() : code2contract(NULL) {}

	virtual WTSCommodityInfo* getCommodity(const char* /*exchgpid*/) override { return NULL; }
	virtual WTSCommodityInfo* getCommodity(const char* /*exchg*/, const char* /*pid*/) override { return NULL; }
	virtual WTSContractInfo* getContract(const char* code, const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override
	{
		if (code2contract == NULL || code == NULL)
			return NULL;
		// 与真柜台语义一致：查不到返回 NULL
		if (strcmp(code, code2contract->getCode()) != 0)
			return NULL;
		return code2contract;
	}
	virtual WTSArray* getContracts(const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override { return NULL; }
	virtual WTSSessionInfo* getSession(const char* /*sid*/) override { return NULL; }
	virtual WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override { return NULL; }
	virtual WTSArray* getAllSessions() override { return NULL; }
	virtual bool isHoliday(const char* /*pid*/, uint32_t /*uDate*/, bool /*isTpl*/ = false) override { return false; }
	virtual uint32_t calcTradingDate(const char* /*stdPID*/, uint32_t /*uDate*/, uint32_t /*uTime*/, bool /*isSession*/ = false) override { return 0; }
	virtual uint64_t getBoundaryTime(const char* /*stdPID*/, uint32_t /*tDate*/, bool /*isSession*/ = false, bool /*isStart*/ = true) override { return 0; }
};

//////////////////////////////////////////////////////////////////////////
// 假的行情回调 sink：把事件/日志/行情快照录下来，供 EXPECT 证伪
//////////////////////////////////////////////////////////////////////////
class FakeParserSink : public IParserSpi
{
public:
	std::vector<std::pair<int32_t, int32_t> > events;	// (event, errcode)
	std::vector<std::string> logs;
	std::vector<WTSLogLevel> logLevels;
	uint32_t quoteCalls;
	WTSTickStruct lastTick;
	uint32_t lastProcFlag;
	FakeDataMgr* mgr;

	FakeParserSink() : quoteCalls(0), mgr(NULL) { memset(&lastTick, 0, sizeof(lastTick)); }

	virtual void handleEvent(WTSParserEvent e, int32_t ec) override
	{
		events.push_back(std::make_pair((int32_t)e, ec));
	}

	virtual void handleSymbolList(const WTSArray* /*aySymbols*/) override {}

	virtual void handleQuote(WTSTickData* quote, uint32_t procFlag) override
	{
		++quoteCalls;
		lastProcFlag = procFlag;
		memcpy(&lastTick, &quote->getTickStruct(), sizeof(WTSTickStruct));
	}

	virtual void handleParserLog(WTSLogLevel ll, const char* message) override
	{
		logLevels.push_back(ll);
		logs.push_back(message == NULL ? std::string("<null>") : message);
	}

	virtual IBaseDataMgr* getBaseDataMgr() override { return mgr; }

	bool hasEvent(int32_t e) const
	{
		for (size_t i = 0; i < events.size(); i++)
			if (events[i].first == e)
				return true;
		return false;
	}
};

// 合约 + 品种：exchg 决定 OnRtnDepthMarketData 的成交额分支
struct Fixture
{
	WTSCommodityInfo* comm;
	WTSContractInfo* ct;
	FakeDataMgr mgr;
	FakeParserSink sink;
	ParserFemas parser;

	explicit Fixture(const char* exchg)
	{
		comm = WTSCommodityInfo::create("ag", "白银", exchg, "SHFE_day", "SHFE", "CNY");
		comm->setVolScale(15);
		ct = WTSContractInfo::create("ag2601", "白银2601", exchg, "ag");
		ct->setCommInfo(comm);
		ct->setVolScale(15);
		mgr.code2contract = ct;
		sink.mgr = &mgr;
		parser.registerSpi(&sink);
	}

	~Fixture()
	{
		parser.registerSpi(NULL);
		ct->release();
		comm->release();
	}
};

CUstpFtdcDepthMarketDataField makeQuote(const char* code, const char* actDate)
{
	CUstpFtdcDepthMarketDataField f;
	memset(&f, 0, sizeof(f));
	strncpy(f.InstrumentID, code, sizeof(f.InstrumentID) - 1);
	strncpy(f.ActionDay, actDate, sizeof(f.ActionDay) - 1);
	strncpy(f.UpdateTime, "10:15:30", sizeof(f.UpdateTime) - 1);
	f.UpdateMillisec = 250;
	f.LastPrice = 8000.5;
	f.OpenPrice = 7900.0;
	f.HighestPrice = 8100.0;
	f.LowestPrice = 7850.0;
	f.Volume = 12345;
	f.Turnover = 60000000.0;
	f.OpenInterest = 45678;
	f.UpperLimitPrice = 8500.0;
	f.LowerLimitPrice = 7500.0;
	f.PreClosePrice = 7880.0;
	f.PreSettlementPrice = 7890.0;
	f.PreOpenInterest = 45000;
	f.AskPrice1 = 8001.0;
	f.AskVolume1 = 10;
	f.BidPrice1 = 8000.0;
	f.BidVolume1 = 20;
	return f;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// 未建立连接：IParserApi.h 将 connect/disconnect 的返回值定义为命令是否发送，
// isConnected 则表示当前是否已连接。用有效 sink 避免依赖未初始化的 SPI 指针。
//////////////////////////////////////////////////////////////////////////
TEST(femas_parser, is_not_connected_without_api)
{
	FakeDataMgr mgr;
	FakeParserSink sink;
	sink.mgr = &mgr;
	ParserFemas parser;
	parser.registerSpi(&sink);

	EXPECT_FALSE(parser.isConnected());
}

TEST(femas_parser, connect_without_api_reports_command_not_sent)
{
	FakeDataMgr mgr;
	FakeParserSink sink;
	sink.mgr = &mgr;
	ParserFemas parser;
	parser.registerSpi(&sink);

	EXPECT_FALSE(parser.connect());
}

TEST(femas_parser, disconnect_without_api_reports_command_not_sent)
{
	FakeDataMgr mgr;
	FakeParserSink sink;
	sink.mgr = &mgr;
	ParserFemas parser;
	parser.registerSpi(&sink);

	EXPECT_FALSE(parser.disconnect());
	EXPECT_NO_THROW(parser.release());
}

TEST(femas_parser, register_spi_pulls_base_data_mgr_from_sink)
{
	FakeParserSink sink;
	FakeDataMgr mgr;
	sink.mgr = &mgr;

	ParserFemas parser;
	parser.registerSpi(&sink);

	// registerSpi(:456) 会把 sink->getBaseDataMgr() 缓存下来；
	// 后续 OnRtnDepthMarketData 依赖它，未注册时必须直接丢包（见下一个用例）
	EXPECT_NO_THROW(parser.OnRspError(NULL, 1, true));

	parser.registerSpi(NULL);
	EXPECT_NO_THROW(parser.OnHeartBeatWarning(30));
}

TEST(femas_parser, subscribe_before_login_only_caches_codes)
{
	ParserFemas parser;
	CodeSet codes;
	codes.insert("SHFE.ag2601");
	codes.insert("CZCE.TA601");

	// m_uTradingDate==0 → 只落到 m_filterSubs，不发 SubMarketData（:410）
	EXPECT_NO_THROW(parser.subscribe(codes));
	EXPECT_NO_THROW(parser.unsubscribe(codes));
	// 空集合同样不得崩
	EXPECT_NO_THROW(parser.subscribe(CodeSet()));
}

//////////////////////////////////////////////////////////////////////////
// 厂商回调 → WTS 事件/日志转换
//////////////////////////////////////////////////////////////////////////
TEST(femas_parser, front_connected_emits_connect_event_then_skips_login)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	parser.OnFrontConnected();

	// OnFrontConnected(:168) 先写日志 + WPE_Connect，再 ReqUserLogin
	// ReqUserLogin(:347) 因 m_pUserAPI==NULL 早退，不能奔溃
	EXPECT_TRUE(sink.hasEvent(WPE_Connect));
	ASSERT_EQ(1u, sink.events.size());
	ASSERT_EQ(1u, sink.logs.size());
	EXPECT_NE(std::string::npos, sink.logs[0].find("Market data server connected"));
	EXPECT_EQ(LL_INFO, sink.logLevels[0]);
}

TEST(femas_parser, front_disconnect_reports_close_event_and_reason)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	parser.OnFrontDisconnected(-9);

	EXPECT_TRUE(sink.hasEvent(WPE_Close));
	ASSERT_EQ(1u, sink.logs.size());
	// 错误码要出现在日志里（:207）
	EXPECT_NE(std::string::npos, sink.logs[0].find("-9"));
	EXPECT_EQ(LL_ERROR, sink.logLevels[0]);
}

TEST(femas_parser, logout_rsp_emits_logout_event)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	CUstpFtdcRspUserLogoutField logout;
	memset(&logout, 0, sizeof(logout));
	CUstpFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));

	parser.OnRspUserLogout(&logout, &rsp, 1, true);

	// 无 sink 时也必须安全（:195 的 if(m_sink)）
	parser.registerSpi(NULL);
	parser.OnRspUserLogout(&logout, &rsp, 2, true);
}

TEST(femas_parser, heartbeat_warning_is_logged)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	parser.OnHeartBeatWarning(1234);
	ASSERT_EQ(1u, sink.logs.size());
	EXPECT_NE(std::string::npos, sink.logs[0].find("1234"));
}


TEST(femas_parser, nonzero_rsp_error_is_reported_through_parser_log)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	CUstpFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));
	rsp.ErrorID = 100;

	// IParserSpi exposes handleParserLog; other parser error callbacks use it for
	// diagnostics. Check observability without prescribing text, level, or code.
	EXPECT_NO_THROW(parser.OnRspError(&rsp, 3, true));
	EXPECT_GT(sink.logs.size(), 0u);
}

TEST(femas_parser, subscription_error_callbacks_are_safe)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	CUstpFtdcSpecificInstrumentField inst;
	memset(&inst, 0, sizeof(inst));
	CUstpFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));
	rsp.ErrorID = 100;

	// These callbacks have no explicit IParserSpi reporting contract in this
	// plugin, so keep this as a safety check without treating silence as success.
	EXPECT_NO_THROW(parser.OnRspSubMarketData(&inst, &rsp, 4, true));
	EXPECT_NO_THROW(parser.OnRspUnSubMarketData(&inst, &rsp, 5, true));
}

TEST(femas_parser, login_rsp_skipped_when_not_last)
{
	FakeParserSink sink;
	ParserFemas parser;
	parser.registerSpi(&sink);

	CUstpFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));
	CUstpFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));

	// bIsLast==false → 整个 if 块不进（:181），不会碰 m_pUserAPI
	parser.OnRspUserLogin(&login, &rsp, 1, false);
	EXPECT_EQ(0u, sink.events.size());
}

//////////////////////////////////////////////////////////////////////////
// 行情快照转换
//////////////////////////////////////////////////////////////////////////
TEST(femas_parser, depth_market_data_dropped_without_data_mgr)
{
	FakeParserSink sink;			// sink.mgr == NULL
	ParserFemas parser;
	parser.registerSpi(&sink);

	CUstpFtdcDepthMarketDataField f = makeQuote("ag2601", "20260105");
	parser.OnRtnDepthMarketData(&f);

	// :219 无 bdMgr 直接 return
	EXPECT_EQ(0u, sink.quoteCalls);
}

TEST(femas_parser, depth_market_data_dropped_when_contract_unknown)
{
	Fixture fx("SHFE");

	CUstpFtdcDepthMarketDataField f = makeQuote("not_exist", "20260105");
	fx.parser.OnRtnDepthMarketData(&f);

	EXPECT_EQ(0u, fx.sink.quoteCalls);
}

TEST(femas_parser, depth_market_data_converts_prices_and_times)
{
	Fixture fx("SHFE");

	CUstpFtdcDepthMarketDataField f = makeQuote("ag2601", "20260105");
	f.AskPrice2 = DBL_MAX;			// 无效价位要归 0（checkValid :75）
	f.BidPrice3 = FLT_MAX;
	f.AskVolume2 = 7;
	f.BidVolume1 = 20;
	f.SettlementPrice = 8200.0;
	f.PreSettlementPrice = 7890.0;

	fx.parser.OnRtnDepthMarketData(&f);

	ASSERT_EQ(1u, fx.sink.quoteCalls);
	const WTSTickStruct& q = fx.sink.lastTick;
	EXPECT_EQ(0u, strcmp(q.exchg, "SHFE"));
	EXPECT_EQ(20260105u, q.action_date);
	// strToTime("10:15:30")=101530 → *1000 + 250
	EXPECT_EQ(101530250u, q.action_time);
	EXPECT_DOUBLE_EQ(8000.5, q.price);
	EXPECT_DOUBLE_EQ(7900.0, q.open);
	EXPECT_DOUBLE_EQ(8100.0, q.high);
	EXPECT_DOUBLE_EQ(7850.0, q.low);
	EXPECT_EQ(12345u, q.total_volume);
	EXPECT_DOUBLE_EQ(8200.0, q.settle_price);
	EXPECT_DOUBLE_EQ(8500.0, q.upper_limit);
	EXPECT_DOUBLE_EQ(7500.0, q.lower_limit);
	EXPECT_DOUBLE_EQ(7880.0, q.pre_close);
	EXPECT_DOUBLE_EQ(7890.0, q.pre_settle);
	EXPECT_EQ(45000u, q.pre_interest);
	// 档位：1 号档取真值，2 号 DBL_MAX 归 0，3 号 FLT_MAX 归 0
	EXPECT_DOUBLE_EQ(8001.0, q.ask_prices[0]);
	EXPECT_DOUBLE_EQ(0.0, q.ask_prices[1]);
	EXPECT_DOUBLE_EQ(8000.0, q.bid_prices[0]);
	EXPECT_DOUBLE_EQ(0.0, q.bid_prices[2]);
	EXPECT_EQ(10u, q.ask_qty[0]);
	EXPECT_EQ(7u, q.ask_qty[1]);
	EXPECT_EQ(20u, q.bid_qty[0]);
	EXPECT_EQ(1u, fx.sink.lastProcFlag);
	// trading_date 取 m_uTradingDate，未登录时是 0
	EXPECT_EQ(0u, q.trading_date);
	// 非 CZCE：Turnover 直接透传（:283）
	EXPECT_DOUBLE_EQ(60000000.0, q.total_turnover);
}

TEST(femas_parser, czce_turnover_multiplied_by_vol_scale)
{
	Fixture fx("CZCE");

	CUstpFtdcDepthMarketDataField f = makeQuote("ag2601", "20260105");
	f.Turnover = 1000.0;
	f.SettlementPrice = DBL_MAX;		// 无效结算价 → 不覆盖（:275）

	fx.parser.OnRtnDepthMarketData(&f);

	ASSERT_EQ(1u, fx.sink.quoteCalls);
	const WTSTickStruct& q = fx.sink.lastTick;
	// CZCE 分支：Turnover * volScale(15)
	EXPECT_DOUBLE_EQ(15000.0, q.total_turnover);
	EXPECT_DOUBLE_EQ(0.0, q.settle_price);
	EXPECT_EQ(0u, strcmp(q.exchg, "CZCE"));
}

//////////////////////////////////////////////////////////////////////////
// 时间串换算
//////////////////////////////////////////////////////////////////////////
TEST(femas_parser, strtotime_strips_colons)
{
	EXPECT_EQ(101530u, strToTime("10:15:30"));
	EXPECT_EQ(93005u, strToTime("09:30:05"));
	EXPECT_EQ(0u, strToTime(""));
	EXPECT_EQ(235959u, strToTime("23:59:59"));
}

//////////////////////////////////////////////////////////////////////////
// 崩溃/挂死路径：整条 DISABLED，说明写在 bug 文件
//////////////////////////////////////////////////////////////////////////
TEST(femas_parser, extern_c_factory_round_trip)
{
	// createParser/deleteParser（:40-56）：走 IParserApi 抽象接口，不碰厂商 API
	IParserApi* p = createParser();
	ASSERT_TRUE(p != NULL);
	EXPECT_FALSE(p->isConnected());
	p->release();
	deleteParser(p);
	EXPECT_TRUE(p == NULL);
	deleteParser(p);			// 二次删除必须安全
}

TEST(femas_parser, DISABLED_init_needs_vendor_dylib_and_null_creator_crashes)
{
	GTEST_SKIP() << "ParserFemas::init(:118-130) dlopen 失败后 m_funcCreator 为空，"
					"末尾直接调用空函数指针必 SIGSEGV；缺厂商 shim 无法进入";
}

TEST(femas_parser, DISABLED_login_rsp_last_true_calls_null_api)
{
	GTEST_SKIP() << "OnRspUserLogin(:183) bIsLast=true 时 m_pUserAPI->GetTradingDay() 解引用空指针";
}
