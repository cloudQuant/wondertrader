/**
 * r181_wtcore_rest_test.cpp — lane R181 / wave26
 *
 * 目标：WtCore 剩余公开面中仍可安全触达的几段：
 *   - TraderAdapter::buy/sell 的 AT_CloseYestoday 非 CoverToday 日志分支
 *     （TraderAdapter.cpp:852-856、:1138-1142）。用 p11 的进程内交易通道替身
 *     + 真实 ActionPolicyMgr（临时策略文件）装配，不接柜台、不起线程。
 *   - WtCtaRtTicker/WtSelRtTicker/WtHftRtTicker 的 run() 自动闭合里
 *     `_store->onMinuteEnd` 分支（WtCtaTicker.cpp:228、WtSelTicker.cpp:209、
 *     WtHftTicker.cpp:204），以及 Cta/Sel run() 的二次调用早退
 *     （WtCtaTicker.cpp:174、WtSelTicker.cpp:163）。
 *
 * 全程只走公开 API：ticker 的初始 _date/_time 由 init() 自行取当前时间，
 * 用例把行情日期设为“明天 00:01:59.999”，保证 on_tick 的“行情早于本地时间”
 * 早退不会命中；随后 run() 线程闭合 00:02，仍在 0:00-23:59 交易时段内，
 * 不会落入 10 秒休市 sleep。不使用 #define private public、不镜像类布局。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

#include <boost/core/noncopyable.hpp>

#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"
#include "q01_sel_env.h"

#include <Includes/IDataReader.h>
#include <Includes/WTSCollection.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/ActionPolicyMgr.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHftEngine.h>
#include <WtCore/WtHftTicker.h>
#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>
#include <WtCore/WtSelTicker.h>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 每个用例独立的生成目录，析构时整棵删除。 */
class TempEnv
{
public:
    explicit TempEnv(const char* /*tag*/)
    {
        // WtHelper::getBaseDir() 在进程内只按首次调用记忆化，所以同一进程的
        // 用例共用一棵根目录；每次构造重建、析构删除，绝不落到仓库目录。
        static std::string root;
        if (root.empty())
        {
            root = (fs::temp_directory_path()
                / ("r181_wtcore_" + std::to_string(static_cast<long long>(::getpid()))))
                       .string()
                + "/";
        }
        std::error_code ec;
        fs::remove_all(fs::path(root), ec);
        fs::create_directories(root, ec);
        _dir = root;
        WtHelper::setGenerateDir(_dir.c_str());
    }

    ~TempEnv()
    {
        std::error_code ec;
        fs::remove_all(fs::path(_dir), ec);
    }

    const std::string& dir() const { return _dir; }

private:
    std::string _dir;
};

/** 0:00-23:59 的单节交易时段。 */
WTSSessionInfo* makeSession()
{
    WTSSessionInfo* s = WTSSessionInfo::create("TRADING", "TRADING", 0);
    s->addTradingSection(0, 2359);
    return s;
}

/**
 * ticker 用例的时段：默认 0:00-23:59；若恰好在 23:59 运行，则该时段
 * 会把“当前分钟”判为最后一分钟、线程先 sleep 10 秒，故改用 0:00-0:00
 * 的 24 小时时段（close 记作 2400），保证 run() 首轮不休市。
 */
void registerTickerSession(q01sel::BaseDataMgr& bd)
{
    if (TimeUtils::getCurMin() / 100 == 2359)
        bd.regisSessionWithTime("TRADING", 0, 0);
    else
        bd.regisSessionWithTime("TRADING", 0, 2359);
}

/** SHFE.ag.2512，coverMode 由调用方指定（CM_OpenCover 才走非 CoverToday 日志）。 */
WTSContractInfo* makeContract(p11fix::FakeBaseDataMgr& bd, WTSSessionInfo* sess,
                              CoverMode coverMode, WTSCommodityInfo** outComm)
{
    WTSCommodityInfo* comm = WTSCommodityInfo::create("ag", "silver", "SHFE", "TRADING", "CHINA");
    comm->setCategory(CC_Future);
    comm->setCoverMode(coverMode);
    comm->setTradingMode(TM_Both);
    comm->setVolScale(1);
    comm->setPriceTick(1.0);
    comm->setSessionInfo(sess);

    WTSContractInfo* ct = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
    ct->setAltCode("ag2512");
    ct->setVolumeLimits(1000, 2000);
    bd.regis(comm, ct);
    *outComm = comm;
    return ct;
}

