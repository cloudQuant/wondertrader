/**
 * r42_parser_adapter_test.cpp  (lane R42 / WtCore 行情解析适配器补测)
 *
 * 覆盖目标 src/WtCore/ParserAdapter.cpp：
 *   init 的三条失败出口（cfg 为空 / module 为空 / 模块加载失败）与重复 init
 *   initExt（api 为空 / api 初始化失败 / 成功订阅合约）
 *   run / release 的 api 生命周期
 *   handleQuote 的日期守卫、合约缺失守卫、期货/分月代码转换与 stub 转发
 *   handleOrderQueue / handleOrderDetail / handleTransaction 的日期与合约守卫
 *   handleParserLog（含 release 后的静默）
 *   ParserAdapterMgr 的 addAdapter / getAdapter / run / release
 *
 * 不加载任何真实行情插件：initExt 直接注入测试内的 IParserApi 替身。
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "r42_core_fakes.h"

#include <WtCore/ParserAdapter.h>
#include <Includes/IParserApi.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace {

static const char* kParserCode = "SHFE.rb.2510";

class R42ParserApi : public IParserApi
{
public:
    bool init_ok = true;
    int spi_calls = 0;
    int init_calls = 0;
    int subscribe_calls = 0;
    int connect_calls = 0;
    int release_calls = 0;
    size_t last_set_size = 0;
    IParserSpi* last_spi = NULL;
    int* ext_release = NULL;

    bool init(WTSVariant*) override { ++init_calls; return init_ok; }
    void release() override
    {
        ++release_calls;
        if (ext_release != NULL)
            ++(*ext_release);
    }
    bool connect() override { ++connect_calls; return true; }
    void subscribe(const CodeSet& setCodes) override
    {
        ++subscribe_calls;
        last_set_size = setCodes.size();
    }
    void registerSpi(IParserSpi* spi) override { ++spi_calls; last_spi = spi; }
};

class R42ParserStub : public IParserStub
{
public:
    int quotes = 0;
    int ord_ques = 0;
    int ord_dtls = 0;
    int trans = 0;
    std::string last_quote_code;

    void handle_push_quote(WTSTickData* curTick) override
    {
        ++quotes;
        last_quote_code = curTick->code();
    }
    void handle_push_order_queue(WTSOrdQueData*) override { ++ord_ques; }
    void handle_push_order_detail(WTSOrdDtlData*) override { ++ord_dtls; }
    void handle_push_transaction(WTSTransData*) override { ++trans; }
};

class R42ParserAdapterTest : public ::testing::Test
{
protected:
    r42fakes::R42GenDir      gen;
    r42fakes::R42BaseDataMgr bd;
    r42fakes::R42HotMgr      hot;
    R42ParserStub            stub;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r42fakes::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        bd.regis(rb, r42fakes::makeCt("SHFE", "rb", "rb2510"));
    }

    WTSVariant* cfg(const char* text)
    {
        return WTSCfgLoader::load_from_content(text);
    }
};

// 覆盖 ParserAdapter.cpp L85-125 init 的 cfg/module 守卫
TEST_F(R42ParserAdapterTest, InitRejectsNullOrMissingModule)
{
    ParserAdapter adapter;
    EXPECT_FALSE(adapter.init("r42", NULL, &stub, &bd, &hot));

    WTSVariant* c1 = cfg(R"json({"module": ""})json");
    ASSERT_NE(c1, nullptr);
    EXPECT_FALSE(adapter.init("r42", c1, &stub, &bd, &hot));
    c1->release();

    // 模块名存在但动态库不存在 -> load_library 失败
    WTSVariant* c2 = cfg(R"json({"module": "r42_no_such_parser"})json");
    ASSERT_NE(c2, nullptr);
    EXPECT_FALSE(adapter.init("r42", c2, &stub, &bd, &hot));
    c2->release();

    // 已持有 _cfg 时再次 init 直接失败
    EXPECT_FALSE(adapter.init("r42", NULL, &stub, &bd, &hot));
    adapter.release();}

// 覆盖 L45-83 initExt：空 api / api 初始化失败 / 成功注册并订阅
TEST_F(R42ParserAdapterTest, InitExtHandlesApiStates)
{
    ParserAdapter adapter;
    EXPECT_FALSE(adapter.initExt("r42", NULL, &stub, &bd, &hot));

    R42ParserApi* bad = new R42ParserApi();
    bad->init_ok = false;
    int bad_released = 0;
    bad->ext_release = &bad_released;
    EXPECT_TRUE(adapter.initExt("r42_bad", bad, &stub, &bd, &hot));
    EXPECT_EQ(bad->spi_calls, 1);
    EXPECT_EQ(bad->init_calls, 1);
    EXPECT_EQ(bad->subscribe_calls, 0);
    EXPECT_EQ(adapter.getBaseDataMgr(), &bd);
    EXPECT_TRUE(adapter.run());
    EXPECT_EQ(bad->connect_calls, 1);
    adapter.release();
    EXPECT_EQ(bad_released, 1);

    ParserAdapter good;
    R42ParserApi* api = new R42ParserApi();
    int good_released = 0;
    api->ext_release = &good_released;
    EXPECT_TRUE(good.initExt("r42_good", api, &stub, &bd, &hot));
    EXPECT_EQ(api->spi_calls, 1);
    EXPECT_EQ(api->subscribe_calls, 1);
    EXPECT_EQ(api->last_set_size, 1u);
    EXPECT_EQ(api->last_spi, &good);
    EXPECT_STREQ(good.id(), "r42_good");
    good.release();
    EXPECT_EQ(good_released, 1);
}

// 覆盖 L239-295 handleQuote 的守卫、代码转换与 stub 转发
TEST_F(R42ParserAdapterTest, HandleQuoteConvertsMonthlyCodeAndFiltersInvalid)
{
    ParserAdapter adapter;
    R42ParserApi* api = new R42ParserApi();
    ASSERT_TRUE(adapter.initExt("r42_q", api, &stub, &bd, &hot));

    adapter.handleQuote(NULL, 0);
    EXPECT_EQ(stub.quotes, 0);

    // 日期为 0 的快照被丢弃
    WTSTickData* no_date = r42fakes::makeTick("SHFE", "rb2510", 0, 0, 93100000, 3500.0);
    adapter.handleQuote(no_date, 1);
    no_date->release();
    EXPECT_EQ(stub.quotes, 0);

    // 未注册的合约被丢弃
    WTSTickData* unknown = r42fakes::makeTick("SHFE", "zz9999", 20250110, 20250110, 93100000, 1.0);
    adapter.handleQuote(unknown, 1);
    unknown->release();
    EXPECT_EQ(stub.quotes, 0);

    // 分月合约 -> 标准代码 SHFE.rb.2510
    WTSTickData* quote = r42fakes::makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3500.0);
    adapter.handleQuote(quote, 1);
    quote->release();
    EXPECT_EQ(stub.quotes, 1);
    EXPECT_EQ(stub.last_quote_code, kParserCode);

    // release 后 _stopped 生效，不再转发
    adapter.release();
    WTSTickData* after = r42fakes::makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3500.0);
    adapter.handleQuote(after, 1);
    after->release();
    EXPECT_EQ(stub.quotes, 1);
}

// 覆盖 L297-364 handleOrderQueue/Detail/Transaction 的守卫与转发
TEST_F(R42ParserAdapterTest, OrderAndTradeDataForwarding)
{
    ParserAdapter adapter;
    R42ParserApi* api = new R42ParserApi();
    ASSERT_TRUE(adapter.initExt("r42_l2", api, &stub, &bd, &hot));

    WTSOrdQueStruct oq;
    memset(&oq, 0, sizeof(oq));
    wt_strcpy(oq.exchg, "SHFE");
    wt_strcpy(oq.code, "rb2510");
    oq.action_date = 20250110;
    oq.trading_date = 20250110;
    WTSOrdQueData* qd = WTSOrdQueData::create(oq);
    adapter.handleOrderQueue(qd);
    qd->release();
    EXPECT_EQ(stub.ord_ques, 1);

    WTSOrdDtlStruct od;
    memset(&od, 0, sizeof(od));
    wt_strcpy(od.exchg, "SHFE");
    wt_strcpy(od.code, "rb2510");
    od.action_date = 20250110;
    od.trading_date = 20250110;
    WTSOrdDtlData* dd = WTSOrdDtlData::create(od);
    adapter.handleOrderDetail(dd);
    dd->release();
    EXPECT_EQ(stub.ord_dtls, 1);

    WTSTransStruct ts;
    memset(&ts, 0, sizeof(ts));
    wt_strcpy(ts.exchg, "SHFE");
    wt_strcpy(ts.code, "rb2510");
    ts.action_date = 20250110;
    ts.trading_date = 20250110;
    WTSTransData* td = WTSTransData::create(ts);
    adapter.handleTransaction(td);
    td->release();
    EXPECT_EQ(stub.trans, 1);

    // 日期缺失 / 合约缺失都不转发
    WTSOrdQueStruct oq2;
    memset(&oq2, 0, sizeof(oq2));
    wt_strcpy(oq2.exchg, "SHFE");
    wt_strcpy(oq2.code, "rb2510");
    WTSOrdQueData* qd2 = WTSOrdQueData::create(oq2);
    adapter.handleOrderQueue(qd2);
    qd2->release();

    WTSOrdDtlStruct od2;
    memset(&od2, 0, sizeof(od2));
    wt_strcpy(od2.exchg, "SHFE");
    wt_strcpy(od2.code, "zz9999");
    od2.action_date = 20250110;
    od2.trading_date = 20250110;
    WTSOrdDtlData* dd2 = WTSOrdDtlData::create(od2);
    adapter.handleOrderDetail(dd2);
    dd2->release();

    WTSTransStruct ts2;
    memset(&ts2, 0, sizeof(ts2));
    wt_strcpy(ts2.exchg, "SHFE");
    wt_strcpy(ts2.code, "zz9999");
    ts2.action_date = 20250110;
    ts2.trading_date = 20250110;
    WTSTransData* td2 = WTSTransData::create(ts2);
    adapter.handleTransaction(td2);
    td2->release();

    EXPECT_EQ(stub.ord_ques, 1);
    EXPECT_EQ(stub.ord_dtls, 1);
    EXPECT_EQ(stub.trans, 1);

    // 解析器日志在停止前会写入日志系统
    adapter.handleParserLog(LL_INFO, "r42 parser log");
    adapter.handleParserLog(LL_ERROR, "r42 parser error");
    adapter.release();
    adapter.handleParserLog(LL_INFO, "r42 after stop");
}

// 覆盖 L367-424 handleParserLog 与 ParserAdapterMgr 全部接口
TEST_F(R42ParserAdapterTest, AdapterManagerAddGetRunRelease)
{
    ParserAdapterMgr mgr;

    ParserAdapterPtr a1(new ParserAdapter());
    ParserAdapterPtr a2(new ParserAdapter());

    EXPECT_FALSE(mgr.addAdapter("", a1));
    ParserAdapterPtr nil;
    EXPECT_FALSE(mgr.addAdapter("p1", nil));
    EXPECT_TRUE(mgr.addAdapter("p1", a1));
    EXPECT_FALSE(mgr.addAdapter("p1", a2));

    EXPECT_EQ(mgr.getAdapter("p1"), a1);
    EXPECT_TRUE(mgr.getAdapter("missing") == NULL);

    // 没有 api 的适配器 run 返回 false，release 只清理空指针
    mgr.run();
    mgr.release();
    EXPECT_TRUE(mgr.getAdapter("p1") == NULL);
}

} // namespace
