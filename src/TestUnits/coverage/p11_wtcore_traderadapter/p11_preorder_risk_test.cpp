/**
 * p11_preorder_risk_test.cpp — 下单前置改写与风控实体分支（TraderAdapter.cpp:99-207, 487-520, 544-661, 1244-1401）
 *
 * 覆盖 n01 跳过的两块：
 *   A. doEntrust / openLong / openShort / closeLong / closeShort / doCancel / cancel 的实体路径：
 *      委托号生成、合约代码由标准码改写为交易所原始码、市场改写、
 *      usertag 打上 "otp.<id>.<localid>" 标记、市价/限价选择、提交失败(ret<0)返回 UINT_MAX、
 *      撤单前的存活检查与风控检查、撤单后按回报摘除订单。
 *   B. 风控开启后 checkOrderLimits / checkCancelLimits 的实体分支：
 *      使用真实 TraderMocker 插件走完整 init、活跃 default 风控策略的解析，
 *      验证累计撤单封禁进入 _exclude_codes、被排除合约短路返回及品种规则命中。
 *      频率用例保留 boundary=2 的拒绝 oracle；当前实现仍错误放行第三次预检查
 *      （R3-002 产品红控）。
 */
#include "p11_trader_fixture.h"

#include "Share/DLLHelper.hpp"
#include "WtCore/ActionPolicyMgr.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

using namespace p11fix;
using wtp::TraderAdapter;
using wtp::ActionPolicyMgr;

namespace {

namespace fs = std::filesystem;

#ifndef WT_P11_TRADERMOCKER_PATH
#error "WT_P11_TRADERMOCKER_PATH must point to the built TraderMocker plugin"
#endif

const char* kStdAu = "SHFE.au.2212";
const char* kTag1001 = "otp.p11r.1001";
const char* kTag1002 = "otp.p11r.1002";

class ScopedMockerEnvironment
{
public:
    explicit ScopedMockerEnvironment(const fs::path& root)
        : _old_cwd(fs::current_path())
        , _old_inst_dir(WtHelper::getInstDir())
    {
        // Cache the original CWD before switching directories: getCWD() is
        // process-static, so module lookup must fall back to the staged instdir.
        const fs::path original_cwd(WtHelper::getCWD());
        std::error_code cwd_ec;
        const bool same_cwd = fs::equivalent(original_cwd, _old_cwd, cwd_ec);
        if (cwd_ec || !same_cwd)
        {
            _error = "WtHelper CWD cache differs from the process CWD";
            if (cwd_ec)
                _error += ": " + cwd_ec.message();
            return;
        }
        const std::string module_name = DLLHelper::wrap_module("TraderMocker", "lib");
        const fs::path cwd_module = original_cwd / "traders" / module_name;
        if (fs::exists(cwd_module))
        {
            _error = "CWD already contains a TraderMocker plugin: " + cwd_module.string();
            return;
        }

        const fs::path source(WT_P11_TRADERMOCKER_PATH);
        if (!fs::exists(source))
        {
            _error = "TraderMocker plugin missing: " + source.string();
            return;
        }

        std::error_code ec;
        fs::create_directories(root / "traders", ec);
        if (ec)
        {
            _error = "cannot create temporary traders directory: " + ec.message();
            return;
        }
        const fs::path staged_module = root / "traders" / module_name;
        if (!fs::is_regular_file(source))
        {
            _error = "TraderMocker plugin is not a regular file: " + source.string();
            return;
        }
        fs::copy_file(source, staged_module, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            _error = "cannot copy TraderMocker plugin: " + ec.message();
            return;
        }
        if (!fs::is_regular_file(staged_module))
        {
            _error = "staged TraderMocker plugin is not a regular file: " + staged_module.string();
            return;
        }

        fs::current_path(root, ec);
        if (ec)
        {
            _error = "cannot switch to temporary mocker CWD: " + ec.message();
            return;
        }
        _cwd_changed = true;

        const std::string inst_dir = root.string() + "/";
        WtHelper::setInstDir(inst_dir.c_str());
        _inst_dir_changed = true;
        _ok = true;
    }

    ~ScopedMockerEnvironment()
    {
        if (_inst_dir_changed)
            WtHelper::setInstDir(_old_inst_dir.c_str());
        if (_cwd_changed)
        {
            std::error_code ec;
            fs::current_path(_old_cwd, ec);
        }
    }

    bool ok() const { return _ok; }
    const std::string& error() const { return _error; }

private:
    fs::path _old_cwd;
    std::string _old_inst_dir;
    std::string _error;
    bool _cwd_changed = false;
    bool _inst_dir_changed = false;
    bool _ok = false;
};

class RiskMockerSink : public RecordingSink
{
public:
    void on_channel_ready() override
    {
        RecordingSink::on_channel_ready();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _ready = true;
        }
        _changed.notify_all();
    }

