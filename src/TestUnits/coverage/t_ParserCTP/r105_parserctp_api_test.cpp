/*
 * r105_parserctp_api_test.cpp — lane R105：ParserCTP 的「有厂商 API」半边
 *
 * 既有 T5 用例只覆盖 m_pUserAPI == NULL 的拒绝分支。本文件用
 * r105_ctp_api_stub.h 在测试 TU 内提供 CThostFtdcMdApi::CreateFtdcMdApi 的
 * 可控实现，让公开的 init/connect/registerSpi/subscribe 与 OnRsp / OnFront
 * 回调真正走进产品代码的成功/失败分支；不连前置、不启线程、不读墙钟等待。
 *
 * 覆盖的生产行段：
 *   init/connect/disconnect/release       ParserCTP.cpp:98-168
 *   OnFrontConnected/OnRspUserLogin       ParserCTP.cpp:175-206
 *   ReqUserLogin/DoSubscribeMD            ParserCTP.cpp:368-424
 *   subscribe                             ParserCTP.cpp:431-442
 *   OnRtnDepthMarketData 本地时间/夜盘纠偏 ParserCTP.cpp:243-275
 *   sink 为空时的 write_log 守卫          ParserCTP.cpp:29-37
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "r105_ctp_api_stub.h"

#include "ParserCTP/ParserCTP.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/TimeUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

namespace {

namespace fs = std::filesystem;

struct R105CtpTempDir
{
	std::string path;

	R105CtpTempDir()
	{
		char tmpl[128];
		std::snprintf(tmpl, sizeof(tmpl), "/tmp/wt_r105_ctp_XXXXXX");
		const char* made = mkdtemp(tmpl);
		if (made != NULL)
			path = made;
	}
	~R105CtpTempDir()
	{
		if (!path.empty())
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	}
};

class R105CtpDataMgr : public IBaseDataMgr
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

class R105CtpSink : public IParserSpi
{
public:
	R105CtpDataMgr bd;
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

class R105CtpTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
		comm_->setCategory(CC_Future);
		comm_->setVolScale(5);
		ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
		sink_.bd.add(comm_, ct_);
		parser_.registerSpi(&sink_);
		r105_ctp::factory_target() = &api_;
	}
	void TearDown() override
	{
		r105_ctp::factory_target() = NULL;
		parser_.disconnect();
		if (ct_ != NULL)
			ct_->release();
		if (comm_ != NULL)
			comm_->release();
	}

	WTSVariant* makeCfg(const std::string& flowdir, bool localtime = false)
	{
		std::string content = "front: tcp://127.0.0.1:1\n"
							  "broker: 9999\n"
							  "user: r105\n"
							  "pass: secret\n"
							  "flowdir: " + flowdir + "\n";
		if (localtime)
			content += "localtime: true\n";
		return WTSCfgLoader::load_from_content(content, true);
	}

	static void fillDepth(CThostFtdcDepthMarketDataField& d)
	{
		std::memset(&d, 0, sizeof(d));
		std::strncpy(d.InstrumentID, "ag2512", sizeof(d.InstrumentID) - 1);
		std::strncpy(d.ExchangeID, "SHFE", sizeof(d.ExchangeID) - 1);
		std::strncpy(d.ActionDay, "20251028", sizeof(d.ActionDay) - 1);
		std::strncpy(d.UpdateTime, "09:30:15", sizeof(d.UpdateTime) - 1);
		d.UpdateMillisec = 450;
		d.LastPrice = 7011.0;
		d.OpenPrice = 7000.0;
		d.HighestPrice = 7099.0;
		d.LowestPrice = 6901.0;
		d.Volume = 12345;
		d.Turnover = 250.0;
		d.SettlementPrice = 7050.0;
		d.OpenInterest = 200000.0;
		d.PreOpenInterest = 190000.0;
		d.UpperLimitPrice = 7500.0;
		d.LowerLimitPrice = 6500.0;
		d.PreClosePrice = 6999.0;
		d.PreSettlementPrice = 6998.0;
		for (int i = 0; i < 5; ++i)
		{
			d.BidPrice1 = 7010.0 + i;
			d.AskPrice1 = 7012.0 + i;
			d.BidVolume1 = 10 + i;
			d.AskVolume1 = 20 + i;
		}
	}

	ParserCTP parser_;
	R105CtpSink sink_;
	R105CtpMdApiStub api_;
	WTSCommodityInfo* comm_ = NULL;
	WTSContractInfo* ct_ = NULL;
};

}	// namespace

/* ParserCTP.cpp:98-168、175-206、368-424、431-442 — init→connect→登录→订阅全链路 */
TEST_F(R105CtpTest, init_connect_login_and_subscribe_flow)
{
	R105CtpTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	const std::string flowdir = tmp.path + "/flows";
	WTSVariant* cfg = makeCfg(flowdir);
	ASSERT_NE(nullptr, cfg);

	EXPECT_TRUE(parser_.init(cfg));
	cfg->release();

	EXPECT_EQ(1, api_.spi_regs);
	EXPECT_EQ(1, api_.front_regs);
	EXPECT_EQ("tcp://127.0.0.1:1", api_.front);
	EXPECT_NE(std::string::npos, api_.flow_path.find("/9999/r105/"));
	EXPECT_TRUE(fs::exists(fs::path(flowdir) / "9999" / "r105"));
	EXPECT_TRUE(parser_.isConnected());

	EXPECT_TRUE(parser_.connect());
	EXPECT_EQ(1, api_.init_calls);

	CodeSet codes;
	codes.insert("SHFE.ag2512");
	codes.insert("CZCE.SF509");
	parser_.subscribe(codes);
	EXPECT_EQ(0, api_.sub_calls);		// 未登录：只缓存

	parser_.OnFrontConnected();
	ASSERT_EQ(1u, sink_.events.size());
	EXPECT_EQ((int)WPE_Connect, sink_.events[0].e);
	EXPECT_TRUE(sink_.hasLog("Market data server connected"));
	ASSERT_EQ(1, api_.login_calls);
	EXPECT_EQ("9999", api_.broker);
	EXPECT_EQ("r105", api_.user);
	EXPECT_EQ("secret", api_.pass);
	EXPECT_NE(std::string::npos, api_.product.find("WT_v"));
	EXPECT_EQ(1, api_.last_reqid);

	api_.trading_day = "20260301";
	parser_.OnRspUserLogin(NULL, NULL, 1, true);
	ASSERT_EQ(2u, sink_.events.size());
	EXPECT_EQ((int)WPE_Login, sink_.events[1].e);
	EXPECT_TRUE(sink_.hasLog("logined, 20260301"));
	ASSERT_EQ(1, api_.sub_calls);
	EXPECT_EQ(2, api_.sub_count);
	EXPECT_TRUE(api_.sub_codes.count("ag2512") == 1);
	EXPECT_TRUE(api_.sub_codes.count("SF509") == 1);
	EXPECT_TRUE(sink_.hasLog("2 contracts subscribed totally"));

	// 登录后的 subscribe 直接走 DoSubscribeMD 的失败/成功/空过滤三条分支
	api_.sub_ret = -5;
	CodeSet one;
	one.insert("DCE.i2405");
	parser_.subscribe(one);
	EXPECT_EQ(2, api_.sub_calls);
	EXPECT_TRUE(sink_.hasLog("Sending md subscribe request failed: -5"));

	api_.sub_ret = 0;
	parser_.subscribe(one);
	EXPECT_EQ(3, api_.sub_calls);

	CodeSet empty;
	parser_.subscribe(empty);
	EXPECT_EQ(3, api_.sub_calls);		// 空过滤直接 return

	parser_.release();
	EXPECT_EQ(1, api_.release_calls);
	EXPECT_FALSE(parser_.isConnected());

	// 已登录（交易日非 0）但 API 已释放：DoSubscribeMD 只清本地缓存
	parser_.subscribe(one);
	EXPECT_EQ(3, api_.sub_calls);
}

