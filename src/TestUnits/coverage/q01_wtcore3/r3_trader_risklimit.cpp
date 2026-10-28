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

#include "../p11_wtcore_traderadapter/p11_trader_fixture.h"

#include <WtCore/ActionPolicyMgr.h>
#include <Includes/WTSSessionInfo.hpp>

using namespace p11fix;
using wtp::TraderAdapter;

namespace {

const char* kStdAu = "SHFE.au.2212";
const char* kTag1001 = "otp.r3a.1001";

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
TEST_F(R3TraderTest, order_freq_window_blocks_only_for_the_raw_code_key)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    enableRisk(trader, 2, 9);
    ASSERT_TRUE(trader.checkOrderLimits(kStdCode));

    // 3 笔：窗口内 cnt=3 >= boundary=2，但 times = 3-0-1 = 2 不 > 2 -> 仍放行
    for (int i = 0; i < 3; i++)
        EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    EXPECT_EQ(3u, api_.n_insert_);
    EXPECT_TRUE(trader.checkOrderLimits(kRawCode));

    // 第 4 笔：times = 4-0-1 = 3 > 2 -> 封禁，并进排除队列
    EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    EXPECT_FALSE(trader.checkOrderLimits(kRawCode));
    EXPECT_FALSE(trader.checkOrderLimits(kRawCode));    // 排除队列短路

    // 排除队列里存的是「ag2512」，而 buy/sell/doCancel 传的是标准代码 -> 封禁不生效
    EXPECT_TRUE(trader.checkOrderLimits(kStdCode));
    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// checkCancelLimits 的频率窗口
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, cancel_freq_window_is_keyed_by_order_raw_code)
{
    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    enableRisk(trader, 9, 2);

    // 建一条活单（onRspOrders 按 usertag 里的 localid 建表）
    trader.onRspOrders(r3Array({ r3Order(ct_, WDT_LONG, WOT_OPEN, 1, 0, "ORD1", kTag1001) }));
    ASSERT_NE(nullptr, trader.getOrders(""));

    // 3 次撤单：times = 3-0-1 = 2，不 > boundary=2 -> 请求照常下发
    EXPECT_TRUE(trader.cancel(1001u));
    EXPECT_TRUE(trader.cancel(1001u));
    EXPECT_TRUE(trader.cancel(1001u));
    EXPECT_EQ(3u, api_.n_action_);
    // doCancel 里按标准代码查窗口，缓存却按订单原始代码写 -> 查不到
    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));

    // 第 4 次：times = 3 > 2 -> 按原始代码封禁
    EXPECT_FALSE(trader.checkCancelLimits(kRawCode));
    EXPECT_FALSE(trader.checkCancelLimits(kRawCode));
    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
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

    trader.onRspPosition(r3Array({
        r3Pos(ct_, WDT_LONG, 3, 3, 2, 2),
        r3Pos(ct_, WDT_SHORT, 1, 1, 0, 0) }));

    std::string rtfile = std::string(WtHelper::getBaseDir()) + "traders/" + id_ + "/rtdata.json";
    EXPECT_TRUE(std::filesystem::exists(rtfile)) << rtfile;

    // saveData 是 private，由 onRspAccount 的资金分发链带动
    WTSAccountInfo* acct = WTSAccountInfo::create();
    acct->setBalance(123456.0);
    acct->setPreBalance(100000.0);
    trader.onRspAccount(r3Array({ acct }));

    std::string body = r3ReadAll(rtfile);
    EXPECT_NE(body.find("\"positions\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"long\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"short\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"newvol\" : 2"), std::string::npos) << body;
    EXPECT_NE(body.find("\"prevol\" : 3"), std::string::npos) << body;
    EXPECT_NE(body.find("\"funds\""), std::string::npos) << body;
    EXPECT_NE(body.find("\"balance\" : 123456"), std::string::npos) << body;
    EXPECT_NE(body.find("\"prebalance\" : 100000"), std::string::npos) << body;

    // 空资金数组走 size()==0 支路，funds 仍是空对象
    trader.onRspAccount(WTSArray::create());
    std::string body2 = r3ReadAll(rtfile);
    EXPECT_NE(body2.find("\"funds\" : { }"), std::string::npos) << body2;
    EXPECT_NE(body2.find("\"prevol\" : 3"), std::string::npos) << body2;   // 持仓段仍在
    trader.release();
}