    void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty,
        double leftQty, double price, bool isCanceled = false) override
    {
        RecordingSink::on_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_orders;
        }
        _changed.notify_all();
    }

    bool waitReady()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(5), [this] { return _ready; });
    }

    bool waitOrders(uint32_t count)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, std::chrono::seconds(5), [this, count] { return _orders >= count; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _ready = false;
    uint32_t _orders = 0;
};

uint32_t nextMockerId()
{
    static std::atomic<uint32_t> sequence{ 0 };
    return static_cast<uint32_t>(::getpid()) * 100 + sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::string mockerRiskConfig(uint32_t mockerId, const std::string& riskmon)
{
    std::ostringstream json;
    json << "{\"module\":\"TraderMocker\",\"mockerid\":" << mockerId
        << ",\"span\":0,\"udp_port\":0,\"savedata\":false,\"riskmon\":" << riskmon << "}";
    return json.str();
}

void releaseVariant(wtp::WTSVariant* value)
{
    if (value)
        value->release();
}

WTSOrderInfo* makeAliveOrder(WTSContractInfo* ct, WTSDirectionType dir, WTSOffsetType off,
    double vol, double traded, const char* oid, const char* utag, WTSOrderFlag flag = WOF_NOR)
{
    WTSOrderInfo* o = WTSOrderInfo::create();
    o->setCode(ct->getCode());
    o->setExchange(ct->getExchg());
    o->setContractInfo(ct);
    o->setDirection(dir);
    o->setOffsetType(off);
    o->setOrderState(WOS_NotTraded_Queuing);
    o->setOrderFlag(flag);
    o->setPrice(7000.0);
    o->setVolume(vol);
    o->setVolTraded(traded);
    o->setVolLeft(vol - traded);
    o->setOrderID(oid);
    o->setEntrustID("P11E100");
    o->setUserTag(utag);
    o->setOrderDate(20240510);
    o->setOrderTime(93000);
    o->setStateMsg("alive");
    return o;
}

WTSOrderInfo* makeCanceledOrder(WTSContractInfo* ct, double vol, double traded,
    const char* oid, const char* utag)
{
    WTSOrderInfo* o = makeAliveOrder(ct, WDT_LONG, WOT_CLOSE, vol, traded, oid, utag);
    o->setOrderState(WOS_Canceled);
    o->setStateMsg("canceled");
    return o;
}

template <typename T>
WTSArray* makeArray(std::initializer_list<T*> items)
{
    WTSArray* arr = WTSArray::create();
    for (auto* it : items)
        arr->append(it);
    return arr;
}

class P11PreOrderRisk : public P11TraderTest
{
protected:
    void SetUp() override
    {
        P11TraderTest::SetUp();
        id_ = "p11r";                       // 订单标记前缀 otp.p11r.
    }

    /// 写一份只带 riskmon 段的配置（无 module -> init 返回 false，但风控参数已解析）
    wtp::WTSVariant* writeCfg(const char* fname, const std::string& text)
    {
        auto path = tmp_ / fname;
        std::ofstream out(path);
        out << text;
        out.close();
        return WTSCfgLoader::load_from_file(path.string().c_str());
    }
};

TEST_F(P11PreOrderRisk, entrust_fields_are_rewritten_before_sending)
{
    TraderAdapter trader;
    buildReady(trader);

    // 限价开多: 标准代码 SHFE.ag.2512 -> 交易所原始代码 ag2512, 市场 SHFE
    uint32_t localid = trader.openLong(kStdCode, 7000.0, 3, 0);
    EXPECT_NE(UINT_MAX, localid);
    EXPECT_GT(localid, 0u);
    EXPECT_EQ(1u, api_.n_insert_);
    EXPECT_EQ(1u, api_.n_make_eid_);
    EXPECT_STREQ(kRawCode, api_.last_code_.c_str());
    EXPECT_STREQ(kExchg, api_.last_exchg_.c_str());
    EXPECT_STREQ("P11E001", api_.last_entrust_id_.c_str());
    // usertag 里带的序号就是返回给上层的 localid, 回报链靠它找回订单
    EXPECT_STREQ((std::string("otp.p11r.") + std::to_string(localid)).c_str(),
        api_.last_usertag_.c_str());
    EXPECT_DOUBLE_EQ(7000.0, api_.last_price_);
    EXPECT_DOUBLE_EQ(3.0, api_.last_vol_);
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_LIMITPRICE, api_.last_pxtype_);
    EXPECT_EQ(WOF_NOR, api_.last_flag_);

    // 市价开空: 价格为 0 走 WPT_ANYPRICE
    uint32_t lid2 = trader.openShort(kStdCode, 0.0, 2, 0);
    EXPECT_NE(UINT_MAX, lid2);
    EXPECT_NE(localid, lid2);
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_OPEN, api_.last_offset_);
    EXPECT_EQ(WPT_ANYPRICE, api_.last_pxtype_);
    EXPECT_STREQ((std::string("otp.p11r.") + std::to_string(lid2)).c_str(),
        api_.last_usertag_.c_str());
    EXPECT_DOUBLE_EQ(2.0, api_.last_vol_);

    // 平多(平仓) / 平多(平今) / 平空(平今) 三种开平标志
    uint32_t lid3 = trader.closeLong(kStdCode, 7010.0, 1, false, 0);
    EXPECT_NE(UINT_MAX, lid3);
    EXPECT_EQ(WDT_LONG, api_.last_dir_);
    EXPECT_EQ(WOT_CLOSE, api_.last_offset_);
    uint32_t lid4 = trader.closeLong(kStdCode, 7010.0, 1, true, 0);
    EXPECT_NE(UINT_MAX, lid4);
    EXPECT_EQ(WOT_CLOSETODAY, api_.last_offset_);
    uint32_t lid5 = trader.closeShort(kStdCode, 7010.0, 1, true, 0);
    EXPECT_NE(UINT_MAX, lid5);
    EXPECT_EQ(WDT_SHORT, api_.last_dir_);
    EXPECT_EQ(WOT_CLOSETODAY, api_.last_offset_);

    EXPECT_EQ(5u, api_.n_make_eid_);
    EXPECT_EQ(5u, api_.n_insert_);
}

