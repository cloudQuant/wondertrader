/**
 * r3_trader_risklimit.cpp —— lane R3（WtCore 第三波）
 *
 * 覆盖 src/WtCore/TraderAdapter.cpp 的以下行段（行号按当前 dev 版本）：
 *   L237-279  initSaveData：trades.csv / orders.csv / rtdata.json 的落地目录（L245 create_directories）
 *   L303-390  saveData(WTSArray*)：positions(long/short 四段) + funds 段 JSON（含 L305-384）
 *   L428-466  getPosition（全量/可用两种口径）+ enumPosition
 *   L487-520  doEntrust：usertag 组装（_order_pattern + '.' + localid）+ orderInsert 失败返回 UINT_MAX
 *   L544-596  checkCancelLimits：撤单频率窗口（L566-593）
 *   L609-661  checkOrderLimits：下单频率窗口（L631-658）
 *   L663-830  buy()：qty==0 早退 / 非交易时段拒绝 / 规则限额 + 单笔最大委托量拆单
 *   L1244-1291 doCancel + cancel(localid)：撤单频率缓存写入（L1278 SpinLock、L1286 cache）
 *   L1293-1329 cancel(stdCode,isBuy,qty)：按方向与代码筛选活单
 *   L1331-1404 openLong/openShort/closeLong/closeShort：方向·开平·价格类型·orderflag 组装
 *   L1514-1543 onRspAccount -> saveData 带动
 * 交易通道仍用 lane p11 的 FakeTraderApi 替身（不起线程、不联网）。
 * 频率窗口的「过期条目裁剪」支路（L588-591 / L653-656）需要可控时钟：缓存里的时间戳由
 * TimeUtils::getLocalTimeNow() 在 doEntrust/doCancel 内部写入，公开接口无法注入历史时间点，
 * 本轮未打（见 bugs/R3_wtcore.md 的接缝清单）。
 */
// BUG-L00-004：TraderAdapter.h 用了 boost::noncopyable 却没 include，必须在自己的 .cpp 里先补
#include <boost/noncopyable.hpp>
#include <boost/core/noncopyable.hpp>

#include <cstdlib>

#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

#include <WtCore/ActionPolicyMgr.h>
#include <Includes/WTSSessionInfo.hpp>
#include <rapidjson/document.h>

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

const char* kStdAu = "SHFE.au.2212";

