/*
 * T5 lane — src/ParserCTP/ParserCTP.cpp 覆盖率用例（行情回调 → WTSTickData 字段映射）
 *
 * 可测边界（实测源码）：
 *   - init() 在 __APPLE__ 分支里直接调用真实 `CThostFtdcMdApi::CreateFtdcMdApi`
 *     （ParserCTP.cpp:119-141），会创建厂商实例并在 cwd 建 flow 目录；
 *     connect() 又会让它起 IO 线程 → 违反「不起线程、不连真柜台」，
 *     所以 init/connect 后的真实 API 生命周期整条留给主控裁定（见 bugs 文件），
 *     这里只覆盖 m_pUserAPI == NULL 的分支。
 *   - CThostFtdcMdSpi 的回调都是 public，可手工构造厂商结构体直喂。
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

#include "ParserCTP/ParserCTP.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"

USING_NS_WTP;

namespace {

class CTPFakeBdMgr : public IBaseDataMgr
{
public:
	virtual ~CTPFakeBdMgr() {}

	void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
	{
		ct->setCommInfo(comm);
		_comms.push_back(comm);
		_cts.push_back(ct);
		_ct_key[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
	}

	virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		for (auto* c : _comms)
			if (strcmp(c->getFullPid(), exchgpid) == 0)
				return c;
		return NULL;
	}
	virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{ (void)exchg; (void)pid; return NULL; }

	virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
	{
		(void)uDate;
		++n_get_contract_;
		last_code_ = code ? code : "";
		last_exchg_ = exchg ? exchg : "";
		auto it = _ct_key.find(last_code_ + "|" + last_exchg_);
		return it == _ct_key.end() ? NULL : it->second;
	}

	virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
	{ (void)exchg; (void)uDate; return NULL; }
	virtual WTSSessionInfo* getSession(const char* sid) override { (void)sid; return NULL; }
	virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
	{ (void)code; (void)exchg; return NULL; }
	virtual WTSArray* getAllSessions() override { return NULL; }
	virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
	{ (void)pid; (void)uDate; (void)isTpl; return false; }
	virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
	{ (void)stdPID; (void)uTime; (void)isSession; return uDate; }
	virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
	{ (void)stdPID; (void)isSession; (void)isStart; return tDate; }

	uint32_t n_get_contract_ = 0;
	std::string last_code_;
	std::string last_exchg_;

private:
	std::vector<WTSCommodityInfo*> _comms;
	std::vector<WTSContractInfo*> _cts;
	wt_hashmap<std::string, WTSContractInfo*> _ct_key;
};

/// IParserSpi 替身：handleQuote 里把 WTSTickStruct 整份快照下来
/// （产品代码在回调返回后立即 tick->release()，指针不能留）
class CTPSink : public IParserSpi
{
public:
	virtual ~CTPSink() {}

	virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }

	virtual void handleSymbolList(const WTSArray* aySymbols) override
	{ ++n_symbol_list_; last_symbol_size_ = aySymbols ? aySymbols->size() : 0; }

	virtual void handleQuote(WTSTickData* quote, uint32_t procFlag) override
	{
		++n_quote_;
		last_proc_flag_ = procFlag;
		if (quote == NULL)
			return;
		last_has_quote_ = true;
		memcpy(&last_tick_, &quote->getTickStruct(), sizeof(WTSTickStruct));
	}

	virtual void handleParserLog(WTSLogLevel ll, const char* message) override
	{ logs_.push_back({ (int)ll, message ? message : "" }); }

	virtual void handleEvent(WTSParserEvent e, int32_t ec) override { events_.push_back({ (int)e, ec }); }

	struct LogRec { int ll; std::string msg; };
	struct EventRec { int e; int32_t ec; };

	uint32_t n_quote_ = 0;
	uint32_t n_symbol_list_ = 0;
	uint32_t last_symbol_size_ = 0;
	uint32_t last_proc_flag_ = 0;
	bool last_has_quote_ = false;
	WTSTickStruct last_tick_;
	std::vector<LogRec> logs_;
	std::vector<EventRec> events_;

	CTPFakeBdMgr bd_;
};

class ParserCTPTest : public ::testing::Test
{
protected:
	ParserCTP parser_;
	CTPSink sink_;
	WTSCommodityInfo* shfe_ag_ = NULL;
	WTSContractInfo* ct_ag_ = NULL;
	WTSCommodityInfo* czce_sf_ = NULL;
	WTSContractInfo* ct_sf_ = NULL;

	void SetUp() override
	{
		memset(&sink_.last_tick_, 0, sizeof(sink_.last_tick_));

		ct_ag_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
		shfe_ag_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
		shfe_ag_->setCategory(CC_Future);
		shfe_ag_->setVolScale(5);
		sink_.bd_.regis(shfe_ag_, ct_ag_);

		ct_sf_ = WTSContractInfo::create("SF509", "玻璃509", "CZCE", "SF");
		czce_sf_ = WTSCommodityInfo::create("SF", "玻璃", "CZCE", "TRADING", "CHINA");
		czce_sf_->setCategory(CC_Future);
		czce_sf_->setVolScale(20);
		sink_.bd_.regis(czce_sf_, ct_sf_);

		parser_.registerSpi(&sink_);
	}

	void TearDown() override
	{
		// 未 init → m_pUserAPI 恒为 NULL，这里只清 WTS 对象
		if (ct_ag_) ct_ag_->release();
		if (shfe_ag_) shfe_ag_->release();
		if (ct_sf_) ct_sf_->release();
		if (czce_sf_) czce_sf_->release();
	}

	/// 手工构造一条「形状合法」的 CTP 深度行情
	void fillDepth(CThostFtdcDepthMarketDataField& d, const char* code, const char* exchg) const
	{
		memset(&d, 0, sizeof(d));
		strncpy(d.InstrumentID, code, sizeof(d.InstrumentID) - 1);
		strncpy(d.ExchangeID, exchg, sizeof(d.ExchangeID) - 1);
		strncpy(d.ActionDay, "20250102", sizeof(d.ActionDay) - 1);
		strncpy(d.UpdateTime, "09:30:15", sizeof(d.UpdateTime) - 1);
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
};

/* ---------------------------------------------------------------- 用例 */