/** 只含 closeyestoday 的默认动作策略，确保 buy/sell 命中指定分支。 */
bool initPolicy(ActionPolicyMgr& policy, const std::string& dir)
{
    const std::string path = dir + "policy.json";
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    ofs << "{\"default\":{\"order\":[{\"action\":\"closeyestoday\"}],"
           "\"filters\":[\"SHFE.ag\"]}}";
    ofs.close();
    return policy.init(path.c_str());
}

WTSPositionItem* makePos(WTSContractInfo* ct, WTSDirectionType dir,
                         double prePos, double preAvail)
{
    WTSPositionItem* p = WTSPositionItem::create(ct->getCode(), "CNY", ct->getExchg());
    p->setContractInfo(ct);
    p->setDirection(dir);
    p->setPrePosition(prePos);
    p->setAvailPrePos(preAvail);
    p->setNewPosition(0);
    p->setAvailNewPos(0);
    p->setInitPosition(prePos);
    p->setAvgPrice(3500.0);
    return p;
}

/** 把一条持仓回报送进适配器（公开 ITraderSpi 回调）。 */
void pushPos(TraderAdapter& trader, WTSContractInfo* ct, WTSDirectionType dir,
             double prePos, double preAvail)
{
    WTSArray* arr = WTSArray::create();
    arr->append(makePos(ct, dir, prePos, preAvail));
    trader.onRspPosition(arr);
}

/** 记录 onMinuteEnd 的数据读取器替身。 */
class R181Reader final : public IDataReader
{
public:
    void onMinuteEnd(uint32_t d, uint32_t t, uint32_t e = 0) override
    {
        last_date.store(d);
        last_time.store(t);
        last_end.store(e);
        count.fetch_add(1);
    }

    WTSTickSlice* readTickSlice(const char*, uint32_t, uint64_t = 0) override { return NULL; }
    WTSKlineSlice* readKlineSlice(const char*, WTSKlinePeriod, uint32_t, uint64_t = 0) override
    {
        return NULL;
    }

    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> last_date{0};
    std::atomic<uint32_t> last_time{0};
    std::atomic<uint32_t> last_end{0};
};

/** 明天 00:01:59.999 的 ag2512 行情：必然晚于 init() 取的本地时间。 */
uint32_t nextDayTick(uint32_t* tickDate)
{
    *tickDate = TimeUtils::getNextDate(TimeUtils::getCurDate(), 1);
    return 159999u;
}

bool waitReader(const R181Reader& reader, int loops)
{
    for (int i = 0; i < loops && reader.count.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return reader.count.load() > 0;
}

WTSTickData* makeAgTick(q01sel::BaseDataMgr& bd, uint32_t date, uint32_t time)
{
    WTSTickData* t = q01sel::makeTick("SHFE", "ag2512", date, date, time, 3500.0);
    t->setContractInfo(bd.getContract("ag2512", "SHFE"));
    return t;
}

}  // namespace

//////////////////////////////////////////////////////////////////
// 买入平空昨仓：规则只有 closeyestoday，coverMode=CM_OpenCover，
// 走 TraderAdapter.cpp:852-856 的 else 日志分支；委托方向空、平仓价。
//////////////////////////////////////////////////////////////////
TEST(R181TraderCloseYest, BuyShortPrePositionUsesCloseNotCloseToday)
{
    TempEnv env("trader_buy_short");
    p11fix::FakeTraderApi api;
    p11fix::FakeBaseDataMgr bd;
    WTSSessionInfo* sess = makeSession();
    WTSCommodityInfo* comm = NULL;
    WTSContractInfo* ct = makeContract(bd, sess, CM_OpenCover, &comm);
    (void)comm;

    ActionPolicyMgr policy;
    ASSERT_TRUE(initPolicy(policy, env.dir()));

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r181_buy_short", &api, &bd, &policy));
    ASSERT_TRUE(trader.run());
    pushPos(trader, ct, WDT_SHORT, 2, 2);

    OrderIDs ids = trader.buy("SHFE.ag.2512", 3500.0, 2.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api.n_insert_);
    EXPECT_EQ(WDT_SHORT, api.last_dir_);
    EXPECT_EQ(WOT_CLOSE, api.last_offset_);
    EXPECT_DOUBLE_EQ(2.0, api.last_vol_);
    EXPECT_EQ("ag2512", api.last_code_);

    trader.release();
}

