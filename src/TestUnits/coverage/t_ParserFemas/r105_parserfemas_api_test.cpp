/*
 * r105_parserfemas_api_test.cpp — lane R105：ParserFemas 的「有厂商 API」半边
 *
 * ParserFemas::init() 走 DLLHelper::load_library + dlsym（ParserFemas.cpp:109-131），
 * 本机没有可 dlopen 的 USTPmduserapiAF，init 末尾必然空指针调用（BUG-T3-001）。
 * 因此这里用测试侧白盒（#define private public，仅本 TU）把 m_pUserAPI 指向
 * r105_femas_api_stub.h 的可控空实现，再通过公开的 connect/subscribe 与
 * OnFront / OnRsp / OnRtn 回调驱动成功/失败/空数据分支。
 *
 * 覆盖的生产行段：
 *   init 的目录/模块解析（异常路径）        ParserFemas.cpp:96-117
 *   connect/disconnect/release              ParserFemas.cpp:136-161
 *   OnFrontConnected/OnRspUserLogin         ParserFemas.cpp:168-193
 *   ReqUserLogin/SubscribeMarketData        ParserFemas.cpp:347-403
 *   subscribe 登录后内联下发                ParserFemas.cpp:410-445
 *   OnRtnDepthMarketData 夜盘纠偏            ParserFemas.cpp:217-253
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

#include "r105_femas_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/TimeUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#define private public
#include "ParserFemas/ParserFemas.h"
#undef private

USING_NS_WTP;

namespace {

namespace fs = std::filesystem;

struct R105FemasTempDir
{
	std::string path;

	R105FemasTempDir()
	{
		char tmpl[128];
		std::snprintf(tmpl, sizeof(tmpl), "/tmp/wt_r105_femas_XXXXXX");
		const char* made = mkdtemp(tmpl);
		if (made != NULL)
			path = made;
	}
	~R105FemasTempDir()
	{
		if (!path.empty())
		{
			std::error_code ec;
			fs::remove_all(path, ec);
		}
	}
};

class R105FemasDataMgr : public IBaseDataMgr
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

class R105FemasSink : public IParserSpi
{
public:
	R105FemasDataMgr bd;
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

class R105FemasTest : public ::testing::Test
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

	static void fillDepth(CUstpFtdcDepthMarketDataField& d)
	{
		std::memset(&d, 0, sizeof(d));
		std::strncpy(d.InstrumentID, "m2401", sizeof(d.InstrumentID) - 1);
		std::strncpy(d.ActionDay, "20251028", sizeof(d.ActionDay) - 1);
		std::strncpy(d.UpdateTime, "10:00:00", sizeof(d.UpdateTime) - 1);
		d.UpdateMillisec = 250;
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

	R105FemasMdApiStub api_;
	ParserFemas parser_;
	R105FemasSink sink_;
	WTSCommodityInfo* comm_ = NULL;
	WTSContractInfo* ct_ = NULL;
};

}	// namespace

/* ParserFemas.cpp:141-149、168-193、347-403 — connect→登录→首次订阅 */
TEST_F(R105FemasTest, injected_api_connect_login_and_do_subscribe)
{
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
}

/* IParserApi.h:115-118 — API 对象存在不等于连接已建立，前置/断开回调更新连接状态 */
TEST_F(R105FemasTest, is_connected_tracks_front_connection_callbacks)
{
	EXPECT_FALSE(parser_.isConnected());

	EXPECT_TRUE(parser_.connect());
	EXPECT_EQ(1, api_.init_calls);
	EXPECT_FALSE(parser_.isConnected());

	parser_.OnFrontConnected();
	EXPECT_TRUE(parser_.isConnected());

	parser_.OnFrontDisconnected(1);
	EXPECT_FALSE(parser_.isConnected());

	EXPECT_TRUE(parser_.disconnect());
	EXPECT_EQ(1, api_.release_calls);
	EXPECT_FALSE(parser_.isConnected());
}

/* ParserFemas.cpp:347-366、368-403、410-445 — 登录/订阅失败与登录后内联下发 */
TEST_F(R105FemasTest, login_failure_and_inline_subscribe_branches)
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

/* ParserFemas.cpp:96-117 — flowdir 父路径是普通文件时 init 抛异常，不触发厂商空指针 */
TEST_F(R105FemasTest, init_flowdir_creation_failure_throws_before_vendor_symbol)
{
	R105FemasTempDir tmp;
	ASSERT_FALSE(tmp.path.empty());
	const std::string blocker = tmp.path + "/blocker";
	FILE* fp = std::fopen(blocker.c_str(), "wb");
	ASSERT_NE(nullptr, fp);
	std::fclose(fp);

	WTSVariant* cfg = makeCfg(blocker + "/sub");
	ASSERT_NE(nullptr, cfg);

	ParserFemas bare;
	EXPECT_THROW(bare.init(cfg), std::filesystem::filesystem_error);
	EXPECT_FALSE(bare.isConnected());
	cfg->release();
}

/* ParserFemas.cpp:219-222、255-294 — 空管理器丢弃 + 正常字段映射 */
TEST_F(R105FemasTest, depth_null_manager_and_normal_settlement)
{
	sink_.null_mgr = true;
	parser_.registerSpi(&sink_);
	CUstpFtdcDepthMarketDataField d;
	fillDepth(d);
	parser_.OnRtnDepthMarketData(&d);
	EXPECT_EQ(0u, sink_.n_quote);

	sink_.null_mgr = false;
	parser_.registerSpi(&sink_);
	parser_.OnRtnDepthMarketData(&d);

	ASSERT_EQ(1u, sink_.n_quote);
	EXPECT_EQ(1u, sink_.last_flag);
	EXPECT_STREQ("m2401", sink_.last_tick.code);
	EXPECT_STREQ("DCE", sink_.last_tick.exchg);
	EXPECT_EQ(20251028u, sink_.last_tick.action_date);
	EXPECT_EQ(100000250u, sink_.last_tick.action_time);
	EXPECT_DOUBLE_EQ(3000.0, sink_.last_tick.price);
	EXPECT_DOUBLE_EQ(2995.0, sink_.last_tick.settle_price);
	EXPECT_DOUBLE_EQ(5555.0, sink_.last_tick.total_turnover);
}

/* ParserFemas.cpp:228-253 — 夜盘发生日=交易日时按本地时钟纠偏 */
TEST_F(R105FemasTest, night_session_recomputes_action_date)
{
	api_.trading_day = "20251028";
	parser_.OnRspUserLogin(NULL, NULL, 1, true);

	uint32_t curDate = 0, curTime = 0;
	TimeUtils::getDateTime(curDate, curTime);
	const uint32_t curHour = curTime / 10000000;

	CUstpFtdcDepthMarketDataField d;
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

/* ParserFemas.cpp:29-37、141-161、347-351 — 无 sink / 无 API 守卫 */
TEST_F(R105FemasTest, null_sink_and_no_api_guards)
{
	ParserFemas bare;
	bare.m_pUserAPI = NULL;
	bare.m_sink = NULL;
	bare.m_pBaseDataMgr = NULL;

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