/// 未 init：isConnected 必须为 false，connect/disconnect/release 走 api 判空分支
TEST_F(ParserCTPTest, lifecycle_without_init_keeps_api_null_and_returns_success)
{
	EXPECT_FALSE(parser_.isConnected());
	EXPECT_TRUE(parser_.connect());        // m_pUserAPI==NULL → 只 return true
	EXPECT_TRUE(parser_.disconnect());
	parser_.release();                    // release → disconnect
	EXPECT_FALSE(parser_.isConnected());
	EXPECT_TRUE(sink_.events_.empty());
}

/// registerSpi 把 IBaseDataMgr 拉过来；重复注册不报错（ParserCTP 没有 replaced 告警）
TEST_F(ParserCTPTest, register_spi_pulls_base_data_manager)
{
	parser_.OnFrontDisconnected(7);
	ASSERT_EQ(1u, sink_.events_.size());
	EXPECT_EQ((int)WPE_Close, sink_.events_[0].e);

	CTPSink other;
	parser_.registerSpi(&other);
	parser_.OnRspUserLogout(NULL, NULL, 0, true);
	EXPECT_EQ(0u, sink_.events_.size());   // 旧 sink 不再收事件
	ASSERT_EQ(1u, other.events_.size());
	parser_.registerSpi(&sink_);
}

/// OnFrontConnected：先外发 WPE_Connect，再 ReqUserLogin —— api 为空则静默返回
TEST_F(ParserCTPTest, front_connected_publishes_event_and_skips_login_request)
{
	parser_.OnFrontConnected();
	ASSERT_EQ(1u, sink_.events_.size());
	EXPECT_EQ((int)WPE_Connect, sink_.events_[0].e);
	EXPECT_EQ(0, sink_.events_[0].ec);
	ASSERT_EQ(1u, sink_.logs_.size());
	EXPECT_NE(std::string::npos, sink_.logs_[0].msg.find("Market data server connected"));
}

