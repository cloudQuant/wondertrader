/*
 * r105_parserctpopt_api_test.cpp — lane R105：ParserCTPOpt 的「有厂商 API」半边
 *
 * ParserCTPOpt::init() 走 DLLHelper::load_library + dlsym（ParserCTPOpt.cpp:110-132），
 * 本机没有可 dlopen 的 sopt 行情库，init 末尾必然空指针调用。这里用测试侧
 * 白盒（#define private public，仅本 TU）把 m_pUserAPI 指向
 * r105_ctpopt_api_stub.h 的可控空实现，再通过公开的 connect/subscribe 与
 * OnFront / OnRsp / OnRtn 回调驱动成功/失败/空数据分支。
 *
 * 覆盖的生产行段：
 *   init 的目录/模块解析（异常路径）        ParserCTPOpt.cpp:97-119
 *   connect/disconnect/release              ParserCTPOpt.cpp:137-162
 *   OnFrontConnected/OnRspUserLogin         ParserCTPOpt.cpp:169-193
 *   ReqUserLogin/SubscribeMarketData        ParserCTPOpt.cpp:344-400
 *   subscribe 登录后内联下发                ParserCTPOpt.cpp:407-442
 *   OnRtnDepthMarketData 空管理器/未知合约/日期回退 ParserCTPOpt.cpp:220-262
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "r105_ctpopt_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#define private public
#include "ParserCTPOpt/ParserCTPOpt.h"
#undef private

USING_NS_WTP;

namespace {

namespace fs = std::filesystem;

struct R105OptTempDir
{
	std::string path;

	R105OptTempDir()
	{
		char tmpl[128];
		std::snprintf(tmpl, sizeof(tmpl), "/tmp/wt_r105_ctpopt_XXXXXX");
		const char* made = mkdtemp(tmpl);
		if (made != NULL)
			path = made;
	}
	~R105OptTempDir()
	{
		if (!path.empty())
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	}
};

class R105OptDataMgr : public IBaseDataMgr
{
public:
	void add(WTSCommodityInfo* comm, WTSContractInfo* ct)
	{
		ct->setCommInfo(comm);
		_comms.push_back(comm);
		_cts.push_back(ct);
	}

	WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		if (code == NULL || exchg == NULL)
			return NULL;
		for (WTSContractInfo* ct : _cts)
		{
			if (std::strcmp(ct->getCode(), code) == 0 && std::strcmp(ct->getExchg(), exchg) == 0)
				return ct;
		}
		return NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }

private:
	std::vector<WTSCommodityInfo*> _comms;
	std::vector<WTSContractInfo*> _cts;
};

class R105OptSink : public IParserSpi
{
public:
	R105OptDataMgr bd;
	bool null_mgr = false;

	struct Event
	{
		int e;
		int32_t ec;
	};
	struct Log
	{
		int ll;
		std::string msg;
	};

	std::vector<Event> events;
	std::vector<Log> logs;
	uint32_t n_quote = 0;
	uint32_t last_flag = 0;
	WTSTickStruct last_tick;

	IBaseDataMgr* getBaseDataMgr() override { return null_mgr ? NULL : &bd; }
	void handleSymbolList(const WTSArray*) override {}
	void handleQuote(WTSTickData* quote, uint32_t procFlag) override
	{
		++n_quote;
		last_flag = procFlag;
		if (quote != NULL)
			last_tick = quote->getTickStruct();
	}
	void handleParserLog(WTSLogLevel ll, const char* msg) override
	{
		Log lg;
		lg.ll = (int)ll;
		lg.msg = (msg == NULL) ? "" : msg;
		logs.push_back(lg);
	}
	void handleEvent(WTSParserEvent e, int32_t ec) override
	{
		Event ev;
		ev.e = (int)e;
		ev.ec = ec;
		events.push_back(ev);
	}

	bool hasLog(const char* needle) const
	{
		for (const Log& lg : logs)
			if (lg.msg.find(needle) != std::string::npos)
				return true;
		return false;
	}
};

class R105OptTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		comm_ = WTSCommodityInfo::create("m", "豆粕", "DCE", "TRADING", "CHINA");
		comm_->setCategory(CC_Future);
		comm_->setVolScale(10);
		ct_ = WTSContractInfo::create("m2401", "豆粕2401", "DCE", "m");
		sink_.bd.add(comm_, ct_);

		parser_.m_pUserAPI = &api_;
		parser_.m_strFrontAddr = "tcp://127.0.0.1:1";
		parser_.m_strBroker = "9999";
		parser_.m_strUserID = "r105";
		parser_.m_strPassword = "secret";
		parser_.m_iRequestID = 0;
		parser_.m_uTradingDate = 0;
		parser_.registerSpi(&sink_);
	}
	void TearDown() override
	{
		parser_.m_pUserAPI = NULL;
		if (ct_ != NULL)
			ct_->release();
		if (comm_ != NULL)
			comm_->release();
	}

	WTSVariant* makeCfg(const std::string& flowdir)
	{
		const std::string content = "front: tcp://127.0.0.1:1\n"
									"broker: 9999\n"
									"user: r105\n"
									"pass: secret\n"
									"flowdir: " + flowdir + "\n";
		return WTSCfgLoader::load_from_content(content, true);
	}

	static void fillDepth(CThostFtdcDepthMarketDataField& d)
	{
		std::memset(&d, 0, sizeof(d));
		std::strncpy(d.InstrumentID, "m2401", sizeof(d.InstrumentID) - 1);
		std::strncpy(d.ExchangeID, "DCE", sizeof(d.ExchangeID) - 1);
		std::strncpy(d.ActionDay, "20251028", sizeof(d.ActionDay) - 1);
		std::strncpy(d.UpdateTime, "10:30:00", sizeof(d.UpdateTime) - 1);
		d.UpdateMillisec = 100;
		d.LastPrice = 3000.0;
		d.OpenPrice = 2980.0;
		d.HighestPrice = 3050.0;
		d.LowestPrice = 2960.0;
		d.Volume = 1234;
		d.Turnover = 5555.0;
		d.SettlementPrice = 2995.0;
		d.OpenInterest = 77000.0;
		d.PreOpenInterest = 76000.0;
		d.UpperLimitPrice = 3300.0;
		d.LowerLimitPrice = 2700.0;
		d.PreClosePrice = 2990.0;
		d.PreSettlementPrice = 2995.0;
		for (int i = 0; i < 5; ++i)
		{
			d.BidPrice1 = 3000.0 - i;
			d.AskPrice1 = 3001.0 + i;
			d.BidVolume1 = 11 + i;
			d.AskVolume1 = 1 + i;
		}
	}

	R105CtpOptMdApiStub api_;
	ParserCTPOpt parser_;
	R105OptSink sink_;
	WTSCommodityInfo* comm_ = NULL;
	WTSContractInfo* ct_ = NULL;
};

}	// namespace

/* ParserCTPOpt.cpp:142-150、169-193、344-400 — connect→登录→首次订阅 */
TEST_F(R105OptTest, injected_api_connect_login_and_do_subscribe)
{
	EXPECT_TRUE(parser_.isConnected());
	EXPECT_TRUE(parser_.connect());
	EXPECT_EQ(1, api_.init_calls);

	CodeSet codes;
	codes.insert("DCE.m2401");
	codes.insert("CZCE.CF405");
	parser_.subscribe(codes);
	EXPECT_EQ(0, api_.sub_calls);

	parser_.OnFrontConnected();
	ASSERT_EQ(1u, sink_.events.size());
	EXPECT_EQ((int)WPE_Connect, sink_.events[0].e);
	ASSERT_EQ(1, api_.login_calls);
	EXPECT_EQ("9999", api_.broker);
	EXPECT_EQ("r105", api_.user);
	EXPECT_EQ("secret", api_.pass);
	EXPECT_EQ(1, api_.last_reqid);

	api_.trading_day = "20260301";
	parser_.OnRspUserLogin(NULL, NULL, 1, true);
	ASSERT_EQ(2u, sink_.events.size());
	EXPECT_EQ((int)WPE_Login, sink_.events[1].e);
	ASSERT_EQ(1, api_.sub_calls);
	EXPECT_EQ(2, api_.sub_count);
	EXPECT_TRUE(api_.sub_codes.count("m2401") == 1);
	EXPECT_TRUE(api_.sub_codes.count("CF405") == 1);
	EXPECT_TRUE(sink_.hasLog("2 contracts subscribed in total"));

	parser_.release();
	EXPECT_EQ(1, api_.release_calls);
	EXPECT_FALSE(parser_.isConnected());
	CodeSet one;
	one.insert("DCE.m2401");
	parser_.subscribe(one);		// 已登录但 API 已释放：只更新本地缓存
	EXPECT_EQ(1, api_.sub_calls);
}