TEST_F(P11PreOrderRisk, submit_failure_returns_max_id)
{
    TraderAdapter trader;
    buildReady(trader);

    // 数量直接透传给通道（拆单上限属于 buy/sell 的 ActionRule 路径, 本 lane 不接真实行情时段）
    uint32_t lid = trader.openLong(kStdCode, 7000.0, 5000.0, 0);
    EXPECT_NE(UINT_MAX, lid);
    EXPECT_DOUBLE_EQ(5000.0, api_.last_vol_);

    // 提交失败: orderInsert 返回负值 -> UINT_MAX
    api_.insert_ret_ = -1;
    uint32_t before = api_.n_make_eid_;
    EXPECT_EQ(UINT_MAX, trader.openShort(kStdCode, 7000.0, 1, 0));
    EXPECT_EQ(before + 1, api_.n_make_eid_);      // 委托号仍然生成
    EXPECT_EQ(2u, api_.n_insert_);

    // 失败不改变已生成的 usertag 语义: 下一次成功下单照样带回新的 localid
    api_.insert_ret_ = 0;
    uint32_t lid2 = trader.closeLong(kStdCode, 7000.0, 1, false, 0);
    EXPECT_NE(UINT_MAX, lid2);
    EXPECT_EQ(3u, api_.n_insert_);
    EXPECT_STREQ((std::string("otp.p11r.") + std::to_string(lid2)).c_str(),
        api_.last_usertag_.c_str());
}

TEST_F(P11PreOrderRisk, cancel_paths)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    // 建表: 1001 买挂单 4/0, 1002 卖挂单 3/1
    trader.onRspOrders(makeArray({
        makeAliveOrder(ct_, WDT_LONG, WOT_OPEN, 4, 0, "CO1", kTag1001),
        makeAliveOrder(ct_, WDT_LONG, WOT_CLOSE, 3, 1, "CO2", kTag1002) }));
    EXPECT_DOUBLE_EQ(2.0, trader.getUndoneQty(kStdCode));   // +4 -2

    // 按 localid 撤单: 撤单用的是交易所原始代码
    EXPECT_TRUE(trader.cancel(1001u));
    EXPECT_EQ(1u, api_.n_action_);
    EXPECT_STREQ(kRawCode, api_.last_action_code_.c_str());

    // 未知 localid
    EXPECT_FALSE(trader.cancel(9999u));
    EXPECT_EQ(1u, api_.n_action_);

    // 按代码撤单: 卖方向只剩 1002
    OrderIDs ids = trader.cancel(kStdCode, false);
    ASSERT_EQ(1u, ids.size());
    EXPECT_EQ(1002u, ids[0]);
    EXPECT_EQ(2u, api_.n_action_);

    // 买方向还有一笔活单(1001 撤单成功不会自动摘除, 要等撤销回报)
    OrderIDs buys = trader.cancel(kStdCode, true);
    ASSERT_EQ(1u, buys.size());
    EXPECT_EQ(1001u, buys[0]);

    // 撤销回报到达后订单被摘掉, 再撤就找不到单子了
    trader.onPushOrder(makeCanceledOrder(ct_, 4, 0, "CO1", kTag1001));
    OrderMap* all = trader.getOrders("");
    ASSERT_NE(nullptr, all);
    EXPECT_EQ(1u, all->size());            // 1002 是平仓单且已被撤, 撤销回报把它也处理了? 见下
    all->release();
    EXPECT_FALSE(trader.cancel(1001u));

    // 通道拒发撤单
    api_.action_ret_ = -1;
    EXPECT_FALSE(trader.cancel(1002u));
    api_.action_ret_ = 0;
}