/// OnRspUserLogin 的两个否决条件：bIsLast=false 与错误响应
/// （IsErrorRspInfo 恒返回 false，所以 !IsErrorRspInfo 永真 —— 见 bugs）
TEST_F(ParserCTPTest, rsp_user_login_ignored_until_last_packet)
{
	CThostFtdcRspUserLoginField rsp;
	memset(&rsp, 0, sizeof(rsp));
	CThostFtdcRspInfoField info;
	memset(&info, 0, sizeof(info));
	info.ErrorID = 0;

	parser_.OnRspUserLogin(&rsp, &info, 1, false);
	EXPECT_TRUE(sink_.events_.empty());
	EXPECT_TRUE(sink_.logs_.empty());
}

/// bIsLast=true 会解引用 m_pUserAPI->GetTradingDay()（本机恒 NULL）→ 必崩，留给主控
TEST_F(ParserCTPTest, DISABLED_rsp_user_login_last_packet_dereferences_null_api)
{
	CThostFtdcRspUserLoginField rsp;
	memset(&rsp, 0, sizeof(rsp));
	parser_.OnRspUserLogin(&rsp, NULL, 1, true);
}

/// 非零错误响应不能被当作行情发布。错误是否需要经日志上报仍需接口契约确认：
/// IParserSpi 只定义通用日志回调；WTSParserEvent 目前只有连接/登录/注销事件。
TEST_F(ParserCTPTest, rsp_error_does_not_publish_market_data)
{
	CThostFtdcRspInfoField info;
	memset(&info, 0, sizeof(info));
	info.ErrorID = 40;
	strncpy(info.ErrorMsg, "MarketNotInit", sizeof(info.ErrorMsg) - 1);

	parser_.OnRspError(&info, 3, true);
	EXPECT_EQ(0u, sink_.n_quote_);
}

/// 订阅/退订在未登录（m_uTradingDate==0）时只缓存，不发请求、不外发日志
TEST_F(ParserCTPTest, subscribe_before_login_only_stores_filter)
{
	CodeSet codes;
	codes.insert("SHFE.ag2512");
	codes.insert("CZCE.SF509");
	codes.insert("ag2512");              // 无 '.' 的原始码也允许进缓存
	parser_.subscribe(codes);
	EXPECT_TRUE(sink_.logs_.empty());
	EXPECT_EQ(0u, sink_.n_quote_);
	EXPECT_FALSE(parser_.isConnected());

	CodeSet none;
	parser_.subscribe(none);            // 空集合分支
	parser_.unsubscribe(codes);         // 空实现
	EXPECT_TRUE(sink_.logs_.empty());
}

/// 心跳与登出回调的事件/日志映射
TEST_F(ParserCTPTest, heartbeat_and_logout_callbacks)
{
	parser_.OnHeartBeatWarning(120);
	ASSERT_EQ(1u, sink_.logs_.size());
	EXPECT_NE(std::string::npos, sink_.logs_[0].msg.find("Heartbeating, elapse: 120"));

	parser_.OnRspUserLogout(NULL, NULL, 0, true);
	ASSERT_EQ(1u, sink_.events_.size());
	EXPECT_EQ((int)WPE_Logout, sink_.events_[0].e);

	CThostFtdcSpecificInstrumentField spec;
	memset(&spec, 0, sizeof(spec));
	CThostFtdcRspInfoField info;
	memset(&info, 0, sizeof(info));
	info.ErrorID = 5;
	parser_.OnRspSubMarketData(&spec, &info, 9, true);
	parser_.OnRspUnSubMarketData(&spec, &info, 9, true);
	// 两个订阅回调在产品侧是空实现：不得新增任何事件/日志
	EXPECT_EQ(1u, sink_.events_.size());
	EXPECT_EQ(1u, sink_.logs_.size());
}

/* ------------------------------------------------------------ 行情映射 */

