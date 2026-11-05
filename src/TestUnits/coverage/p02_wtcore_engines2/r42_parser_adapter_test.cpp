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
 *
 * 迭代03（卡9 9a=A+C/9b=A，I03 dt-parsers 包，BUG-R42-003）：WtCore ParserAdapter
 * 只在 parser lifecycle V2 证明排空后销毁 API；initExt 外部 API 没有 V2 表时保留、不销毁。
 * 旧用例通过 setRemover 注入删除函数并以 prove_callbacks_drained() 代表排空，现改为经
 * ParserAdapterTestAccess::setLifecycle 给替身挂一张进程内 V2 表：wait_drained 仅在替身
 * 证明排空（且未被要求延后）时返回 DRAINED，destroy_api 在模块侧完成最终 release 与删除；
 * “删除函数抛异常/延后删除”分别对应 destroy_api 返回 ERROR（永久隔离、不重试）与
 * wait_drained 返回 PENDING（保留、可重试）。忙碌时转入的候选 API 无 V2 证明，按 9a 保留。
 * 原测试意图（仅在排空证明后释放一次、失败 fail-closed 不重试、管理器保留失败 owner）不变。
 */
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "r42_core_fakes.h"

#include <WtCore/ParserAdapter.h>
#include <Includes/IParserApi.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

using namespace wtp;

namespace wtp
{
struct ParserAdapterTestAccess
{
    static void setRemover(ParserAdapter& adapter, FuncDeleteParser remover)
    {
        adapter._remover = remover;
    }

    // Attach an in-process lifecycle V2 table to the API accepted by initExt.
    static void setLifecycle(ParserAdapter& adapter, const WtParserLifecycleApiV2* table)
    {
        adapter._lifecycle = table;
    }

    static IParserApi* getOwnedApi(const ParserAdapter& adapter)
    {
        return adapter._parser_api;
    }
};
}

namespace {

static const char* kParserCode = "SHFE.rb.2510";
int gThrowingParserRemoverCalls = 0;	// failing destroy_api attempts
int gDeferringParserRemoverCalls = 0;	// wait_drained polls while deletion is deferred
bool gParserRemoverCanDelete = false;	// deferred mode: deletion permitted
bool gParserDeferDeletion = false;		// wait_drained stays PENDING until permitted
bool gParserDestroyFails = false;		// destroy_api reports ERROR after release

class R42ParserApi : public IParserApi
{
public:
    bool init_ok = true;
    bool throw_on_release = false;
    int spi_calls = 0;
    int init_calls = 0;
    int subscribe_calls = 0;
    int connect_calls = 0;
    int* ext_connect_calls = NULL;
    int release_calls = 0;
    size_t last_set_size = 0;
    IParserSpi* last_spi = NULL;
    int* ext_release_calls = NULL;
    int* ext_release = NULL;
    int* ext_destroyed = NULL;
    int* ext_unsafe_release = NULL;
    int* ext_unsafe_destroy = NULL;

    ~R42ParserApi() override
    {
        if (ext_destroyed != NULL)
            ++(*ext_destroyed);
        if (!_callbacks_drained && ext_unsafe_destroy != NULL)
            ++(*ext_unsafe_destroy);
    }