TEST_F(P11PreOrderRisk, cancel_of_dead_order_is_rejected)
{
    TraderAdapter trader;
    buildReady(trader);
    trader.onLoginResult(true, "ok", 20240510);

    // 全部成交的单不进订单表, 撤它得到 false
    WTSOrderInfo* done = makeAliveOrder(ct_, WDT_LONG, WOT_OPEN, 2, 2, "CO3", kTag1001);
    done->setOrderState(WOS_AllTraded);
    trader.onRspOrders(makeArray({ done }));
    OrderMap* all = trader.getOrders("");
    ASSERT_NE(nullptr, all);
    EXPECT_EQ(0u, all->size());
    all->release();

    EXPECT_FALSE(trader.cancel(1001u));
    EXPECT_EQ(0u, api_.n_action_);

    // 表里没有单时按代码撤单直接返回空集合
    EXPECT_EQ(0u, trader.cancel(kStdCode, false).size());
    EXPECT_EQ(0u, api_.n_action_);
}

TEST_F(P11PreOrderRisk, risk_params_loaded_by_init_and_cancel_limit_blocks)
{
    const std::string riskmon =
        "{\"active\":true,\"policy\":{"
        "\"default\":{\"order_times_boundary\":2,\"order_stat_timespan\":60,"
        "\"order_total_limits\":100,\"cancel_times_boundary\":9,"
        "\"cancel_stat_timespan\":60,\"cancel_total_limits\":2},"
        "\"SHFE.au\":{\"order_times_boundary\":9,\"order_stat_timespan\":60,"
        "\"order_total_limits\":9,\"cancel_times_boundary\":9,"
        "\"cancel_stat_timespan\":60,\"cancel_total_limits\":9}}}";
    std::unique_ptr<wtp::WTSVariant, decltype(&releaseVariant)> cfg(
        writeCfg("risk.json", mockerRiskConfig(nextMockerId(), riskmon)), &releaseVariant);
    ASSERT_NE(nullptr, cfg.get());

    ScopedMockerEnvironment mockerEnv(tmp_);
    ASSERT_TRUE(mockerEnv.ok()) << mockerEnv.error();
    ActionPolicyMgr policy;
    RiskMockerSink sink;
    TraderAdapter trader;
    ASSERT_TRUE(trader.init(id_.c_str(), cfg.get(), &bd_, &policy));
    cfg.reset();
    trader.addSink(&sink);
    ASSERT_TRUE(trader.run());
    ASSERT_TRUE(sink.waitReady()) << "TraderMocker query callbacks did not complete";

    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
    EXPECT_TRUE(trader.checkOrderLimits(kStdCode));

    // 两笔撤销回报 -> total_cancels 达到 cancel_total_limits(2)
    WTSOrderInfo* firstCanceled = makeCanceledOrder(ct_, 1, 0, "RA1", kTag1001);
    trader.onPushOrder(firstCanceled);
    firstCanceled->release();
    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));         // 1 次, 未越界
    WTSOrderInfo* secondCanceled = makeCanceledOrder(ct_, 1, 0, "RA2", kTag1002);
    trader.onPushOrder(secondCanceled);
    secondCanceled->release();

    EXPECT_FALSE(trader.checkCancelLimits(kStdCode));        // 2 次, 封禁并进排除队列
    EXPECT_FALSE(trader.checkCancelLimits(kStdCode));        // 排除队列短路
    EXPECT_FALSE(trader.checkOrderLimits(kStdCode));         // 同一排除队列
    EXPECT_TRUE(trader.checkCancelLimits(kStdAu));           // 另一品种不受影响

    // 被排除的合约连撤单请求都发不出去（doCancel 前置的风控检查直接拒绝）
    WTSOrderInfo* alive = makeAliveOrder(ct_, WDT_LONG, WOT_CLOSE, 1, 0, "RA3", "otp.p11r.1003");
    trader.onPushOrder(alive);
    alive->release();
    OrderMap* tab = trader.getOrders("");
    ASSERT_NE(nullptr, tab);
    EXPECT_EQ(1u, tab->size());            // 活单进表
    tab->release();
    EXPECT_FALSE(trader.cancel(1003u));    // 风控封禁 -> 撤单请求不下发

    trader.release();
}