/// 没注册 spi（m_pBaseDataMgr==NULL）时，行情必须被丢弃
TEST(ParserCTPNoSink, depth_dropped_without_base_data_mgr)
{
	ParserCTP p;
	CThostFtdcDepthMarketDataField d;
	memset(&d, 0, sizeof(d));
	strncpy(d.InstrumentID, "ag2512", sizeof(d.InstrumentID) - 1);
	strncpy(d.ExchangeID, "SHFE", sizeof(d.ExchangeID) - 1);
	p.OnRtnDepthMarketData(&d);
	SUCCEED();   // 只证明守卫分支不崩；无 sink 可观测
}

/// 未登记的合约 → getContract 返回 NULL → 不外发行情
TEST_F(ParserCTPTest, depth_of_unregistered_instrument_dropped)
{
	CThostFtdcDepthMarketDataField d;
	fillDepth(d, "cu1234", "SHFE");
	parser_.OnRtnDepthMarketData(&d);
	EXPECT_EQ(0u, sink_.n_quote_);
	EXPECT_EQ("cu1234", sink_.bd_.last_code_);
	EXPECT_EQ("SHFE", sink_.bd_.last_exchg_);
}

/// 行情结构 → WTSTickData 的主字段映射（日期/时间/毫秒/价格/量/持仓）
TEST_F(ParserCTPTest, depth_maps_core_fields_into_tick_struct)
{
	CThostFtdcDepthMarketDataField d;
	fillDepth(d, "ag2512", "SHFE");
	parser_.OnRtnDepthMarketData(&d);

	ASSERT_EQ(1u, sink_.n_quote_);
	EXPECT_EQ(1u, sink_.last_proc_flag_);          // handleQuote(tick, 1)
	const WTSTickStruct& q = sink_.last_tick_;
	EXPECT_STREQ("SHFE", q.exchg);                 // 来自 commInfo->getExchg()
	EXPECT_EQ(20250102u, q.action_date);           // ActionDay
	// "09:30:15" 取数字 → 093015 → *1000 + 450
	EXPECT_EQ(93015450u, q.action_time);
	EXPECT_DOUBLE_EQ(7011.0, q.price);
	EXPECT_DOUBLE_EQ(7000.0, q.open);
	EXPECT_DOUBLE_EQ(7099.0, q.high);
	EXPECT_DOUBLE_EQ(6901.0, q.low);
	EXPECT_EQ(12345u, q.total_volume);
	EXPECT_DOUBLE_EQ(250.0, q.total_turnover);     // 非 CZCE：直接透传
	EXPECT_DOUBLE_EQ(7050.0, q.settle_price);
	EXPECT_DOUBLE_EQ(200000.0, q.open_interest);
	EXPECT_DOUBLE_EQ(190000.0, q.pre_interest);
	EXPECT_DOUBLE_EQ(7500.0, q.upper_limit);
	EXPECT_DOUBLE_EQ(6500.0, q.lower_limit);
	EXPECT_DOUBLE_EQ(6999.0, q.pre_close);
	EXPECT_DOUBLE_EQ(6998.0, q.pre_settle);
	EXPECT_EQ(0u, q.trading_date);                 // 未经登录，m_uTradingDate 仍是构造值 0
}