/* ParserCTP.cpp:186-205、368-387 — 登录发送失败 + 交易日为空回退本地日期 */
TEST_F(R105CtpTest, login_request_failure_and_empty_trading_day_fallback)
{
	R105CtpTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	WTSVariant* cfg = makeCfg(tmp.path + "/flows");
	ASSERT_NE(nullptr, cfg);
	ASSERT_TRUE(parser_.init(cfg));
	cfg->release();

	api_.login_ret = -3;
	parser_.OnFrontConnected();
	EXPECT_TRUE(sink_.hasLog("Sending login request failed: -3"));
	ASSERT_EQ(1, api_.login_calls);

	api_.trading_day = "";
	parser_.OnRspUserLogin(NULL, NULL, 2, true);
	EXPECT_TRUE(sink_.hasLog("Market data server logined,"));

	// 回退到 TimeUtils::getCurDate()：交易日非 0，后续 subscribe 才会真正下发
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	parser_.subscribe(codes);
	EXPECT_EQ(1, api_.sub_calls);
}

/* ParserCTP.cpp:243-247 — localtime=true 时忽略行情包日期，改用本地时钟 */
TEST_F(R105CtpTest, localtime_config_uses_local_timestamp)
{
	R105CtpTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	WTSVariant* cfg = makeCfg(tmp.path + "/flows", true);
	ASSERT_NE(nullptr, cfg);
	ASSERT_TRUE(parser_.init(cfg));
	cfg->release();

	const uint32_t before = TimeUtils::getCurDate();
	CThostFtdcDepthMarketDataField d;
	fillDepth(d);
	std::strncpy(d.ActionDay, "20000101", sizeof(d.ActionDay) - 1);
	std::strncpy(d.UpdateTime, "00:00:00", sizeof(d.UpdateTime) - 1);
	d.UpdateMillisec = 0;

	parser_.OnRtnDepthMarketData(&d);
	ASSERT_EQ(1u, sink_.n_quote);
	const uint32_t after = TimeUtils::getCurDate();
	EXPECT_TRUE(sink_.last_tick.action_date == before || sink_.last_tick.action_date == after);
	EXPECT_NE(0u, sink_.last_tick.action_time);
	EXPECT_NE(20000101u, sink_.last_tick.action_date);
}