TEST_F(R105OptTest, nonzero_login_error_does_not_report_success_or_subscribe)
{
	CodeSet codes;
	codes.insert("DCE.m2401");
	parser_.subscribe(codes); // 缓存登录前的订阅，错误登录不应下发
	EXPECT_EQ(0, api_.sub_calls);

	CThostFtdcRspInfoField err;
	std::memset(&err, 0, sizeof(err));
	err.ErrorID = 404;
	std::strncpy(err.ErrorMsg, "Instrument not found", sizeof(err.ErrorMsg) - 1);
	parser_.OnRspUserLogin(NULL, &err, 7, true);

	bool reportedSuccessfulLogin = false;
	for (const R105OptSink::Event& event : sink_.events)
	{
		if (event.e == (int)WPE_Login && event.ec == 0)
			reportedSuccessfulLogin = true;
	}
	EXPECT_FALSE(reportedSuccessfulLogin);
	EXPECT_EQ(0, api_.sub_calls);
	EXPECT_TRUE(api_.sub_codes.empty());
}

TEST_F(R105OptTest, zero_error_login_response_reports_success_and_subscribes)
{
	CodeSet codes;
	codes.insert("DCE.m2401");
	parser_.subscribe(codes);

	CThostFtdcRspInfoField ok;
	std::memset(&ok, 0, sizeof(ok));
	parser_.OnRspUserLogin(NULL, &ok, 8, true);

	ASSERT_EQ(1u, sink_.events.size());
	EXPECT_EQ((int)WPE_Login, sink_.events[0].e);
	EXPECT_EQ(0, sink_.events[0].ec);
	ASSERT_EQ(1, api_.sub_calls);
	EXPECT_EQ(1, api_.sub_count);
	EXPECT_EQ(1u, api_.sub_codes.count("m2401"));
}