static std::string r3TmpDir(const char* tag)
{
    std::filesystem::path p = std::filesystem::temp_directory_path()
        / (std::string("r3_trader_") + tag + "_" + std::to_string((long)::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string() + "/";
}

static WTSVariant* r3LoadJson(const std::string& dir, const char* fname, const std::string& body)
{
    std::string path = dir + fname;
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    ofs << body;
    ofs.flush();
    ofs.close();
    return WTSCfgLoader::load_from_file(path.c_str());
}

/// 活单：不调 setOrderState，与 lane p11 的 makeAliveOrder 同构
WTSOrderInfo* r3Order(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double traded, const char* oid, const char* utag)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(ct->getCode());
    o->setExchange(ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderFlag(WOF_NOR);
    o->setPrice(7000.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("P11E001");
    o->setUserTag(utag);
    o->setOrderDate(20250110);
    o->setOrderTime(93000);
    o->setStateMsg("ok");
    return o;
}

WTSOrderInfo* r3DeadOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double traded, const char* oid, const char* utag)
{
    WTSOrderInfo* o = r3Order(ct, dir, off, vol, traded, oid, utag);
    o->setOrderState(WOS_Canceled);
    return o;
}

WTSPositionItem* r3Pos(WTSContractInfo* ct, WTSDirectionType dir,
    double prePos, double preAvail, double newPos, double newAvail)
{
    WTSPositionItem* p = WTSPositionItem::create(ct->getCode(), "CNY", ct->getExchg());
    p->setContractInfo(ct);
    p->setDirection(dir);
    p->setPrePosition(prePos);
    p->setAvailPrePos(preAvail);
    p->setNewPosition(newPos);
    p->setAvailNewPos(newAvail);
    p->setInitPosition(prePos + newPos);
    p->setAvgPrice(7000.0);
    return p;
}

template <typename T>
WTSArray* r3Array(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
        arr->append(it);
    return arr;
}

template <typename T>
WTSArray* r3OwnedArray(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
    {
        arr->append(it);
        it->release(); // transfer each create() reference to the array
    }
    return arr;
}

static std::string r3ReadAll(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/**
 * 在 p11 夹具之上补三件事，让 buy()/sell() 可达：
 *   1) 品种挂上带交易时段的 session（否则 L678-680 空指针）
 *   2) WtHelper::setTime 落在时段内（L680 读的是全局时间）
 *   3) 真实的 ActionPolicyMgr（L701 无判空，必须给非 NULL）
 */
class R3TraderTest : public P11TraderTest
{
protected:
    WTSSessionInfo*  sess_ = NULL;
    ActionPolicyMgr  policy_;
    std::string      dir_;

    void SetUp() override
    {
        P11TraderTest::SetUp();
        dir_ = r3TmpDir("policy");
        sess_ = WTSSessionInfo::create("TRADING", "TRADING", 0);
        sess_->addTradingSection(900, 1130);
        comm_->setSessionInfo(sess_);
        WtHelper::setTime(20250110, 931, 0);
    }

    void TearDown() override
    {
        if (sess_)
        {
            sess_->release();
            sess_ = NULL;
        }
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        P11TraderTest::TearDown();
    }

    /// 写入下单/撤单频率限额配置并激活风控（无 module 段 -> init 返回 false，但风控参数已落地）
    void enableRisk(TraderAdapter& trader, uint32_t orderBoundary, uint32_t cancelBoundary)
    {
        std::stringstream ss;
        ss << "{\"ignore_selfmatch\":false,\"savedata\":true,\"riskmon\":{\"active\":true,\"policy\":{"
           << "\"default\":{\"order_times_boundary\":" << orderBoundary
           << ",\"order_stat_timespan\":60,\"order_total_limits\":100"
           << ",\"cancel_times_boundary\":" << cancelBoundary
           << ",\"cancel_stat_timespan\":60,\"cancel_total_limits\":99}}}}";
        WTSVariant* cfg = r3LoadJson(dir_, "risk.json", ss.str());
        ASSERT_NE(nullptr, cfg);
        EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
        cfg->release();
        ASSERT_TRUE(trader.run());
    }

    void loadPolicy(const std::string& body)
    {
        r3LoadJson(dir_, "policy.json", body);
        ASSERT_TRUE(policy_.init((dir_ + "policy.json").c_str()));
    }
};

//////////////////////////////////////////////////////////////////////////
// checkOrderLimits 的频率窗口
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, order_freq_window_denies_submission_after_boundary)
{
    // Align the fake contract's alt code with checkOrderLimits' documented
    // standardized-code key so this case measures the boundary logic alone.
    ct_->setAltCode(kStdCode);
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    enableRisk(trader, 2, 9);
    ASSERT_TRUE(trader.checkOrderLimits(kStdCode));

    // boundary is the maximum successful submissions in the window. Each
    // submission follows the caller contract: check first, then submit.
    for (int i = 0; i < 2; i++) {
        ASSERT_TRUE(trader.checkOrderLimits(kStdCode));
        EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    }
    ASSERT_EQ(2u, api_.n_insert_);
    const bool thirdAllowed = trader.checkOrderLimits(kStdCode);
    EXPECT_FALSE(thirdAllowed);

    // Honor the precheck result as the caller does; a denied third submission
    // must not reach the trader API.
    if (thirdAllowed)
        (void)trader.openLong(kStdCode, 7000.0, 1, 0);
    EXPECT_EQ(2u, api_.n_insert_);
    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
    trader.release();
}

TEST_F(R3TraderTest, order_freq_window_precheck_uses_standard_code_after_raw_submission)
{
    // First observe how many submissions fit before this configured checker
    // refuses a precheck when the submission key and check key are identical.
    // This lets the alias case use the same measured boundary without pinning
    // the current threshold arithmetic as the contract.
    const std::string alignedStdCode = "SHFE.au.2212";
    ct2_->setAltCode(alignedStdCode.c_str());

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    enableRisk(trader, 2, 9);

    uint32_t alignedSubmissions = 0;
    bool alignedDenied = false;
    for (uint32_t attempt = 0; attempt < 4; ++attempt) {
        if (!trader.checkOrderLimits(alignedStdCode.c_str())) {
            alignedDenied = true;
            break;
        }
        const uint32_t localid = trader.openLong(alignedStdCode.c_str(), 7000.0, 1, 0);
        EXPECT_NE(UINT_MAX, localid);
        if (localid == UINT_MAX)
            break;
        ++alignedSubmissions;
        EXPECT_EQ(alignedStdCode, api_.last_code_);
    }
    // If the loop used all four attempts, check once more to observe whether
    // the configured boundary has been reached.
    if (!alignedDenied)
        alignedDenied = !trader.checkOrderLimits(alignedStdCode.c_str());
    EXPECT_TRUE(alignedDenied);
    EXPECT_GT(alignedSubmissions, 0u);
    EXPECT_LE(alignedSubmissions, 4u);

    // The normal caller sequence is precheck, then submit. For this contract,
    // doEntrust changes the submitted code to the raw alt code and caches the
    // timestamp under that raw key. Repeating the sequence for the same number
    // of accepted submissions should exhaust the same window, so the
    // standard-code precheck must deny the next request. The
    // aligned control above proves when this boundary setup is reached; a pass
    // here would isolate the raw/std key mismatch.
    uint32_t rawSubmissions = 0;
    for (; rawSubmissions < alignedSubmissions; ) {
        const bool allowed = trader.checkOrderLimits(kStdCode);
        EXPECT_TRUE(allowed);
        if (!allowed)
            break;
        const uint32_t localid = trader.openLong(kStdCode, 7000.0, 1, 0);
        EXPECT_NE(UINT_MAX, localid);
        if (localid == UINT_MAX)
            break;
        ++rawSubmissions;
        EXPECT_EQ(kRawCode, api_.last_code_);
    }
    EXPECT_EQ(alignedSubmissions, rawSubmissions);

    const bool nextAllowed = trader.checkOrderLimits(kStdCode);
    EXPECT_FALSE(nextAllowed);
    if (nextAllowed)
        (void)trader.openLong(kStdCode, 7000.0, 1, 0);
    EXPECT_EQ(alignedSubmissions + rawSubmissions, api_.n_insert_);

    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// checkCancelLimits 的频率窗口
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, cancel_freq_window_denies_cancel_after_boundary)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    enableRisk(trader, 9, 2);

    // Seed three separately identified live orders so successful cancels are
    // distinct operations. The test temporarily aligns the stored code with
    // the standardized check key to keep raw/std key mapping out of the
    // boundary assertion in this case.
    WTSOrderInfo* order1 = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0, "ORD1", "otp.p11a.1001");
    WTSOrderInfo* order2 = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0, "ORD2", "otp.p11a.1002");
    WTSOrderInfo* order3 = r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0, "ORD3", "otp.p11a.1003");
    WTSArray* orders = r3Array({ order1, order2, order3 });
    trader.onRspOrders(orders);
    orders->release();
    OrderMap* seeded = trader.getOrders("");
    ASSERT_NE(nullptr, seeded);
    EXPECT_EQ(3u, seeded->size());
    seeded->release();
    order1->setCode(kStdCode);
    order2->setCode(kStdCode);
    order3->setCode(kStdCode);
    order1->release();
    order2->release();
    order3->release();

    ASSERT_TRUE(trader.checkCancelLimits(kStdCode));
    EXPECT_TRUE(trader.cancel(1001u));
    ASSERT_TRUE(trader.checkCancelLimits(kStdCode));
    EXPECT_TRUE(trader.cancel(1002u));
    EXPECT_EQ(2u, api_.n_action_);

    EXPECT_FALSE(trader.checkCancelLimits(kStdCode));
    EXPECT_FALSE(trader.cancel(1003u));
    EXPECT_EQ(2u, api_.n_action_);
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// saveData（由 onRspAccount 带动）
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, save_data_writes_long_short_positions_and_fund_block)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    ASSERT_TRUE(trader.run());

    WTSArray* positions = r3OwnedArray({
        r3Pos(ct_, WDT_LONG, 3, 3, 2, 2),
        r3Pos(ct_, WDT_SHORT, 1, 1, 0, 0) });
    trader.onRspPosition(positions);
    positions->release();

    std::string rtfile = std::string(WtHelper::getBaseDir()) + "traders/" + id_ + "/rtdata.json";
    EXPECT_FALSE(std::filesystem::exists(rtfile)) << rtfile;

    // saveData 是 private，由 onRspAccount 的资金分发链带动
    WTSAccountInfo* acct = WTSAccountInfo::create();
    acct->setBalance(123456.0);
    acct->setPreBalance(100000.0);
    WTSArray* accounts = r3OwnedArray({ acct });
    trader.onRspAccount(accounts);
    accounts->release();

    std::string body = r3ReadAll(rtfile);
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    ASSERT_FALSE(doc.HasParseError()) << body;
    ASSERT_TRUE(doc.IsObject());
    ASSERT_TRUE(doc.HasMember("positions") && doc["positions"].IsArray());
    ASSERT_EQ(1u, doc["positions"].Size());
    const auto& position = doc["positions"][0];
    ASSERT_TRUE(position.IsObject());
    ASSERT_TRUE(position.HasMember("code") && position["code"].IsString());
    EXPECT_STREQ(kStdCode, position["code"].GetString());
    ASSERT_TRUE(position.HasMember("long") && position["long"].IsObject());
    ASSERT_TRUE(position["long"].HasMember("newvol") && position["long"]["newvol"].IsNumber());
    ASSERT_TRUE(position["long"].HasMember("prevol") && position["long"]["prevol"].IsNumber());
    EXPECT_DOUBLE_EQ(2.0, position["long"]["newvol"].GetDouble());
    EXPECT_DOUBLE_EQ(3.0, position["long"]["prevol"].GetDouble());
    ASSERT_TRUE(position.HasMember("short") && position["short"].IsObject());
    ASSERT_TRUE(position["short"].HasMember("newvol") && position["short"]["newvol"].IsNumber());
    ASSERT_TRUE(position["short"].HasMember("prevol") && position["short"]["prevol"].IsNumber());
    EXPECT_DOUBLE_EQ(0.0, position["short"]["newvol"].GetDouble());
    EXPECT_DOUBLE_EQ(1.0, position["short"]["prevol"].GetDouble());
    ASSERT_TRUE(doc.HasMember("funds") && doc["funds"].IsObject());
    ASSERT_TRUE(doc["funds"].HasMember("CNY") && doc["funds"]["CNY"].IsObject());
    ASSERT_TRUE(doc["funds"]["CNY"].HasMember("balance") && doc["funds"]["CNY"]["balance"].IsNumber());
    ASSERT_TRUE(doc["funds"]["CNY"].HasMember("prebalance") && doc["funds"]["CNY"]["prebalance"].IsNumber());
    EXPECT_DOUBLE_EQ(123456.0, doc["funds"]["CNY"]["balance"].GetDouble());
    EXPECT_DOUBLE_EQ(100000.0, doc["funds"]["CNY"]["prebalance"].GetDouble());

    // 空资金数组走 size()==0 支路，funds 仍是空对象
    WTSArray* emptyAccounts = WTSArray::create();
    trader.onRspAccount(emptyAccounts);
    emptyAccounts->release();
    std::string body2 = r3ReadAll(rtfile);
    rapidjson::Document emptyFundsDoc;
    emptyFundsDoc.Parse(body2.c_str());
    ASSERT_FALSE(emptyFundsDoc.HasParseError()) << body2;
    ASSERT_TRUE(emptyFundsDoc.HasMember("funds") && emptyFundsDoc["funds"].IsObject());
    EXPECT_EQ(0u, emptyFundsDoc["funds"].MemberCount());
    ASSERT_TRUE(emptyFundsDoc.HasMember("positions") && emptyFundsDoc["positions"].IsArray());
    ASSERT_EQ(1u, emptyFundsDoc["positions"].Size());
    ASSERT_TRUE(emptyFundsDoc["positions"][0].IsObject());
    ASSERT_TRUE(emptyFundsDoc["positions"][0].HasMember("long") && emptyFundsDoc["positions"][0]["long"].IsObject());
    ASSERT_TRUE(emptyFundsDoc["positions"][0]["long"].HasMember("prevol")
        && emptyFundsDoc["positions"][0]["long"]["prevol"].IsNumber());
    EXPECT_DOUBLE_EQ(3.0, emptyFundsDoc["positions"][0]["long"]["prevol"].GetDouble());
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// 四个下单包装：方向 / 开平 / 价格类型 / flag / usertag
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, four_wrappers_stamp_direction_offset_price_and_flag)
{
    TraderAdapter trader;
    buildReady(trader);

    const uint32_t firstLocalId = trader.openLong(kStdCode, 7000.0, 1, 0);
    EXPECT_NE(UINT_MAX, firstLocalId);
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_LIMITPRICE, api_.last_pxtype_);
    EXPECT_EQ((int)WOF_NOR, (int)api_.last_flag_);
    EXPECT_STREQ(kRawCode, api_.last_code_.c_str());
    EXPECT_STREQ(kExchg, api_.last_exchg_.c_str());
    EXPECT_EQ("otp." + id_ + "." + std::to_string(firstLocalId), api_.last_usertag_);
    EXPECT_EQ("P11E001", api_.last_entrust_id_);

    EXPECT_NE(UINT_MAX, trader.openShort(kStdCode, 0.0, 2, 0));
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_ANYPRICE, api_.last_pxtype_);   // 0 价 => 市价

    EXPECT_NE(UINT_MAX, trader.closeLong(kStdCode, 7100.0, 1, true, 1));
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_CLOSETODAY, api_.last_offset_);
    EXPECT_EQ((int)WOF_NOR + 1, (int)api_.last_flag_);

    EXPECT_NE(UINT_MAX, trader.closeShort(kStdCode, 7100.0, 1, false, 0));
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_CLOSE, api_.last_offset_);
    EXPECT_EQ(4u, api_.n_insert_);

    // 提交失败 -> UINT_MAX
    api_.insert_ret_ = -1;
    EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    EXPECT_EQ(5u, api_.n_insert_); // FakeTraderApi counts the failed API request too.
    api_.insert_ret_ = 0;

    // 按方向批量撤单：只挑卖方活单
    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()),
        r3Order(ct_, WDT_SHORT, WOT_OPEN, 2, 0, "ORD2", ("otp." + id_ + ".2002").c_str()),
        r3DeadOrder(ct_, WDT_LONG, WOT_OPEN, 2, 2, "ORD3", ("otp." + id_ + ".2003").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();
    OrderIDs sold = trader.cancel(kStdCode, false);
    EXPECT_EQ(1u, sold.size());                  // 只有那笔卖开
    trader.release();
}