/* ParserCTP.cpp:254-275 — 夜盘行情发生日=交易日时按本地时钟纠偏 */
TEST_F(R105CtpTest, night_session_recomputes_action_date)
{
	R105CtpTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	WTSVariant* cfg = makeCfg(tmp.path + "/flows");
	ASSERT_NE(nullptr, cfg);
	ASSERT_TRUE(parser_.init(cfg));
	cfg->release();

	api_.trading_day = "20251028";
	parser_.OnRspUserLogin(NULL, NULL, 1, true);

	uint32_t curDate = 0, curTime = 0;
	TimeUtils::getDateTime(curDate, curTime);
	const uint32_t curHour = curTime / 10000000;

	CThostFtdcDepthMarketDataField d;
	fillDepth(d);
	// actHour 必须是 23 才能命中 23 点与 0 点的跨日修正；15:00 落在普通路径。
	std::strncpy(d.UpdateTime, "23:30:00", sizeof(d.UpdateTime) - 1);
	d.UpdateMillisec = 0;

	parser_.OnRtnDepthMarketData(&d);
	if (curHour >= 3 && curHour < 9)
	{
		// 早盘前收到「发生日=交易日」的隔夜行情：产品直接丢弃
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

/* ParserCTP.cpp:29-37、148-168、368-373 — 无 sink / 无 API 时回调必须静默 */
TEST_F(R105CtpTest, null_sink_and_null_api_guards_are_silent)
{
	R105CtpTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	WTSVariant* cfg = makeCfg(tmp.path + "/flows");
	ASSERT_NE(nullptr, cfg);
	ASSERT_TRUE(parser_.init(cfg));
	cfg->release();

	// API 有效但 sink 为空：OnRspUserLogin 里未加 if(m_sink) 的 write_log 必须早退
	parser_.registerSpi(NULL);
	parser_.OnRspUserLogin(NULL, NULL, 1, true);
	EXPECT_TRUE(parser_.isConnected());

	parser_.registerSpi(&sink_);
	parser_.OnFrontConnected();
	ASSERT_EQ(1u, sink_.events.size());
	EXPECT_EQ((int)WPE_Connect, sink_.events[0].e);

	parser_.OnRspError(NULL, 1, true);
	parser_.OnRspSubMarketData(NULL, NULL, 2, true);
	parser_.OnRspUnSubMarketData(NULL, NULL, 3, true);
	EXPECT_EQ(1u, sink_.events.size());

	EXPECT_TRUE(parser_.connect());
	EXPECT_TRUE(parser_.disconnect());
	parser_.release();

	parser_.registerSpi(NULL);
	parser_.OnFrontConnected();		// m_pUserAPI==NULL：ReqUserLogin 直接返回
	parser_.OnHeartBeatWarning(10);		// sink==NULL 的日志守卫
	parser_.OnFrontDisconnected(7);
	parser_.OnRspUserLogout(NULL, NULL, 4, true);
	SUCCEED();
}