//////////////////////////////////////////////////////////////////
// 卖出平多昨仓：同一策略映射到 sell 侧，
// 走 TraderAdapter.cpp:1138-1142 的 else 日志分支。
//////////////////////////////////////////////////////////////////
TEST(R181TraderCloseYest, SellLongPrePositionUsesCloseNotCloseToday)
{
    TempEnv env("trader_sell_long");
    p11fix::FakeTraderApi api;
    p11fix::FakeBaseDataMgr bd;
    WTSSessionInfo* sess = makeSession();
    WTSCommodityInfo* comm = NULL;
    WTSContractInfo* ct = makeContract(bd, sess, CM_OpenCover, &comm);
    (void)comm;

    ActionPolicyMgr policy;
    ASSERT_TRUE(initPolicy(policy, env.dir()));

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r181_sell_long", &api, &bd, &policy));
    ASSERT_TRUE(trader.run());
    pushPos(trader, ct, WDT_LONG, 2, 2);

    OrderIDs ids = trader.sell("SHFE.ag.2512", 3500.0, 2.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api.n_insert_);
    EXPECT_EQ(WDT_LONG, api.last_dir_);
    EXPECT_EQ(WOT_CLOSE, api.last_offset_);
    EXPECT_DOUBLE_EQ(2.0, api.last_vol_);

    trader.release();
}

//////////////////////////////////////////////////////////////////
// 昨仓总量 5、可平 2：平昨规则只能下 2 手，剩余 3 手不再拆单。
// 与用例 1 同一条生产行段，但验证 maxQty=min(left, s_preavail) 的取值。
//////////////////////////////////////////////////////////////////
TEST(R181TraderCloseYest, BuyClampsToPreAvail)
{
    TempEnv env("trader_buy_clamp");
    p11fix::FakeTraderApi api;
    p11fix::FakeBaseDataMgr bd;
    WTSSessionInfo* sess = makeSession();
    WTSCommodityInfo* comm = NULL;
    WTSContractInfo* ct = makeContract(bd, sess, CM_OpenCover, &comm);
    (void)comm;

    ActionPolicyMgr policy;
    ASSERT_TRUE(initPolicy(policy, env.dir()));

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r181_buy_clamp", &api, &bd, &policy));
    ASSERT_TRUE(trader.run());
    pushPos(trader, ct, WDT_SHORT, 5, 2);

    OrderIDs ids = trader.buy("SHFE.ag.2512", 3500.0, 5.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api.n_insert_);
    EXPECT_DOUBLE_EQ(2.0, api.last_vol_);
    EXPECT_EQ(WOT_CLOSE, api.last_offset_);

    trader.release();
}

//////////////////////////////////////////////////////////////////
// 卖出侧同样的可平量裁剪：多头昨仓 5、可平 2 -> 只下 2 手。
//////////////////////////////////////////////////////////////////
TEST(R181TraderCloseYest, SellClampsToPreAvail)
{
    TempEnv env("trader_sell_clamp");
    p11fix::FakeTraderApi api;
    p11fix::FakeBaseDataMgr bd;
    WTSSessionInfo* sess = makeSession();
    WTSCommodityInfo* comm = NULL;
    WTSContractInfo* ct = makeContract(bd, sess, CM_OpenCover, &comm);
    (void)comm;

    ActionPolicyMgr policy;
    ASSERT_TRUE(initPolicy(policy, env.dir()));

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r181_sell_clamp", &api, &bd, &policy));
    ASSERT_TRUE(trader.run());
    pushPos(trader, ct, WDT_LONG, 5, 2);

    OrderIDs ids = trader.sell("SHFE.ag.2512", 3500.0, 5.0, 0, false);
    ASSERT_EQ(1u, ids.size());
    ASSERT_EQ(1u, api.n_insert_);
    EXPECT_DOUBLE_EQ(2.0, api.last_vol_);
    EXPECT_EQ(WOT_CLOSE, api.last_offset_);

    trader.release();
}

