/*
 * lane T2 — src/ParserCTPOpt/ParserCTPOpt.cpp 覆盖率用例（ETF 期权行情解析）
 *
 * ParserCTPOpt 的数据成员全是 private（ParserCTPOpt.h:90 起），没有 protected 段，
 * 所以不能像 t_TraderCTP 那样用派生 seam 注入 m_pUserAPI；
 * 但 OnRsp / OnRtn / OnFront 这一批厂商回调是 **public**（ParserCTPOpt.h:56-73），
 * 于是「行情回调 → WTSTickData 转换」这条最大的分支可以手工构造结构体直接驱动。
 *
 * 不会碰到的路径（详见 bugs/T2_traderctp_xele_ctpopt.md）：
 *   - init()：dll 缺失时 m_funcCreator 为 NULL，第 130 行直接调用 → 段错误，改名 DISABLED_；
 *   - OnRspUserLogin(bIsLast=true)：第 184 行对 NULL m_pUserAPI 取 TradingDay → 同样会崩。
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstring>
#include <set>
#include <string>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSVariant.hpp"

#include "ParserCTPOpt/ParserCTPOpt.h"

// 插件入口（ParserCTPOpt.cpp:40-56 的 extern "C" 定义）
extern "C"
{
    wtp::IParserApi* createParser();
    void deleteParser(wtp::IParserApi*& parser);
}

namespace t2opt {

USING_NS_WTP;

/// 只登记两张合约：DCE.m2401（换手值原样）与 CZCE.CF405（换手值 *volScale）
class OptBaseDataMgr : public IBaseDataMgr
{
public:
    virtual ~OptBaseDataMgr() {}

    void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
    {
        ct->setCommInfo(comm);
        _cts.push_back(ct);
        _comms.push_back(comm);
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
    {
        for (auto* c : _comms)
            if (strcmp(c->getExchg(), exchg) == 0 && strcmp(c->getProduct(), pid) == 0)
                return c;
        return NULL;
    }
    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)uDate;
        auto it = _ct_key.find(std::string(code) + "|" + exchg);
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

private:
    std::vector<WTSCommodityInfo*>  _comms;
    std::vector<WTSContractInfo*>   _cts;
    wt_hashmap<std::string, WTSContractInfo*> _ct_key;
};

/// 记录型解析 sink：tick 会在回调返回后被 release，所以只能抄快照
class OptParserSink : public IParserSpi
{
public:
    OptBaseDataMgr  bd_;

    uint32_t        n_bdmgr_ = 0;
    uint32_t        n_events_ = 0;
    int             last_event_ = -1;
    uint32_t        n_logs_ = 0;
    std::string     last_log_;
    uint32_t        n_quotes_ = 0;
    WTSTickStruct   snap_;
    std::string     snap_code_;

    virtual ~OptParserSink() {}

    virtual IBaseDataMgr* getBaseDataMgr() override { ++n_bdmgr_; return &bd_; }
    virtual void handleEvent(WTSParserEvent e, int32_t ec) override
    { ++n_events_; last_event_ = (int)e; (void)ec; }
    virtual void handleParserLog(WTSLogLevel ll, const char* message) override
    { ++n_logs_; last_log_ = message; (void)ll; }
    virtual void handleQuote(WTSTickData* quote, uint32_t procFlag) override
    {
        ++n_quotes_;
        snap_ = quote->getTickStruct();
        WTSContractInfo* ct = quote->getContractInfo();
        snap_code_ = (ct != NULL) ? ct->getCode() : "";
        (void)procFlag;
    }
    virtual void handleSymbolList(const WTSArray* aySymbols) override { (void)aySymbols; }
};

inline void fill_basic(CThostFtdcDepthMarketDataField& f)
{
    memset(&f, 0, sizeof(f));
    strcpy(f.InstrumentID, "m2401");
    strcpy(f.ExchangeID, "DCE");
    strcpy(f.ActionDay, "20240115");
    strcpy(f.UpdateTime, "10:30:00");
    f.UpdateMillisec = 100;
    f.LastPrice = 3000.0;
    f.OpenPrice = 2980.0;
    f.HighestPrice = 3050.0;
    f.LowestPrice = 2960.0;
    f.Volume = 1234;
    f.Turnover = 5555.0;
    f.OpenInterest = 77000;
    f.UpperLimitPrice = 3300.0;
    f.LowerLimitPrice = 2700.0;
    f.PreClosePrice = 2990.0;
    f.PreSettlementPrice = 2995.0;
    f.PreOpenInterest = 76000;
    f.SettlementPrice = DBL_MAX;         // 未结算 => 不覆盖 settle_price
    f.BidPrice1 = 3000.0; f.BidPrice2 = 2999.0; f.BidPrice3 = 2998.0;
    f.BidPrice4 = 2997.0; f.BidPrice5 = 2996.0;
    f.BidVolume1 = 11; f.BidVolume2 = 22; f.BidVolume3 = 33; f.BidVolume4 = 44; f.BidVolume5 = 55;
    f.AskPrice1 = 3001.0; f.AskPrice2 = 3002.0; f.AskPrice3 = 3003.0;
    f.AskPrice4 = 3004.0; f.AskPrice5 = 3005.0;
    f.AskVolume1 = 1; f.AskVolume2 = 2; f.AskVolume3 = 3; f.AskVolume4 = 4; f.AskVolume5 = 5;
}

}// namespace t2opt

using namespace t2opt;

class T2ParserCTPOptTest : public ::testing::Test
{
protected:
    ParserCTPOpt    parser_;
    OptParserSink   sink_;

    void SetUp() override
    {
        comm_ = WTSCommodityInfo::create("m", "豆粕", "DCE", "TRADING", "CHINA");
        comm_->setCategory(CC_Future);
        comm_->setVolScale(10);
        ct_ = WTSContractInfo::create("m2401", "豆粕2401", "DCE", "m");
        sink_.bd_.regis(comm_, ct_);

        comm2_ = WTSCommodityInfo::create("CF", "棉花", "CZCE", "TRADING", "CHINA");
        comm2_->setCategory(CC_Future);
        comm2_->setVolScale(5);
        ct2_ = WTSContractInfo::create("CF405", "棉花405", "CZCE", "CF");
        sink_.bd_.regis(comm2_, ct2_);

        parser_.registerSpi(&sink_);
    }

    WTSCommodityInfo* comm_ = NULL;
    WTSContractInfo* ct_ = NULL;
    WTSCommodityInfo* comm2_ = NULL;
    WTSContractInfo* ct2_ = NULL;
};

TEST(T2ParserCTPOptStatic, plugin_entry_points_build_and_destroy_parser)
{
    IParserApi* p = createParser();
    ASSERT_NE(nullptr, p);
    EXPECT_NE(nullptr, dynamic_cast<ParserCTPOpt*>(p));
    deleteParser(p);
    EXPECT_EQ(nullptr, p);
    // 二次删除必须安全
    deleteParser(p);
    EXPECT_EQ(nullptr, p);
}

TEST_F(T2ParserCTPOptTest, fresh_parser_is_not_connected_but_lifecycle_is_safe)
{
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_TRUE(parser_.connect());           // m_pUserAPI==NULL => 只走判空分支
    EXPECT_TRUE(parser_.disconnect());
    EXPECT_NO_THROW(parser_.release());
    EXPECT_FALSE(parser_.isConnected());
}

TEST_F(T2ParserCTPOptTest, register_spi_pulls_base_datamgr_once)
{
    EXPECT_EQ(1u, sink_.n_bdmgr_);
    ParserCTPOpt other;
    EXPECT_EQ(1u, sink_.n_bdmgr_);            // 未注册不会再取
    other.registerSpi(NULL);
    EXPECT_EQ(1u, sink_.n_bdmgr_);
}

TEST_F(T2ParserCTPOptTest, front_connected_and_close_events)
{
    parser_.OnFrontConnected();
    EXPECT_EQ(1u, sink_.n_events_);
    EXPECT_EQ((int)WPE_Connect, sink_.last_event_);
    EXPECT_NE(std::string::npos, sink_.last_log_.find("Market data server connected"));
    // ReqUserLogin 在没有 API 指针时必须直接返回（不崩、也不再回调事件）
    EXPECT_EQ(1u, sink_.n_events_);

    parser_.OnFrontDisconnected(0x1002);
    EXPECT_EQ(2u, sink_.n_events_);
    EXPECT_EQ((int)WPE_Close, sink_.last_event_);
    EXPECT_NE(std::string::npos, sink_.last_log_.find("disconnected"));

    CThostFtdcUserLogoutField lu;
    memset(&lu, 0, sizeof(lu));
    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));
    parser_.OnRspUserLogout(&lu, &ok, 4, true);
    EXPECT_EQ(3u, sink_.n_events_);
    EXPECT_EQ((int)WPE_Logout, sink_.last_event_);
}

TEST_F(T2ParserCTPOptTest, heartbeat_and_error_callbacks_do_not_reach_core)
{
    uint32_t n_before = sink_.n_events_;
    parser_.OnHeartBeatWarning(12);
    EXPECT_EQ(1u, sink_.n_logs_);
    EXPECT_NE(std::string::npos, sink_.last_log_.find("Heartbeating"));
    EXPECT_EQ(n_before, sink_.n_events_);

    // IsErrorRspInfo 恒返回 false（ParserCTPOpt.cpp:402），所以 OnRspError 是个空操作：
    // 柜台的错误码/错误文本既不进日志也不进事件
    CThostFtdcRspInfoField err;
    memset(&err, 0, sizeof(err));
    err.ErrorID = 404;
    strcpy(err.ErrorMsg, "Instrument not found");
    uint32_t logs_before = sink_.n_logs_;
    parser_.OnRspError(&err, 7, true);
    EXPECT_EQ(logs_before, sink_.n_logs_);
    EXPECT_EQ(n_before, sink_.n_events_);

    CThostFtdcSpecificInstrumentField si;
    memset(&si, 0, sizeof(si));
    strcpy(si.InstrumentID, "m2401");
    parser_.OnRspSubMarketData(&si, &err, 8, true);
    parser_.OnRspSubMarketData(&si, NULL, 8, true);
    parser_.OnRspUnSubMarketData(&si, &err, 9, true);
    EXPECT_EQ(logs_before, sink_.n_logs_);
    EXPECT_EQ(0u, sink_.n_quotes_);
}

TEST_F(T2ParserCTPOptTest, login_rsp_without_is_last_is_ignored)
{
    CThostFtdcRspUserLoginField lg;
    memset(&lg, 0, sizeof(lg));
    CThostFtdcRspInfoField ok;
    memset(&ok, 0, sizeof(ok));
    // bIsLast=false => 不进入取 TradingDay 的分支（true 会 NULL 解引用，见 DISABLED_ 用例）
    EXPECT_NO_THROW(parser_.OnRspUserLogin(&lg, &ok, 1, false));
    EXPECT_EQ(0u, sink_.n_events_);
}

TEST_F(T2ParserCTPOptTest, depth_tick_without_datamgr_is_dropped)
{
    CThostFtdcDepthMarketDataField f;
    fill_basic(f);
    ParserCTPOpt bare;                        // 从未 registerSpi => m_pBaseDataMgr==NULL
    EXPECT_NO_THROW(bare.OnRtnDepthMarketData(&f));
    EXPECT_EQ(0u, sink_.n_quotes_);

    // 有 datamgr 但合约未登记 => getContract 返回 NULL 后直接 return
    strcpy(f.InstrumentID, "zz9999");
    parser_.OnRtnDepthMarketData(&f);
    EXPECT_EQ(0u, sink_.n_quotes_);
}

TEST_F(T2ParserCTPOptTest, depth_tick_maps_prices_and_levels)
{
    CThostFtdcDepthMarketDataField f;
    fill_basic(f);
    parser_.OnRtnDepthMarketData(&f);
    ASSERT_EQ(1u, sink_.n_quotes_);
    const WTSTickStruct& q = sink_.snap_;
    EXPECT_EQ("m2401", sink_.snap_code_);
    EXPECT_EQ("DCE", std::string(q.exchg));
    EXPECT_EQ(20240115u, q.action_date);
    EXPECT_EQ(103000100u, q.action_time);     // strToTime("10:30:00")*1000 + 100
    EXPECT_DOUBLE_EQ(3000.0, q.price);
    EXPECT_DOUBLE_EQ(2980.0, q.open);
    EXPECT_DOUBLE_EQ(3050.0, q.high);
    EXPECT_DOUBLE_EQ(2960.0, q.low);
    EXPECT_EQ(1234u, q.total_volume);
    EXPECT_EQ(77000u, q.open_interest);
    EXPECT_EQ(76000u, q.pre_interest);
    EXPECT_DOUBLE_EQ(5555.0, q.total_turnover);
    EXPECT_DOUBLE_EQ(3300.0, q.upper_limit);
    EXPECT_DOUBLE_EQ(2700.0, q.lower_limit);
    EXPECT_DOUBLE_EQ(2990.0, q.pre_close);
    EXPECT_DOUBLE_EQ(2995.0, q.pre_settle);
    EXPECT_DOUBLE_EQ(0.0, q.settle_price);    // SettlementPrice==DBL_MAX => 不写入
    EXPECT_DOUBLE_EQ(3001.0, q.ask_prices[0]);
    EXPECT_DOUBLE_EQ(3005.0, q.ask_prices[4]);
    EXPECT_DOUBLE_EQ(2996.0, q.bid_prices[4]);
    EXPECT_EQ(1u, q.ask_qty[0]);
    EXPECT_EQ(5u, q.ask_qty[4]);
    EXPECT_EQ(11u, q.bid_qty[0]);
    EXPECT_EQ(55u, q.bid_qty[4]);
    EXPECT_EQ(0u, q.trading_date);            // init 未跑，交易日仍是默认值
}

TEST_F(T2ParserCTPOptTest, depth_tick_invalid_values_are_zeroed)
{
    CThostFtdcDepthMarketDataField f;
    fill_basic(f);
    f.LastPrice = DBL_MAX;
    f.OpenPrice = FLT_MAX;
    f.HighestPrice = DBL_MAX;
    f.LowestPrice = FLT_MAX;
    f.UpperLimitPrice = DBL_MAX;
    f.PreClosePrice = DBL_MAX;
    f.BidPrice1 = DBL_MAX;
    f.AskPrice1 = FLT_MAX;
    f.Turnover = DBL_MAX;
    parser_.OnRtnDepthMarketData(&f);
    ASSERT_EQ(1u, sink_.n_quotes_);
    const WTSTickStruct& q = sink_.snap_;
    EXPECT_DOUBLE_EQ(0.0, q.price);
    EXPECT_DOUBLE_EQ(0.0, q.open);
    EXPECT_DOUBLE_EQ(0.0, q.high);
    EXPECT_DOUBLE_EQ(0.0, q.low);
    EXPECT_DOUBLE_EQ(0.0, q.upper_limit);
    EXPECT_DOUBLE_EQ(0.0, q.pre_close);
    EXPECT_DOUBLE_EQ(0.0, q.bid_prices[0]);
    EXPECT_DOUBLE_EQ(0.0, q.ask_prices[0]);
    EXPECT_DOUBLE_EQ(0.0, q.total_turnover);  // 非 CZCE 且 Turnover==DBL_MAX => 保持 0
}

TEST_F(T2ParserCTPOptTest, czce_turnover_is_scaled_by_volscale)
{
    CThostFtdcDepthMarketDataField f;
    fill_basic(f);
    strcpy(f.InstrumentID, "CF405");
    strcpy(f.ExchangeID, "CZCE");
    f.Turnover = 1000.0;
    f.SettlementPrice = 3100.0;
    parser_.OnRtnDepthMarketData(&f);
    ASSERT_EQ(1u, sink_.n_quotes_);
    EXPECT_EQ("CZCE", std::string(sink_.snap_.exchg));
    EXPECT_DOUBLE_EQ(5000.0, sink_.snap_.total_turnover);   // 1000 * volScale(5)
    EXPECT_DOUBLE_EQ(3100.0, sink_.snap_.settle_price);
    EXPECT_EQ("CF405", sink_.snap_code_);
}

TEST_F(T2ParserCTPOptTest, stale_tick_time_is_lifted_to_latest)
{
    CThostFtdcDepthMarketDataField f;
    fill_basic(f);
    parser_.OnRtnDepthMarketData(&f);
    ASSERT_EQ(1u, sink_.n_quotes_);
    const uint32_t first = sink_.snap_.action_time;
    EXPECT_EQ(103000100u, first);

    // 更旧的行情（期权不活跃合约会重复推最后成交价，时间戳倒退）
    CThostFtdcDepthMarketDataField older = f;
    strcpy(older.UpdateTime, "09:00:00");
    older.UpdateMillisec = 0;
    parser_.OnRtnDepthMarketData(&older);
    ASSERT_EQ(2u, sink_.n_quotes_);
    EXPECT_GE(sink_.snap_.action_time, first);
    EXPECT_NE(90000000u, sink_.snap_.action_time);

    // 时间戳为 0（从未成交的合约）也走同一修正分支
    CThostFtdcDepthMarketDataField zero = f;
    strcpy(zero.UpdateTime, "");
    zero.UpdateMillisec = 0;
    parser_.OnRtnDepthMarketData(&zero);
    ASSERT_EQ(3u, sink_.n_quotes_);
    EXPECT_GE(sink_.snap_.action_time, first);
}

TEST_F(T2ParserCTPOptTest, first_zero_time_tick_falls_back_to_local_clock)
{
    CThostFtdcDepthMarketDataField f;
    fill_basic(f);
    strcpy(f.UpdateTime, "");
    f.UpdateMillisec = 0;
    strcpy(f.ActionDay, "20200102");
    parser_.OnRtnDepthMarketData(&f);
    ASSERT_EQ(1u, sink_.n_quotes_);
    // m_uLatestTime==0 => TimeUtils::getDateTime 用本地时间覆盖 date/time
    EXPECT_GT(sink_.snap_.action_date, 20200102u);
    EXPECT_NE(20200102u, sink_.snap_.action_date);
}

TEST_F(T2ParserCTPOptTest, subscribe_and_unsubscribe_without_api_are_harmless)
{
    CodeSet subs;
    subs.insert("DCE.m2401");
    subs.insert("CZCE.CF405");
    EXPECT_NO_THROW(parser_.subscribe(subs));
    EXPECT_NO_THROW(parser_.unsubscribe(subs));
    CodeSet one;
    one.insert("nomarket");
    EXPECT_NO_THROW(parser_.subscribe(one));
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_EQ(0u, sink_.n_quotes_);
}

TEST_F(T2ParserCTPOptTest, DISABLED_init_and_login_rsp_dereference_missing_ctpopt_library)
{
    // init() 第 129-132 行：DLLHelper::load_library 失败后 get_symbol 返回 NULL，
    // 紧接着 m_pUserAPI = m_funcCreator(...) 是空指针调用 => 段错误；
    // OnRspUserLogin(bIsLast=true) 第 184 行同理解引用 NULL 的 m_pUserAPI。
    // 两者都会让覆盖率进程崩掉，整条改名 DISABLED_，留给主控裁定。
    GTEST_SKIP() << "本机无 soptthostmduserapi_se，init/登录回报路径必崩";
}