    bool init(WTSVariant*) override { ++init_calls; return init_ok; }
    void release() override
    {
        ++release_calls;
        if (ext_release_calls != NULL)
            ++(*ext_release_calls);
        if (ext_release != NULL)
            ++(*ext_release);
        if (ext_unsafe_release != NULL && !_callbacks_drained)
            ++(*ext_unsafe_release);
        if (throw_on_release)
            throw std::runtime_error("controlled parser release failure");
    }
    void prove_callbacks_drained() { _callbacks_drained = true; }
    bool callbacks_drained() const { return _callbacks_drained; }
    bool connect() override
    {
        ++connect_calls;
        if (ext_connect_calls != NULL)
            ++(*ext_connect_calls);
        return true;
    }
    void subscribe(const CodeSet& setCodes) override
    {
        ++subscribe_calls;
        last_set_size = setCodes.size();
    }
    void registerSpi(IParserSpi* spi) override { ++spi_calls; last_spi = spi; }

private:
    bool _callbacks_drained = false;
};

// In-process lifecycle V2 table for the R42ParserApi fake (see file header).
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r42Preflight() { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r42RequestStop(void*) { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r42CanWait(void*) { return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r42WaitDrained(void* api, uint32_t)
{
    R42ParserApi* parser = static_cast<R42ParserApi*>(static_cast<IParserApi*>(api));
    if (gParserDeferDeletion && !gParserRemoverCanDelete)
    {
        ++gDeferringParserRemoverCalls;
        return WT_PARSER_LIFECYCLE_V2_PENDING;
    }
    return parser->callbacks_drained() ? WT_PARSER_LIFECYCLE_V2_DRAINED : WT_PARSER_LIFECYCLE_V2_PENDING;
}
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL r42DestroyApi(void** api)
{
    R42ParserApi* parser = static_cast<R42ParserApi*>(static_cast<IParserApi*>(*api));
    try
    {
        parser->release();		// final synchronous provider release
    }
    catch (...)
    {
        return WT_PARSER_LIFECYCLE_V2_ERROR;	// partial teardown: keep the handle
    }
    if (gParserDestroyFails)
    {
        ++gThrowingParserRemoverCalls;
        return WT_PARSER_LIFECYCLE_V2_ERROR;
    }
    delete parser;
    *api = NULL;
    return WT_PARSER_LIFECYCLE_V2_OK;
}
const WtParserLifecycleApiV2 kR42Lifecycle = {
    sizeof(WtParserLifecycleApiV2), WT_PARSER_LIFECYCLE_V2_ABI_VERSION,
    WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0, 0},
    r42Preflight, r42RequestStop, r42CanWait, r42WaitDrained, r42DestroyApi };

void resetLifecycleModes()
{
    gThrowingParserRemoverCalls = 0;
    gDeferringParserRemoverCalls = 0;
    gParserRemoverCanDelete = false;
    gParserDeferDeletion = false;
    gParserDestroyFails = false;
}

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
        resetLifecycleModes();
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
    int bad_destroyed = 0;
    int bad_unsafe_release = 0;
    int bad_unsafe_destroy = 0;
    bad->ext_release = &bad_released;
    bad->ext_destroyed = &bad_destroyed;
    bad->ext_unsafe_release = &bad_unsafe_release;
    bad->ext_unsafe_destroy = &bad_unsafe_destroy;
    EXPECT_FALSE(adapter.initExt("r42_bad", bad, &stub, &bd, &hot));
    ParserAdapterTestAccess::setLifecycle(adapter, &kR42Lifecycle);
    EXPECT_EQ(bad->spi_calls, 1);
    EXPECT_EQ(bad->init_calls, 1);
    EXPECT_EQ(bad->subscribe_calls, 0);
    EXPECT_EQ(adapter.getBaseDataMgr(), &bd);
    EXPECT_TRUE(adapter.hasApi());
    EXPECT_FALSE(adapter.run());
    EXPECT_EQ(bad->connect_calls, 0);
    EXPECT_EQ(bad_destroyed, 0);
    EXPECT_EQ(bad_released, 0);
    bad->prove_callbacks_drained();
    adapter.release();
    EXPECT_EQ(bad_released, 1);
    EXPECT_EQ(bad_destroyed, 1);
    EXPECT_EQ(bad_unsafe_release, 0);
    EXPECT_EQ(bad_unsafe_destroy, 0);
    EXPECT_FALSE(adapter.hasApi());

    ParserAdapter good;
    R42ParserApi* api = new R42ParserApi();
    int good_released = 0;
    api->ext_release = &good_released;
    EXPECT_TRUE(good.initExt("r42_good", api, &stub, &bd, &hot));
    ParserAdapterTestAccess::setLifecycle(good, &kR42Lifecycle);
    EXPECT_EQ(api->spi_calls, 1);
    EXPECT_EQ(api->subscribe_calls, 1);
    EXPECT_EQ(api->last_set_size, 1u);
    EXPECT_EQ(api->last_spi, &good);
    EXPECT_STREQ(good.id(), "r42_good");
    api->prove_callbacks_drained();
    good.release();
    EXPECT_EQ(good_released, 1);
}

TEST_F(R42ParserAdapterTest, BusyInitExtRetainsAndDeduplicatesTransferredApis)
{
    ParserAdapter adapter;
    R42ParserApi* primary = new R42ParserApi();
    int primary_released = 0;
    int primary_destroyed = 0;
    int primary_unsafe_release = 0;
    int primary_unsafe_destroy = 0;
    primary->ext_release = &primary_released;
    primary->ext_destroyed = &primary_destroyed;
    primary->ext_unsafe_release = &primary_unsafe_release;
    primary->ext_unsafe_destroy = &primary_unsafe_destroy;
    ASSERT_TRUE(adapter.initExt("r42_primary", primary, &stub, &bd, &hot));
    ParserAdapterTestAccess::setLifecycle(adapter, &kR42Lifecycle);

    R42ParserApi* rejected = new R42ParserApi();
    int rejected_released = 0;
    int rejected_destroyed = 0;
    int rejected_unsafe_release = 0;
    int rejected_unsafe_destroy = 0;
    rejected->ext_release = &rejected_released;
    rejected->ext_destroyed = &rejected_destroyed;
    rejected->ext_unsafe_release = &rejected_unsafe_release;
    rejected->ext_unsafe_destroy = &rejected_unsafe_destroy;

    // A busy adapter still takes ownership of a distinct non-null candidate,
    // while duplicate submissions of either pointer create no second owner.
    EXPECT_FALSE(adapter.initExt("r42_rejected", rejected, &stub, &bd, &hot));
    EXPECT_FALSE(adapter.initExt("r42_rejected_again", rejected, &stub, &bd, &hot));
    EXPECT_FALSE(adapter.initExt("r42_primary_again", primary, &stub, &bd, &hot));
    EXPECT_TRUE(adapter.hasApi());
    EXPECT_EQ(0, rejected->spi_calls);
    EXPECT_EQ(0, rejected->init_calls);
    EXPECT_EQ(0, primary_destroyed);
    EXPECT_EQ(0, rejected_destroyed);
    EXPECT_EQ(0, primary_released);
    EXPECT_EQ(0, rejected_released);

    // The primary API carries the V2 drain proof and is destroyed exactly once.
    // The busy candidate has no V2 table: under card 9a it stays retained (never
    // released or destroyed) and keeps the adapter owning an API. The former
    // oracle expected the rejected candidate to be released as well.
    primary->prove_callbacks_drained();
    rejected->prove_callbacks_drained();
    adapter.release();
    EXPECT_EQ(1, primary_released);
    EXPECT_EQ(1, primary_destroyed);
    EXPECT_EQ(0, rejected_released);
    EXPECT_EQ(0, rejected_destroyed);
    EXPECT_EQ(0, primary_unsafe_release);
    EXPECT_EQ(0, primary_unsafe_destroy);
    EXPECT_EQ(0, rejected_unsafe_release);
    EXPECT_EQ(0, rejected_unsafe_destroy);
    EXPECT_TRUE(adapter.hasApi());		// deliberately leaked retained candidate
}

TEST_F(R42ParserAdapterTest, ReleaseExceptionFailsClosedWithoutRetry)
{
    // Keep this deliberately unresolved fake reachable after teardown throws.
    // The adapter cannot determine whether the provider partially released
    // itself, so neither another release attempt nor destruction is safe.
    static int released = 0;
    static int destroyed = 0;
    static int unsafe_release = 0;
    static int unsafe_destroy = 0;
    static R42ParserApi api_storage;
    R42ParserApi* api = &api_storage;
    released = 0;
    destroyed = 0;
    unsafe_release = 0;
    unsafe_destroy = 0;
    api->release_calls = 0;
    api->init_ok = true;
    api->throw_on_release = true;
    api->ext_release = &released;
    api->ext_destroyed = &destroyed;
    api->ext_unsafe_release = &unsafe_release;
    api->ext_unsafe_destroy = &unsafe_destroy;

    ParserAdapter adapter;
    ASSERT_TRUE(adapter.initExt("r42_release_throw", api, &stub, &bd, &hot));
    ParserAdapterTestAccess::setLifecycle(adapter, &kR42Lifecycle);
    api->prove_callbacks_drained();

    adapter.release();
    EXPECT_EQ(1, released);
    EXPECT_EQ(1, api->release_calls);
    EXPECT_EQ(0, destroyed);
    EXPECT_EQ(0, unsafe_release);
    EXPECT_EQ(0, unsafe_destroy);
    EXPECT_TRUE(adapter.hasApi());
    EXPECT_FALSE(adapter.isInitialized());
    EXPECT_FALSE(adapter.run());

    // A retry must not repeat a teardown call that may have partially run.
    adapter.release();
    EXPECT_EQ(1, released);
    EXPECT_EQ(1, api->release_calls);
    EXPECT_EQ(0, destroyed);
    EXPECT_TRUE(adapter.hasApi());
}

TEST_F(R42ParserAdapterTest, RemoverExceptionPreservesOwnerAndFailsClosed)
{
    static int released = 0;
    static int destroyed = 0;
    static int unsafe_release = 0;
    static int unsafe_destroy = 0;
    static R42ParserApi api_storage;
    R42ParserApi* api = &api_storage;
    released = 0;
    destroyed = 0;
    unsafe_release = 0;
    unsafe_destroy = 0;
    gThrowingParserRemoverCalls = 0;
    api->release_calls = 0;
    api->init_ok = true;
    api->throw_on_release = false;
    api->ext_release = &released;
    api->ext_destroyed = &destroyed;
    api->ext_unsafe_release = &unsafe_release;
    api->ext_unsafe_destroy = &unsafe_destroy;

    ParserAdapter adapter;
    ASSERT_TRUE(adapter.initExt("r42_remover_throw", api, &stub, &bd, &hot));
    api->prove_callbacks_drained();
    // V2 equivalent of the former throwing deleteParser: destroy_api reports
    // ERROR after the provider release, so the owner is quarantined for good.
    gParserDestroyFails = true;
    ParserAdapterTestAccess::setLifecycle(adapter, &kR42Lifecycle);

    adapter.release();
    EXPECT_EQ(1, released);
    EXPECT_EQ(1, api->release_calls);
    EXPECT_EQ(1, gThrowingParserRemoverCalls);
    EXPECT_EQ(0, destroyed);
    EXPECT_EQ(0, unsafe_release);
    EXPECT_EQ(0, unsafe_destroy);
    EXPECT_EQ(api, ParserAdapterTestAccess::getOwnedApi(adapter));
    EXPECT_TRUE(adapter.hasApi());

    adapter.release();
    EXPECT_EQ(1, released);
    EXPECT_EQ(1, api->release_calls);
    EXPECT_EQ(1, gThrowingParserRemoverCalls);
    EXPECT_EQ(0, destroyed);
    EXPECT_TRUE(adapter.hasApi());
}

TEST_F(R42ParserAdapterTest, ParserAdapterMgrRetainsFailedOwner)
{
    static int released = 0;
    static int destroyed = 0;
    static int unsafe_release = 0;
    static int unsafe_destroy = 0;
    static R42ParserApi api_storage;
    R42ParserApi* api = &api_storage;
    released = 0;
    destroyed = 0;
    unsafe_release = 0;
    unsafe_destroy = 0;
    api->release_calls = 0;
    api->init_ok = true;
    api->throw_on_release = true;
    api->ext_release = &released;
    api->ext_destroyed = &destroyed;
    api->ext_unsafe_release = &unsafe_release;
    api->ext_unsafe_destroy = &unsafe_destroy;

    ParserAdapterMgr mgr;
    ParserAdapterPtr adapter(new ParserAdapter());
    ASSERT_TRUE(adapter->initExt("r42_mgr_throw", api, &stub, &bd, &hot));
    ParserAdapterTestAccess::setLifecycle(*adapter, &kR42Lifecycle);
    ASSERT_TRUE(mgr.addAdapter("r42_mgr_throw", adapter));
    std::weak_ptr<ParserAdapter> weak_adapter(adapter);
    api->prove_callbacks_drained();

    mgr.release();
    EXPECT_TRUE(mgr.hasQuarantinedAdapters());
    EXPECT_EQ(adapter, mgr.getAdapter("r42_mgr_throw"));
    EXPECT_EQ(1, released);
    EXPECT_EQ(0, destroyed);

    // The map itself must own the failed adapter after the caller drops its
    // reference, and another manager release must not touch the uncertain API.
    adapter.reset();
    EXPECT_FALSE(weak_adapter.expired());
    mgr.release();
    EXPECT_TRUE(mgr.hasQuarantinedAdapters());
    EXPECT_EQ(1, released);
    EXPECT_EQ(1, api->release_calls);
    EXPECT_EQ(0, destroyed);
    EXPECT_EQ(0, unsafe_release);
    EXPECT_EQ(0, unsafe_destroy);
}

TEST_F(R42ParserAdapterTest, ParserAdapterMgrRetriesDeferredRemoverWithoutLosingOwner)
{
    int released = 0;
    int api_release_calls = 0;
    int destroyed = 0;
    int unsafe_release = 0;
    int unsafe_destroy = 0;
    R42ParserApi* api = new R42ParserApi();
    api->ext_release_calls = &api_release_calls;
    api->ext_release = &released;
    api->ext_destroyed = &destroyed;
    api->ext_unsafe_release = &unsafe_release;
    api->ext_unsafe_destroy = &unsafe_destroy;
    gDeferringParserRemoverCalls = 0;
    gParserRemoverCanDelete = false;
    gParserDeferDeletion = true;

    ParserAdapterMgr mgr;
    ParserAdapterPtr adapter(new ParserAdapter());
    ASSERT_TRUE(adapter->initExt("r42_mgr_deferred", api, &stub, &bd, &hot));
    // V2 equivalent of the former deferring deleteParser: wait_drained stays
    // PENDING until deletion is permitted, so the owner is kept and retried.
    ParserAdapterTestAccess::setLifecycle(*adapter, &kR42Lifecycle);
    ASSERT_TRUE(mgr.addAdapter("r42_mgr_deferred", adapter));
    std::weak_ptr<ParserAdapter> weak_adapter(adapter);
    api->prove_callbacks_drained();

    // Drain is still pending: the manager must keep the adapter and its SPI, and
    // under V2 the provider release has not been issued yet (it happens inside
    // destroy_api after DRAINED). The former oracle expected release first.
    mgr.release();
    EXPECT_TRUE(mgr.hasQuarantinedAdapters());
    EXPECT_EQ(adapter, mgr.getAdapter("r42_mgr_deferred"));
    EXPECT_EQ(0, released);
    EXPECT_EQ(0, api_release_calls);
    EXPECT_EQ(0, api->release_calls);
    EXPECT_EQ(1, gDeferringParserRemoverCalls);
    EXPECT_EQ(0, destroyed);

    adapter.reset();
    EXPECT_FALSE(weak_adapter.expired());

    // Once the test provider permits deletion, the next safe drain pass reaches
    // DRAINED and destroys the API once (provider release exactly once).
    api->prove_callbacks_drained();
    gParserRemoverCanDelete = true;
    mgr.release();
    gParserRemoverCanDelete = false;
    EXPECT_FALSE(mgr.hasQuarantinedAdapters());
    EXPECT_TRUE(mgr.getAdapter("r42_mgr_deferred") == NULL);
    EXPECT_TRUE(weak_adapter.expired());
    EXPECT_EQ(1, released);
    EXPECT_EQ(1, api_release_calls);
    EXPECT_EQ(1, gDeferringParserRemoverCalls);
    EXPECT_EQ(1, destroyed);
    EXPECT_EQ(0, unsafe_release);
    EXPECT_EQ(0, unsafe_destroy);
}

TEST_F(R42ParserAdapterTest, ManagerReleaseStopsRetainedSuccessfulAdapter)
{
    int connect_calls = 0;
    int release_calls = 0;
    int destroyed = 0;
    int unsafe_release = 0;
    int unsafe_destroy = 0;
    R42ParserApi* api = new R42ParserApi();
    api->ext_connect_calls = &connect_calls;
    api->ext_release = &release_calls;
    api->ext_destroyed = &destroyed;
    api->ext_unsafe_release = &unsafe_release;
    api->ext_unsafe_destroy = &unsafe_destroy;

    ParserAdapterMgr mgr;
    ParserAdapterPtr retained(new ParserAdapter());
    ASSERT_TRUE(retained->initExt("r42_mgr_retained", api, &stub, &bd, &hot));
    ParserAdapterTestAccess::setLifecycle(*retained, &kR42Lifecycle);
    ASSERT_TRUE(mgr.addAdapter("r42_mgr_retained", retained));
    std::weak_ptr<ParserAdapter> weak_adapter(retained);

    // This fake connect is synchronous and starts no worker or callback source.
    mgr.run();
    EXPECT_EQ(1, connect_calls);

    // The fake provides an explicit callback-drained proof before manager teardown.
    api->prove_callbacks_drained();
    mgr.release();

    EXPECT_TRUE(mgr.getAdapter("r42_mgr_retained") == NULL);
    EXPECT_FALSE(mgr.hasQuarantinedAdapters());
    EXPECT_EQ(1, release_calls);
    EXPECT_EQ(1, destroyed);
    EXPECT_FALSE(retained->hasApi());
    EXPECT_FALSE(retained->run());
    EXPECT_EQ(1, connect_calls);
    EXPECT_EQ(0, unsafe_release);
    EXPECT_EQ(0, unsafe_destroy);

    // Neither another manager drain nor a retained adapter release may revive
    // the API or invoke its connect/release/destructor a second time.
    mgr.release();
    retained->release();
    EXPECT_FALSE(retained->run());
    EXPECT_EQ(1, connect_calls);
    EXPECT_EQ(1, release_calls);
    EXPECT_EQ(1, destroyed);
    EXPECT_FALSE(weak_adapter.expired());

    retained.reset();
    EXPECT_TRUE(weak_adapter.expired());
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

    // Tick 保持月码形态，但合同侧原始码过短，无法转换成标准代码。
    // 转换失败时不得清空原 tick 代码，也不得转发给 stub。
    WTSTickData* invalid_month = r42fakes::makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3500.0);
    WTSContractInfo* short_code = WTSContractInfo::create("rb", "rb", "SHFE", "rb");
    ASSERT_NE(short_code, nullptr);
    short_code->setCommInfo(bd.getCommodity("SHFE", "rb"));
    invalid_month->setContractInfo(short_code);
    adapter.handleQuote(invalid_month, 1);
    EXPECT_EQ(stub.quotes, 0);
    EXPECT_STREQ(invalid_month->code(), "rb2510");
    invalid_month->release();
    short_code->release();

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
    api->prove_callbacks_drained();
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
    api->prove_callbacks_drained();
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
    EXPECT_FALSE(mgr.hasQuarantinedAdapters());
}

} // namespace
