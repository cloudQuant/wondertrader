/*
 * lane T2 — src/ParserXeleSkt/ParserXeleSkt.cpp 覆盖率用例（易盛 SKT 组播行情）
 *
 * 可达面：ParserXeleSkt 的 9 个成员全是 **private**（ParserXeleSkt.h:64 起），
 * handle_udp_read / prepare / reconnect / extract_buffer 也都是 private，
 * 既没有 protected 段也没有可注入的 API 指针，
 * 所以「组播包 → WTSTickData」这段最大的转换逻辑（extract_buffer，约 L360-530）
 * 在不用 #define private public 的前提下**进不去**——已按规则留白并写入 bugs 文件。
 * 本文件覆盖的是 IParserApi 公开面 + init 的地址解析失败路径。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSVariant.hpp"

#include "ParserXeleSkt/ParserXeleSkt.h"

// 插件入口（ParserXeleSkt.cpp:64-80 的 extern "C" 定义）
extern "C"
{
    wtp::IParserApi* createParser();
    void deleteParser(wtp::IParserApi*& parser);
}

namespace t2xele {

USING_NS_WTP;

/// 空合约表：本文件不断言行情转换，只需要一个合法的 IBaseDataMgr 出处
class XeleBaseDataMgr : public IBaseDataMgr
{
public:
    virtual ~XeleBaseDataMgr() {}

    virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override { (void)exchgpid; return NULL; }
    virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    { (void)exchg; (void)pid; return NULL; }
    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    { (void)code; (void)exchg; (void)uDate; return NULL; }
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
};

class XeleSink : public IParserSpi
{
public:
    XeleBaseDataMgr   bd_;

    uint32_t          n_bdmgr_ = 0;
    uint32_t          n_events_ = 0;
    uint32_t          n_logs_ = 0;
    uint32_t          n_quotes_ = 0;
    std::string       last_log_;
    int               last_event_ = -1;

    virtual ~XeleSink() {}

    virtual IBaseDataMgr* getBaseDataMgr() override { ++n_bdmgr_; return &bd_; }
    virtual void handleEvent(WTSParserEvent e, int32_t ec) override
    { ++n_events_; last_event_ = (int)e; (void)ec; }
    virtual void handleParserLog(WTSLogLevel ll, const char* message) override
    { ++n_logs_; last_log_ = message; (void)ll; }
    virtual void handleQuote(WTSTickData* quote, uint32_t procFlag) override
    { ++n_quotes_; (void)quote; (void)procFlag; }
    virtual void handleSymbolList(const WTSArray* aySymbols) override { (void)aySymbols; }
};

inline WTSVariant* make_cfg(const char* tcp_host)
{
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("tcp_host", tcp_host);
    cfg->append("tcp_port", (int32_t)9001);
    cfg->append("mcast_host", "224.1.2.3");
    cfg->append("mcast_port", (int32_t)9002);
    cfg->append("local_host", "127.0.0.1");
    cfg->append("gpsize", (uint32_t)0);
    return cfg;
}

}// namespace t2xele

using namespace t2xele;

class T2ParserXeleSktTest : public ::testing::Test
{
protected:
    ParserXeleSkt parser_;
    XeleSink      sink_;

    void SetUp() override
    {
        WTSVariant* cfg = make_cfg("127.0.0.1");
        ASSERT_TRUE(parser_.init(cfg));
        parser_.registerSpi(&sink_);
    }
};

TEST(T2ParserXeleSktStatic, plugin_entry_points_build_and_destroy_parser)
{
    IParserApi* p = createParser();
    ASSERT_NE(nullptr, p);
    EXPECT_NE(nullptr, dynamic_cast<ParserXeleSkt*>(p));
    deleteParser(p);
    EXPECT_EQ(nullptr, p);
    deleteParser(p);            // 已置空的指针必须安全
    EXPECT_EQ(nullptr, p);
}

TEST_F(T2ParserXeleSktTest, init_builds_endpoints_without_opening_sockets)
{
    // init 只做地址解析：此时不允许有组播 socket（isConnected 依赖 _udp_socket）
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_EQ(0u, sink_.n_quotes_);

    ParserXeleSkt other;
    WTSVariant* cfg = make_cfg("10.20.30.40");
    ASSERT_TRUE(other.init(cfg));
    EXPECT_FALSE(other.isConnected());
    EXPECT_NO_THROW(other.release());
}

TEST_F(T2ParserXeleSktTest, init_reports_failure_for_unparsable_tcp_host)
{
    ParserXeleSkt bad;
    WTSVariant* cfg = make_cfg("not-an-ip");
    bool initialized = true;
    EXPECT_NO_THROW(initialized = bad.init(cfg));
    EXPECT_FALSE(initialized);
    EXPECT_FALSE(bad.isConnected());
    cfg->release();
}

TEST_F(T2ParserXeleSktTest, init_reports_failure_when_tcp_host_is_missing)
{
    ParserXeleSkt bad;
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("tcp_port", (int32_t)9001);
    bool initialized = true;
    EXPECT_NO_THROW(initialized = bad.init(cfg));
    EXPECT_FALSE(initialized);
    cfg->release();
}

TEST_F(T2ParserXeleSktTest, register_spi_wires_sink_and_base_datamgr)
{
    EXPECT_EQ(1u, sink_.n_bdmgr_);
    EXPECT_EQ(0u, sink_.n_logs_);     // 首次注册不该有「替换」日志
}

TEST_F(T2ParserXeleSktTest, registering_second_sink_logs_replacement)
{
    XeleSink second;
    parser_.registerSpi(&second);
    EXPECT_EQ(1u, second.n_bdmgr_);
    ASSERT_EQ(1u, second.n_logs_);
    EXPECT_NE(std::string::npos, second.last_log_.find("Listener is replaced"));
    EXPECT_EQ(0u, sink_.n_logs_);     // 旧 sink 拿不到日志
}

TEST_F(T2ParserXeleSktTest, unregistering_sink_keeps_public_calls_crash_free)
{
    parser_.registerSpi(NULL);
    CodeSet subs;
    subs.insert("SHFE.ag2512");
    EXPECT_NO_THROW(parser_.subscribe(subs));
    EXPECT_NO_THROW(parser_.unsubscribe(subs));
    EXPECT_NO_THROW(parser_.release());
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_EQ(0u, sink_.n_events_);
}

TEST_F(T2ParserXeleSktTest, subscribe_is_additive_and_repeatable)
{
    CodeSet subs;
    subs.insert("SHFE.ag2512");
    subs.insert("SHFE.au2506");
    parser_.subscribe(subs);
    EXPECT_NO_THROW(parser_.subscribe(subs));   // 重复订阅同一集合
    EXPECT_NO_THROW(parser_.subscribe(subs));
    EXPECT_FALSE(parser_.isConnected());        // 订阅不开组播口
    EXPECT_EQ(0u, sink_.n_quotes_);
}

TEST_F(T2ParserXeleSktTest, unsubscribe_is_an_empty_stub_today)
{
    CodeSet subs;
    subs.insert("SHFE.ag2512");
    parser_.subscribe(subs);
    // 空实现：退订不会关口、不会推事件、也不会报错（钉死当前行为）
    EXPECT_NO_THROW(parser_.unsubscribe(subs));
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_EQ(0u, sink_.n_events_);
    EXPECT_EQ(0u, sink_.n_logs_);
}

TEST_F(T2ParserXeleSktTest, release_body_is_empty)
{
    EXPECT_NO_THROW(parser_.release());
    EXPECT_NO_THROW(parser_.release());
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_EQ(0u, sink_.n_events_);
}

TEST_F(T2ParserXeleSktTest, disconnect_without_connect_closes_nothing)
{
    EXPECT_TRUE(parser_.disconnect());          // _udp_socket==NULL => 跳过关口
    EXPECT_FALSE(parser_.isConnected());
    EXPECT_TRUE(parser_.disconnect());          // 幂等
    EXPECT_EQ(0u, sink_.n_events_);             // doOnDisconnected 排在 strand 上，无人 run
    CodeSet subs;
    subs.insert("DCE.m2401");
    EXPECT_NO_THROW(parser_.subscribe(subs));
}

TEST_F(T2ParserXeleSktTest, DISABLED_connect_joins_multicast_and_starts_parser_thread)
{
    // connect() -> reconnect() 会 bind 组播口 + join_group，并 new 一个 StdThread 跑 io_context::run；
    // 断言规则禁止起线程/真实网络，且断言前无超时出口（会挂），整条改名 DISABLED_。
    GTEST_SKIP() << "需要真实易盛组播环境";
}