/* ParserCTPOpt.cpp:344-363、365-400、407-442 — 登录/订阅失败与登录后内联下发 */
TEST_F(R105OptTest, login_failure_and_inline_subscribe_branches)
{
	api_.login_ret = -2;
	parser_.OnFrontConnected();
	EXPECT_TRUE(sink_.hasLog("Sending login request failed: -2"));

	api_.login_ret = 0;
	parser_.OnRspUserLogin(NULL, NULL, 1, true);	// 空过滤：SubscribeMarketData 直接 return
	EXPECT_EQ(0, api_.sub_calls);
	ASSERT_EQ(2u, sink_.events.size());

	CodeSet codes;
	codes.insert("DCE.m2401");
	codes.insert("SHFE.rb2401");
	api_.sub_ret = -3;
	parser_.subscribe(codes);
	EXPECT_EQ(1, api_.sub_calls);
	EXPECT_EQ(2, api_.sub_count);
	EXPECT_TRUE(sink_.hasLog("Sending md subscribe request failed: -3"));

	api_.sub_ret = 0;
	parser_.subscribe(codes);
	EXPECT_EQ(2, api_.sub_calls);
	EXPECT_TRUE(sink_.hasLog("2 contracts subscribed in total"));

	CodeSet empty;
	parser_.subscribe(empty);
	EXPECT_EQ(2, api_.sub_calls);

	parser_.m_pUserAPI = NULL;
	parser_.subscribe(codes);
	EXPECT_EQ(2, api_.sub_calls);
	parser_.m_pUserAPI = &api_;
}

/* ParserCTPOpt.cpp:97-119 — flowdir 父路径是普通文件时 init 抛异常，不触发厂商空指针 */
TEST_F(R105OptTest, init_flowdir_creation_failure_throws_before_vendor_symbol)
{
	R105OptTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	const std::string blocker = tmp.path + "/blocker";
	FILE* fp = std::fopen(blocker.c_str(), "wb");
	ASSERT_NE(nullptr, fp);
	std::fclose(fp);

	WTSVariant* cfg = makeCfg(blocker + "/sub");
	ASSERT_NE(nullptr, cfg);

	ParserCTPOpt bare;
	EXPECT_THROW(bare.init(cfg), std::filesystem::filesystem_error);
	EXPECT_FALSE(bare.isConnected());
	cfg->release();
}

/* ParserCTPOpt.cpp:220-223、251-262 — 空管理器/未知合约丢弃 + ActionDay 缺省回退交易日 */
TEST_F(R105OptTest, depth_null_manager_unknown_contract_and_actionday_fallback)
{
	sink_.null_mgr = true;
	parser_.registerSpi(&sink_);
	CThostFtdcDepthMarketDataField d;
	fillDepth(d);
	parser_.OnRtnDepthMarketData(&d);
	EXPECT_EQ(0u, sink_.n_quote);

	sink_.null_mgr = false;
	parser_.registerSpi(&sink_);
	api_.trading_day = "20260301";
	parser_.OnRspUserLogin(NULL, NULL, 1, true);

	d.ActionDay[0] = '\0';
	std::strncpy(d.UpdateTime, "10:00:00", sizeof(d.UpdateTime) - 1);
	d.UpdateMillisec = 100;
	parser_.OnRtnDepthMarketData(&d);
	ASSERT_EQ(1u, sink_.n_quote);
	EXPECT_EQ(20260301u, sink_.last_tick.action_date);
	EXPECT_EQ(100000100u, sink_.last_tick.action_time);

	std::strncpy(d.InstrumentID, "zz9999", sizeof(d.InstrumentID) - 1);
	parser_.OnRtnDepthMarketData(&d);
	EXPECT_EQ(1u, sink_.n_quote);
}

/* ParserCTPOpt.cpp:29-37、142-162、344-349 — 无 sink / 无 API 守卫 */
TEST_F(R105OptTest, null_sink_and_no_api_guards)
{
	ParserCTPOpt bare;
	bare.m_pUserAPI = NULL;
	bare.m_sink = NULL;
	bare.m_pBaseDataMgr = NULL;

	EXPECT_TRUE(bare.connect());
	EXPECT_TRUE(bare.disconnect());
	bare.release();
	bare.OnFrontConnected();
	bare.OnHeartBeatWarning(3);
	bare.OnFrontDisconnected(4);
	bare.OnRspUserLogout(NULL, NULL, 1, true);
	bare.OnRspError(NULL, 2, true);
	bare.OnRspSubMarketData(NULL, NULL, 3, true);
	bare.OnRspUnSubMarketData(NULL, NULL, 4, true);
	SUCCEED();
}
