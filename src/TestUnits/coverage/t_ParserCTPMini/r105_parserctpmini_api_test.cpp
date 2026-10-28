/*
 * r105_parserctpmin_api_test.cpp — lane R105：ParserCTPMini 的「有厂商 API」半边
 *
 * ParserCTPMini::init() 走 DLLHelper::load_library + dlsym（ParserCTPMini.cpp:109-132），
 * 本机没有可 dlopen 的 mini 行情库，init 末尾必然空指针调用（BUG-T4-009）。
 * 因此这里用测试侧白盒（#define private public，仅本 TU）把 m_pUserAPI 指向
 * r105_ctpmini_api_stub.h 的可控空实现，再通过公开的 connect/subscribe 与
 * OnFront / OnRsp / OnRtn 回调驱动成功/失败/空数据分支。
 *
 * 覆盖的生产行段：
 *   init 的目录/模块解析（异常路径）        ParserCTPMini.cpp:96-118
 *   connect/disconnect/release              ParserCTPMini.cpp:137-162
 *   OnFrontConnected/OnRspUserLogin         ParserCTPMini.cpp:169-194
 *   ReqUserLogin/SubscribeMarketData        ParserCTPMini.cpp:352-408
 *   subscribe 登录后内联下发                ParserCTPMini.cpp:415-450
 *   OnRtnDepthMarketData 缺省日/夜盘纠偏     ParserCTPMini.cpp:218-257、279-280
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

#include "r105_ctpmini_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/TimeUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#define private public
#include "ParserCTPMini/ParserCTPMini.h"
#undef private

USING_NS_WTP;

namespace {

namespace fs = std::filesystem;

struct R105MiniTempDir
{
	std::string path;

	R105MiniTempDir()
	{
		char tmpl[128];
		std::snprintf(tmpl, sizeof(tmpl), "/tmp/wt_r105_ctpmini_XXXXXX");
		const char* made = mkdtemp(tmpl);
		if (made != NULL)
			path = made;
	}
	~R105MiniTempDir()
	{
		if (!path.empty())
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	}
};

class R105MiniDataMgr : public IBaseDataMgr
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
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{
		if (code == NULL)
			return NULL;
		for (WTSContractInfo* ct : _cts)
			if (std::strcmp(ct->getCode(), code) == 0)
				return ct;
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

class R105MiniSink : public IParserSpi
{
public:
	R105MiniDataMgr bd;
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

class R105MiniTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		comm_ = WTSCommodityInfo::create("rb", "Rebar", "SHFE", "TRADING", "CHINA");
		comm_->setCategory(CC_Future);
		comm_->setVolScale(10);
		ct_ = WTSContractInfo::create("rb2401", "Rebar2401", "SHFE", "rb");
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
		std::strncpy(d.InstrumentID, "rb2401", sizeof(d.InstrumentID) - 1);
		std::strncpy(d.ExchangeID, "SHFE", sizeof(d.ExchangeID) - 1);
		std::strncpy(d.ActionDay, "20251028", sizeof(d.ActionDay) - 1);
		std::strncpy(d.UpdateTime, "10:00:00", sizeof(d.UpdateTime) - 1);
		d.UpdateMillisec = 250;
		d.LastPrice = 3521.0;
		d.OpenPrice = 3500.0;
		d.HighestPrice = 3550.0;
		d.LowestPrice = 3490.0;
		d.Volume = 120000;
		d.Turnover = 4.2e8;
		d.SettlementPrice = 3510.0;
		d.OpenInterest = 900000.0;
		d.PreOpenInterest = 880000.0;
		d.UpperLimitPrice = 3700.0;
		d.LowerLimitPrice = 3300.0;
		d.PreClosePrice = 3495.0;
		d.PreSettlementPrice = 3499.0;
		for (int i = 0; i < 5; ++i)
		{
			d.BidPrice1 = 3521.0 - i;
			d.AskPrice1 = 3522.0 + i;
			d.BidVolume1 = 11 + i;
			d.AskVolume1 = 10 + i;
		}
	}

	R105CtpMiniMdApiStub api_;
	ParserCTPMini parser_;
	R105MiniSink sink_;
	WTSCommodityInfo* comm_ = NULL;
	WTSContractInfo* ct_ = NULL;
};

}	// namespace

/* ParserCTPMini.cpp:142-150、169-194、352-408 — connect→登录→首次订阅 */
TEST_F(R105MiniTest, injected_api_connect_login_and_do_subscribe)
{
	EXPECT_TRUE(parser_.isConnected());
	EXPECT_TRUE(parser_.connect());
	EXPECT_EQ(1, api_.init_calls);

	CodeSet codes;
	codes.insert("SHFE.rb2401");
	codes.insert("DCE.i2405");
	parser_.subscribe(codes);
	EXPECT_EQ(0, api_.sub_calls);		// 未登录只缓存

	parser_.OnFrontConnected();
	ASSERT_EQ(1u, sink_.events.size());
	EXPECT_EQ((int)WPE_Connect, sink_.events[0].e);
	EXPECT_TRUE(sink_.hasLog("Market data server connected"));
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
	EXPECT_TRUE(api_.sub_codes.count("rb2401") == 1);
	EXPECT_TRUE(api_.sub_codes.count("i2405") == 1);
	EXPECT_TRUE(sink_.hasLog("2 contracts subscribed in total"));
}