TEST_F(P11PreOrderRisk, order_frequency_window_rejects_standard_code_after_successful_orders)
{
    const std::string riskmon =
        "{\"active\":true,\"policy\":{"
        "\"default\":{\"order_times_boundary\":2,\"order_stat_timespan\":60,"
        "\"order_total_limits\":100,\"cancel_times_boundary\":9,"
        "\"cancel_stat_timespan\":60,\"cancel_total_limits\":99}}}";
    std::unique_ptr<wtp::WTSVariant, decltype(&releaseVariant)> cfg(
        writeCfg("freq.json", mockerRiskConfig(nextMockerId(), riskmon)), &releaseVariant);
    ASSERT_NE(nullptr, cfg.get());

    ScopedMockerEnvironment mockerEnv(tmp_);
    ASSERT_TRUE(mockerEnv.ok()) << mockerEnv.error();
    ActionPolicyMgr policy;
    RiskMockerSink sink;
    TraderAdapter trader;
    ASSERT_TRUE(trader.init(id_.c_str(), cfg.get(), &bd_, &policy));
    cfg.reset();
    trader.addSink(&sink);
    ASSERT_TRUE(trader.run());
    ASSERT_TRUE(sink.waitReady()) << "TraderMocker query callbacks did not complete";

    // 未下单前放行
    EXPECT_TRUE(trader.checkOrderLimits(kStdCode));

    // 两笔提交均收到真实 TraderMocker 订单回报后，再检查 boundary=2 的第三次预检查。
    for (uint32_t i = 0; i < 2; ++i)
    {
        EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
        ASSERT_TRUE(sink.waitOrders(i + 1)) << "TraderMocker did not acknowledge order " << (i + 1);
    }

    // R3-002 产品红控：当前实现允许 boundary=2 后的第三次预检查通过。
    EXPECT_FALSE(trader.checkOrderLimits(kStdCode));

    trader.release();
}

TEST_F(P11PreOrderRisk, inactive_riskmon_short_circuits_before_stat_map)
{
    wtp::WTSVariant* cfg = writeCfg("inactive.json",
        "{\"savedata\":false,\"riskmon\":{\"active\":false}}");
    ASSERT_NE(nullptr, cfg);

    TraderAdapter trader;
    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
    cfg->release();

    // 风控未激活: 不进 _stat_map 分支, 没有 run() 也不会解引用空表
    EXPECT_TRUE(trader.checkOrderLimits(kStdCode));
    EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
    EXPECT_EQ(0u, api_.n_qaccount_);

    // 没有 riskmon 段: 走「未配置风控」分支, 同样短路
    wtp::WTSVariant* bare = writeCfg("bare.json", "{\"savedata\":false}");
    ASSERT_NE(nullptr, bare);
    TraderAdapter trader2;
    ASSERT_TRUE(trader2.initExt("p11r2", &api_, &bd_, NULL));
    EXPECT_FALSE(trader2.init("p11r2", bare, &bd_, NULL));
    bare->release();
    EXPECT_TRUE(trader2.checkOrderLimits(kStdCode));
    EXPECT_TRUE(trader2.checkCancelLimits(kStdCode));
    trader2.release();
}

TEST_F(P11PreOrderRisk, init_rejects_null_params_and_second_cfg)
{
    TraderAdapter trader;
    EXPECT_FALSE(trader.init(id_.c_str(), NULL, &bd_, NULL));
    EXPECT_EQ(0u, api_.n_init_);

    ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    EXPECT_EQ(1u, api_.n_init_);

    // _cfg 已经存在时 init 直接返回 false
    wtp::WTSVariant* cfg = writeCfg("second.json", "{\"savedata\":false,\"module\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    EXPECT_FALSE(trader.init(id_.c_str(), cfg, &bd_, NULL));
    cfg->release();

    // 已装配 API 时拒绝重复 initExt，且不会再次初始化同一个 API
    EXPECT_FALSE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
    EXPECT_EQ(1u, api_.n_init_);

    // api 为空则直接失败
    TraderAdapter trader2;
    EXPECT_FALSE(trader2.initExt(id_.c_str(), NULL, &bd_, NULL));
}

} // namespace