TEST_F(R3TraderTest, cancel_empty_code_cancels_all_buy_orders_without_crashing)
{
    TraderAdapter trader;
    buildReady(trader);

    WTSArray* seededOrders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".2001").c_str()) });
    trader.onRspOrders(seededOrders);
    seededOrders->release();

    // TraderAdapter has an explicit empty-code "all orders" path. Keep the
    // wrapper contract isolated because extractStdCode currently runs first.
    EXPECT_EXIT(
        {
            OrderIDs cancelled = trader.cancel("", true);
            if (cancelled.size() != 1u || api_.n_action_ != 1u)
                std::exit(2);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");

    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// buy()：规则限额 + 单笔最大委托量拆单
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, buy_splits_by_rule_cap_and_single_order_max_volume)
{
    loadPolicy(R"JSON({"default":{"order":[
        {"action":"open","limit":3,"limit_l":60,"limit_s":40,"pure":false}
    ]}})JSON");

    ct_->setVolumeLimits(2, 2);   // 单笔最多 2 手
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, &policy_));
    ASSERT_TRUE(trader.run());

    OrderIDs ids = trader.buy(kStdCode, 7000.0, 5, 0, false);
    // 规则总限额 3 手把 5 手压到 3 手，单笔上限 2 手 => 2 + 1 两笔
    EXPECT_EQ(2u, ids.size());
    EXPECT_EQ(2u, api_.n_insert_);
    EXPECT_DOUBLE_EQ(api_.last_vol_, 1.0);
    EXPECT_EQ(WPT_LIMITPRICE, api_.last_pxtype_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_DOUBLE_EQ(trader.getUndoneQty(kStdCode), 3.0);

    // qty==0 直接返回空集
    EXPECT_EQ(0u, trader.buy(kStdCode, 7000.0, 0, 0, false).size());
    EXPECT_EQ(2u, api_.n_insert_);

    // 非交易时段（15:00）拒绝，一笔都不下发
    WtHelper::setTime(20250110, 1500, 0);
    EXPECT_EQ(0u, trader.buy(kStdCode, 7000.0, 2, 0, false).size());
    EXPECT_EQ(2u, api_.n_insert_);
    WtHelper::setTime(20250110, 931, 0);

    // 市价单按 maxMktVol 拆分：2 手 => 一笔 2 手
    api_.last_vol_ = 0;
    EXPECT_EQ(1u, trader.buy(kStdCode, 0.0, 2, 0, false).size());
    EXPECT_DOUBLE_EQ(api_.last_vol_, 2.0);
    EXPECT_EQ(WPT_ANYPRICE, api_.last_pxtype_);

    // sell() 走空头开仓支路
    EXPECT_EQ(1u, trader.sell(kStdCode, 0.0, 2, 0, false).size());
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_DOUBLE_EQ(api_.last_vol_, 2.0);
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// 规则限额吃满 => 该动作被跳过，一单不发
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, buy_skips_open_rule_when_daily_cap_already_consumed)
{
    loadPolicy(R"JSON({"default":{"order":[
        {"action":"open","limit":2,"limit_l":1,"limit_s":1,"pure":false}
    ]}})JSON");

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, &policy_));
    ASSERT_TRUE(trader.run());

    // statItem.l_openvol 由开仓成交回报累加（L1780），先喂一笔成交把限额吃满
    WTSTradeInfo* t = WTSTradeInfo::create(kRawCode, kExchg);
    t->setContractInfo(ct_);
    t->setDirection(WDT_LONG);
    t->setOffsetType(WOT_OPEN);
    t->setVolume(1);
    t->setPrice(7000.0);
    t->setTradeID("T1");
    t->setRefOrder("ORD9");
    t->setUserTag(("otp." + id_ + ".9001").c_str());
    t->setTradeDate(20250110);
    t->setTradeTime(93010);
    t->setAmount(7000.0);
    WTSArray* trades = r3OwnedArray({ t });
    trader.onRspTrades(trades);
    trades->release();

    size_t before = api_.n_insert_;
    OrderIDs ids = trader.buy(kStdCode, 7000.0, 3, 0, false);
    EXPECT_EQ(0u, ids.size());                 // 多头开仓限额已满 => continue
    EXPECT_EQ(before, api_.n_insert_);         // 没有任何委托下发
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// getPosition / enumPosition / getOrders
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, position_and_order_tables_read_seeded_state)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    ASSERT_TRUE(trader.run());

    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, false), 0.0);
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, true), 0.0);
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdAu, false), 0.0);

    WTSArray* positions = r3OwnedArray({
        r3Pos(ct_, WDT_LONG, 3, 1, 2, 2),
        r3Pos(ct_, WDT_SHORT, 1, 0, 0, 0) });
    trader.onRspPosition(positions);
    positions->release();

    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, false), 4.0);   // long 3+2 less short 1+0
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, true), 3.0);    // 1+2+0+0

    size_t n = 0;
    double longVol = 0, shortVol = 0;
    trader.enumPosition([&n, &longVol, &shortVol](const char* stdCode, bool isLong,
        double prevol, double preavail, double newvol, double newavail) {
        (void)stdCode; (void)preavail; (void)newavail;
        ++n;
        if (isLong)
            longVol += prevol + newvol;
        else
            shortVol += prevol + newvol;
    });
    EXPECT_EQ(2u, n);
    EXPECT_DOUBLE_EQ(longVol, 5.0);
    EXPECT_DOUBLE_EQ(shortVol, 1.0);

    WTSArray* orders = r3OwnedArray({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", ("otp." + id_ + ".1001").c_str()),
        r3DeadOrder(ct_, WDT_LONG, WOT_CLOSE, 3, 3, "ORD2", ("otp." + id_ + ".1002").c_str()) });
    trader.onRspOrders(orders);
    orders->release();
    OrderMap* tab = trader.getOrders(kStdCode);
    ASSERT_NE(nullptr, tab);
    EXPECT_EQ(1u, tab->size());      // 文档要求按标准化代码筛选；保持对当前 raw/std 不匹配的可证伪断言。
    tab->release();
    OrderMap* rawTab = trader.getOrders(kRawCode);
    ASSERT_NE(nullptr, rawTab);
    EXPECT_EQ(1u, rawTab->size());   // 确认活单已入表，定位标准化代码筛选差异。
    rawTab->release();
    OrderMap* allTab = trader.getOrders("");
    EXPECT_NE(nullptr, allTab);
    if (allTab)
        allTab->release();
    trader.release();
}

}  // namespace