/* ParserCTPMini.cpp:352-371、368-408、415-449 — 登录/订阅失败与登录后内联下发 */
TEST_F(R105MiniTest, login_failure_and_inline_subscribe_branches)
{
	api_.login_ret = -2;
	parser_.OnFrontConnected();
	EXPECT_TRUE(sink_.hasLog("Sending login request failed: -2"));

	api_.login_ret = 0;
	CodeSet empty;
	parser_.OnRspUserLogin(NULL, NULL, 1, true);	// 空过滤：SubscribeMarketData 直接 return
	EXPECT_EQ(0, api_.sub_calls);
	ASSERT_EQ(2u, sink_.events.size());

	CodeSet codes;
	codes.insert("SHFE.rb2401");
	codes.insert("CFFEX.IF2412");
	api_.sub_ret = -3;
	parser_.subscribe(codes);		// 已登录：走 415-449 的内联下发
	EXPECT_EQ(1, api_.sub_calls);
	EXPECT_EQ(2, api_.sub_count);
	EXPECT_TRUE(sink_.hasLog("Sending md subscribe request failed: -3"));

	api_.sub_ret = 0;
	parser_.subscribe(codes);
	EXPECT_EQ(2, api_.sub_calls);
	EXPECT_TRUE(sink_.hasLog("2 contracts subscribed in total"));

	parser_.subscribe(empty);
	EXPECT_EQ(2, api_.sub_calls);		// nCount==0，不发请求

	parser_.m_pUserAPI = NULL;
	parser_.subscribe(codes);			// API 为空：只更新缓存
	EXPECT_EQ(2, api_.sub_calls);
	parser_.m_pUserAPI = &api_;
}

/* ParserCTPMini.cpp:96-118 — flowdir 父路径是普通文件时 init 抛异常，且未触碰厂商符号 */
TEST_F(R105MiniTest, init_flowdir_creation_failure_throws_before_vendor_symbol)
{
	R105MiniTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	const std::string blocker = tmp.path + "/blocker";
	FILE* fp = std::fopen(blocker.c_str(), "wb");
	ASSERT_NE(nullptr, fp);
	std::fclose(fp);

	WTSVariant* cfg = makeCfg(blocker + "/sub");
	ASSERT_NE(nullptr, cfg);

	ParserCTPMini bare;
	EXPECT_THROW(bare.init(cfg), std::filesystem::filesystem_error);
	EXPECT_FALSE(bare.isConnected());
	cfg->release();
}

/* ParserCTPMini.cpp:220-230、279-280 — 空管理器丢弃、ActionDay 缺省回退交易日、正常结算价 */
TEST_F(R105MiniTest, depth_null_manager_actionday_fallback_and_settlement)
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

	fillDepth(d);
	d.ActionDay[0] = '\0';
	std::strncpy(d.UpdateTime, "10:00:00", sizeof(d.UpdateTime) - 1);
	d.UpdateMillisec = 100;
	d.SettlementPrice = 3510.0;
	parser_.OnRtnDepthMarketData(&d);

	ASSERT_EQ(1u, sink_.n_quote);
	EXPECT_EQ(1u, sink_.last_flag);
	EXPECT_EQ(20260301u, sink_.last_tick.action_date);
	EXPECT_EQ(100000100u, sink_.last_tick.action_time);
	EXPECT_DOUBLE_EQ(3510.0, sink_.last_tick.settle_price);
}

/* ParserCTPMini.cpp:232-257 — 夜盘发生日=交易日时按本地时钟纠偏 */
TEST_F(R105MiniTest, night_session_recomputes_action_date)
{
	api_.trading_day = "20251028";
	parser_.OnRspUserLogin(NULL, NULL, 1, true);

	uint32_t curDate = 0, curTime = 0;
	TimeUtils::getDateTime(curDate, curTime);
	const uint32_t curHour = curTime / 10000000;

	CThostFtdcDepthMarketDataField d;
	fillDepth(d);
	std::strncpy(d.ActionDay, "20251028", sizeof(d.ActionDay) - 1);
	std::strncpy(d.UpdateTime, "23:30:00", sizeof(d.UpdateTime) - 1);
	d.UpdateMillisec = 0;

	parser_.OnRtnDepthMarketData(&d);
	if (curHour >= 3 && curHour < 9)
	{
		EXPECT_EQ(0u, sink_.n_quote);
	}
	else
	{
		ASSERT_EQ(1u, sink_.n_quote);
		if (curHour == 0)
			EXPECT_EQ(TimeUtils::getNextDate(curDate, -1), sink_.last_tick.action_date);
		else
			EXPECT_EQ(curDate, sink_.last_tick.action_date);
	}
}

/* ParserCTPMini.cpp:29-37、142-162、352-356 — 无 sink / 无 API 守卫 */
TEST_F(R105MiniTest, null_sink_and_no_api_guards)
{
	ParserCTPMini bare;
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