/// 五档买卖价量的展开映射
TEST_F(ParserCTPTest, depth_maps_five_level_quotes)
{
	CThostFtdcDepthMarketDataField d;
	fillDepth(d, "ag2512", "SHFE");
	// 逐档写入（CTP 结构是 5 个独立字段，不共用一个数组）
	d.BidPrice1 = 100.1; d.BidPrice2 = 100.2; d.BidPrice3 = 100.3; d.BidPrice4 = 100.4; d.BidPrice5 = 100.5;
	d.AskPrice1 = 101.1; d.AskPrice2 = 101.2; d.AskPrice3 = 101.3; d.AskPrice4 = 101.4; d.AskPrice5 = 101.5;
	d.BidVolume1 = 1; d.BidVolume2 = 2; d.BidVolume3 = 3; d.BidVolume4 = 4; d.BidVolume5 = 5;
	d.AskVolume1 = 11; d.AskVolume2 = 12; d.AskVolume3 = 13; d.AskVolume4 = 14; d.AskVolume5 = 15;

	parser_.OnRtnDepthMarketData(&d);
	ASSERT_EQ(1u, sink_.n_quote_);
	const WTSTickStruct& q = sink_.last_tick_;
	for (int i = 0; i < 5; ++i)
	{
		EXPECT_DOUBLE_EQ(100.1 + i * 0.1, q.bid_prices[i]) << "bid idx " << i;
		EXPECT_DOUBLE_EQ(101.1 + i * 0.1, q.ask_prices[i]) << "ask idx " << i;
		EXPECT_EQ((uint32_t)(1 + i), q.bid_qty[i]) << "bidqty idx " << i;
		EXPECT_EQ((uint32_t)(11 + i), q.ask_qty[i]) << "askqty idx " << i;
	}
}

/// DBL_MAX / FLT_MAX 哨兵值必须落成 0（checkValid 分支）
TEST_F(ParserCTPTest, depth_sanitize_dbl_max_and_flt_max_sentinels)
{
	CThostFtdcDepthMarketDataField d;
	fillDepth(d, "ag2512", "SHFE");
	d.LastPrice = DBL_MAX;
	d.OpenPrice = FLT_MAX;
	d.HighestPrice = DBL_MAX;
	d.LowestPrice = 0.0;
	d.UpperLimitPrice = DBL_MAX;
	d.LowerLimitPrice = FLT_MAX;
	d.PreClosePrice = DBL_MAX;
	d.PreSettlementPrice = FLT_MAX;
	d.BidPrice1 = DBL_MAX;
	d.AskPrice3 = FLT_MAX;

	parser_.OnRtnDepthMarketData(&d);
	ASSERT_EQ(1u, sink_.n_quote_);
	const WTSTickStruct& q = sink_.last_tick_;
	EXPECT_DOUBLE_EQ(0.0, q.price);
	EXPECT_DOUBLE_EQ(0.0, q.open);
	EXPECT_DOUBLE_EQ(0.0, q.high);
	EXPECT_DOUBLE_EQ(0.0, q.low);
	EXPECT_DOUBLE_EQ(0.0, q.upper_limit);
	EXPECT_DOUBLE_EQ(0.0, q.lower_limit);
	EXPECT_DOUBLE_EQ(0.0, q.pre_close);
	EXPECT_DOUBLE_EQ(0.0, q.pre_settle);
	EXPECT_DOUBLE_EQ(0.0, q.bid_prices[0]);
	EXPECT_DOUBLE_EQ(0.0, q.ask_prices[2]);
}

/// CZCE 的成交额要乘 volScale（20），与非 CZCE 分支可区分
TEST_F(ParserCTPTest, depth_czce_turnover_scaled_by_volume_scale)
{
	CThostFtdcDepthMarketDataField d;
	fillDepth(d, "SF509", "CZCE");
	d.Turnover = 100.0;
	parser_.OnRtnDepthMarketData(&d);

	ASSERT_EQ(1u, sink_.n_quote_);
	const WTSTickStruct& q = sink_.last_tick_;
	EXPECT_STREQ("CZCE", q.exchg);
	EXPECT_DOUBLE_EQ(100.0 * 20.0, q.total_turnover);
}

/* ---------------------------------------------------------------- 工厂 */

extern "C"
{
	IParserApi* createParser();
	void deleteParser(IParserApi*& parser);
}

TEST(ctp_plugin_factory, create_and_delete_roundtrip_null_safe)
{
	IParserApi* p = createParser();
	ASSERT_NE(nullptr, p);
	EXPECT_FALSE(p->isConnected());
	p->release();
	deleteParser(p);
	EXPECT_EQ(nullptr, p);

	IParserApi* nil = NULL;
	deleteParser(nil);
	EXPECT_EQ(nullptr, nil);
}

} // namespace