//////////////////////////////////////////////////////////////////////////
// 四个下单包装：方向 / 开平 / 价格类型 / flag / usertag
//////////////////////////////////////////////////////////////////////////
TEST_F(R3TraderTest, four_wrappers_stamp_direction_offset_price_and_flag)
{
    TraderAdapter trader;
    buildReady(trader);

    EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_LIMITPRICE, api_.last_pxtype_);
    EXPECT_EQ((int)WOF_NOR, (int)api_.last_flag_);
    EXPECT_STREQ(kRawCode, api_.last_code_.c_str());
    EXPECT_STREQ(kExchg, api_.last_exchg_.c_str());
    EXPECT_EQ(0u, api_.last_usertag_.find("otp.")) << api_.last_usertag_;
    EXPECT_NE(api_.last_usertag_.find(".1001"), std::string::npos) << api_.last_usertag_;

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
    EXPECT_EQ(4u, api_.n_insert_);
    api_.insert_ret_ = 0;

    // 按方向批量撤单：只挑卖方活单
    trader.onRspOrders(r3Array({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", "otp.r3a.2001"),
        r3Order(ct_, WDT_SHORT, WOT_OPEN, 2, 0, "ORD2", "otp.r3a.2002"),
        r3DeadOrder(ct_, WDT_LONG, WOT_OPEN, 2, 2, "ORD3", "otp.r3a.2003") }));
    OrderIDs sold = trader.cancel(kStdCode, false);
    EXPECT_EQ(1u, sold.size());                  // 只有那笔卖开
    OrderIDs all = trader.cancel("", true);
    EXPECT_GE(all.size(), 1u);                   // 空代码 => 全部买向活单
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
    t->setUserTag("otp.r3a.9001");
    t->setTradeDate(20250110);
    t->setTradeTime(93010);
    t->setAmount(7000.0);
    trader.onRspTrades(r3Array({ t }));

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

    trader.onRspPosition(r3Array({
        r3Pos(ct_, WDT_LONG, 3, 1, 2, 2),
        r3Pos(ct_, WDT_SHORT, 1, 0, 0, 0) }));

    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, false), 6.0);   // 3+2+1+0
    EXPECT_DOUBLE_EQ(trader.getPosition(kStdCode, true), 3.0);    // 1+2+0+0

    size_t n = 0;
    double longVol = 0, shortVol = 0;
    trader.enumPosition([&n, &longVol, &shortVol](const char* stdCode, bool isLong,
        double prevol, double preavail, double newvol, double newavail) {
        (void)stdCode; (void)preavail; (void)newavail;
        ++n;
        longVol += prevol + newvol;
        shortVol += isLong ? 0.0 : (prevol + newvol);
    });
    EXPECT_EQ(1u, n);
    EXPECT_DOUBLE_EQ(longVol, 5.0);
    EXPECT_DOUBLE_EQ(shortVol, 1.0);

    trader.onRspOrders(r3Array({
        r3Order(ct_, WDT_LONG, WOT_OPEN, 2, 0, "ORD1", kTag1001),
        r3DeadOrder(ct_, WDT_LONG, WOT_CLOSE, 3, 3, "ORD2", "otp.r3a.1002") }));
    OrderMap* tab = trader.getOrders(kStdCode);
    ASSERT_NE(nullptr, tab);
    EXPECT_EQ(1u, tab->size());      // 死单不进表
    tab->release();
    EXPECT_NE(nullptr, trader.getOrders(""));
    trader.release();
}

}  // namespace