//////////////////////////////////////////////////////////////////
// 数量为 0 的买入指令不下单，验证公开入口的早退。
//////////////////////////////////////////////////////////////////
TEST(R181TraderCloseYest, ZeroQtyDoesNotReachApi)
{
    TempEnv env("trader_zero");
    p11fix::FakeTraderApi api;
    p11fix::FakeBaseDataMgr bd;
    WTSSessionInfo* sess = makeSession();
    WTSCommodityInfo* comm = NULL;
    WTSContractInfo* ct = makeContract(bd, sess, CM_OpenCover, &comm);
    (void)comm;

    ActionPolicyMgr policy;
    ASSERT_TRUE(initPolicy(policy, env.dir()));

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt("r181_zero", &api, &bd, &policy));
    ASSERT_TRUE(trader.run());
    pushPos(trader, ct, WDT_SHORT, 2, 2);

    EXPECT_TRUE(trader.buy("SHFE.ag.2512", 3500.0, 0.0, 0, false).empty());
    EXPECT_EQ(0u, api.n_insert_);

    trader.release();
}

//////////////////////////////////////////////////////////////////
// CTA run()：on_tick 把 _cur_pos 推到下一分钟，run 线程随后闭合，
// 必须把闭合事件转给 IDataReader::onMinuteEnd（endTDate=0）；
// 再次 run() 命中 _thrd 非空的早退。
// src/WtCore/WtCtaTicker.cpp:174、:228
//////////////////////////////////////////////////////////////////
TEST(R181TickerRun, CtaStoreGetsMinuteEndAndSecondRunReturns)
{
    TempEnv env("cta_store");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    registerTickerSession(bd);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    WtDtMgr dm;
    TraderAdapterMgr adapterMgr;
    WtCtaEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();
    engine.set_adapter_mgr(&adapterMgr);

    R181Reader reader;
    WtCtaRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    uint32_t tickDate = 0;
    uint32_t tickTime = nextDayTick(&tickDate);
    WTSTickData* t = makeAgTick(bd, tickDate, tickTime);
    ticker.on_tick(t);
    t->release();

    ASSERT_TRUE(waitReader(reader, 400)) << "CTA ticker did not close next minute";
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(2u, reader.last_time.load());
    EXPECT_EQ(0u, reader.last_end.load());

    ticker.run();  // _thrd 已存在：早退
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// SEL run()：同样验证 _store->onMinuteEnd 与二次 run 早退。
// src/WtCore/WtSelTicker.cpp:163、:209
//////////////////////////////////////////////////////////////////
TEST(R181TickerRun, SelStoreGetsMinuteEndAndSecondRunReturns)
{
    TempEnv env("sel_store");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    registerTickerSession(bd);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    WtDtMgr dm;

    // BUG-R42-001：构造未初始化 _tm_ticker，析构 release 会读脏指针，堆分配不回收。
    WtSelEngine& engine = *new WtSelEngine();
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();

    R181Reader reader;
    WtSelRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    uint32_t tickDate = 0;
    uint32_t tickTime = nextDayTick(&tickDate);
    WTSTickData* t = makeAgTick(bd, tickDate, tickTime);
    ticker.on_tick(t, 0);
    t->release();

    ASSERT_TRUE(waitReader(reader, 400)) << "SEL ticker did not close next minute";
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(2u, reader.last_time.load());
    EXPECT_EQ(0u, reader.last_end.load());

    ticker.run();
    ticker.stop();
}

//////////////////////////////////////////////////////////////////
// HFT run()：_time 为 HHMM 约定，闭合后仍被 isInTradingTime(0) 视为交易中，
// 可安全验证 _store->onMinuteEnd 分支。
// src/WtCore/WtHftTicker.cpp:204
//////////////////////////////////////////////////////////////////
TEST(R181TickerRun, HftStoreGetsMinuteEnd)
{
    TempEnv env("hft_store");
    q01sel::BaseDataMgr bd;
    q01sel::HotMgr hot;
    registerTickerSession(bd);
    WTSSessionInfo* sess = bd.session("TRADING");
    WTSCommodityInfo* ag = q01sel::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, q01sel::makeCt("SHFE", "ag", "ag2512"));

    WtDtMgr dm;
    WtHftEngine engine;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, &dm, &hot, NULL);
    cfg->release();

    R181Reader reader;
    WtHftRtTicker ticker(&engine);
    ticker.init(&reader, "TRADING");
    ticker.run();

    uint32_t tickDate = 0;
    uint32_t tickTime = nextDayTick(&tickDate);
    WTSTickData* t = makeAgTick(bd, tickDate, tickTime);
    ticker.on_tick(t);
    t->release();

    ASSERT_TRUE(waitReader(reader, 400)) << "HFT ticker did not close next minute";
    EXPECT_EQ(1u, reader.count.load());
    EXPECT_EQ(2u, reader.last_time.load());
    EXPECT_EQ(0u, reader.last_end.load());

    ticker.stop();
}
