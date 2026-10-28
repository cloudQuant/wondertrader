/**
 * r10_wtcore_dtmgr.cpp —— lane R10 / src/WtCore/WtDtMgr.cpp 可达面
 *
 * 本文件只打「不依赖 WtDataStorage 插件（_reader 恒为 NULL）」的分支：
 *   L26-48   构造/析构（含缓存 release）
 *   L89-102  init()：_engine/_force_cache/_align_by_section 赋值后，
 *            initStore(cfg->get("store")==NULL) 在 L52-53 早退 ⇒ init 返回 false，
 *            但 _engine 已经生效，故 L119-142 的 5 个委托 getter 可达
 *   L119-142 get_basedata_mgr/get_hot_mgr/get_date/get_min_time/get_secs
 *   L144-147 reader_log
 *   L149-181 on_bar：未订阅基础周期 + _bars_cache==NULL ⇒ L181 早退
 *   L104-107 on_all_bar_updated：_bar_notifies 为空 ⇒ 早退
 *   L219-240 handle_push_quote（NULL 早退 / 建 _rt_tick_map / _ticks_adjusted==NULL 跳过）
 *   L242-253 grab_last_tick（缓存未建 / 命中 retain / 未命中）
 *   L255-275 get_adjusting_factor / get_adjusting_flag（_reader==NULL 两支）
 *   L277-400 五个 slice 查询的 _reader==NULL 早退
 *
 * 需要接缝（登记到 bugs/R10_wtcore.md，本文件不硬闯）：
 *   L50-87 initStore 的 DLL 装载、L109-117 on_all_bar_updated 派发、L183-217 on_bar 重采样、
 *   L287-375 get_tick_slice 后复权缓存、L421-542 get_kline_slice 重采样缓存
 *   —— 全部依赖 _reader（只有 DLLHelper::load_library("WtDataStorage") 才会非空），
 *   WtDtMgr 无 reader 注入口（_reader 私有、无 setter、无虚函数）。
 *
 * 替身：q01_wtcore3/q01_sel_env.h（BaseDataMgr/HotMgr/SelEngine/Q01TempEnv），
 *       SelEngine 派生自 WtSelEngine（ concrete，不 init/run/起线程）。
 */
#include "q01_sel_env.h"

#include <Includes/WTSVariant.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <WtCore/WtDtMgr.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

/** WtDtMgr 需要 WtEngine*，用 q01 的裸选股引擎（已接上替身，不起线程） */
typedef SelEngine R10Engine;

class R10DtMgrTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r10dtmgr"));
        _bdm.regisSession("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        // attach: set_date_time(20250110, 931, 0) + set_trading_date(20250110)
        _eng.attach(&_bdm, &_hot);
    }

    void TearDown() override
    {
        _mgr.reset();
        _env.reset();
    }

    /** init() 用的配置：不带 store ⇒ initStore 早退 ⇒ _reader 恒 NULL */
    WTSVariant* makeNoStoreCfg()
    {
        WTSVariant* cfg = WTSVariant::createObject();
        cfg->append("align_by_section", false);
        return cfg;
    }

    WTSTickData* makeAgTick(uint32_t mins, double price)
    {
        return makeTick("SHFE", "ag2512", 20250110, 20250110, mins, price);
    }

    std::unique_ptr<Q01TempEnv> _env;
    std::unique_ptr<WtDtMgr> _mgr;
    BaseDataMgr _bdm;
    HotMgr _hot;
    R10Engine _eng;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
};

} // namespace

TEST_F(R10DtMgrTest, fresh_manager_has_no_reader_and_queries_return_null)
{
    _mgr.reset(new WtDtMgr());
    EXPECT_EQ(_mgr->reader(), nullptr);
    EXPECT_EQ(_mgr->loader(), nullptr);

    // L277-400：五个切片查询在 _reader==NULL 时全部早退
    EXPECT_EQ(_mgr->get_tick_slice("SHFE.ag.2512", 10), nullptr);
    EXPECT_EQ(_mgr->get_order_queue_slice("SHFE.ag.2512", 10), nullptr);
    EXPECT_EQ(_mgr->get_order_detail_slice("SHFE.ag.2512", 10), nullptr);
    EXPECT_EQ(_mgr->get_transaction_slice("SHFE.ag.2512", 10), nullptr);
    EXPECT_EQ(_mgr->get_kline_slice("SHFE.ag.2512", KP_Minute1, 1, 10), nullptr);

    // L242-245：实时缓存还没建
    EXPECT_EQ(_mgr->grab_last_tick("SHFE.ag.2512"), nullptr);
}

TEST_F(R10DtMgrTest, init_without_store_sets_engine_and_returns_false)
{
    _mgr.reset(new WtDtMgr());
    WTSVariant* cfg = makeNoStoreCfg();

    // init 先置 _engine，再 initStore(NULL) ⇒ false（L91-101 + L52-53）
    EXPECT_FALSE(_mgr->init(cfg, &_eng, true));
    EXPECT_EQ(_mgr->reader(), nullptr);   // 插件没装载，reader 仍为空

    // 委托 getter（L119-142）此时已可用
    EXPECT_EQ(_mgr->get_date(), 20250110u);
    EXPECT_EQ(_mgr->get_min_time(), 931u);
    EXPECT_EQ(_mgr->get_secs(), 0u);
    EXPECT_EQ(_mgr->get_basedata_mgr(), &_bdm);
    EXPECT_EQ(_mgr->get_hot_mgr(), &_hot);
}

TEST_F(R10DtMgrTest, init_forwards_loader_registered_before_init)
{
    _mgr.reset(new WtDtMgr());
    // regsiter_loader 是唯一的公开注入口（WtDtMgr.h L78）
    _mgr->regsiter_loader(nullptr);
    EXPECT_EQ(_mgr->loader(), nullptr);

    WTSVariant* cfg = makeNoStoreCfg();
    EXPECT_FALSE(_mgr->init(cfg, &_eng, false));
    EXPECT_EQ(_mgr->loader(), nullptr);
    EXPECT_EQ(_mgr->get_date(), 20250110u);
}

TEST_F(R10DtMgrTest, adjusting_factor_and_flag_fall_back_without_reader)
{
    _mgr.reset(new WtDtMgr());
    // L255-261：_reader==NULL ⇒ 因子 1.0
    EXPECT_DOUBLE_EQ(_mgr->get_adjusting_factor("SHFE.ag.2512", 20250110), 1.0);
    // L263-275：_reader==NULL ⇒ flag 0（函数内 static 记忆化，见缺陷 BUG-R10-003）
    EXPECT_EQ(_mgr->get_adjusting_flag(), 0u);
    // 第二次调用命中 static 缓存分支
    EXPECT_EQ(_mgr->get_adjusting_flag(), 0u);
}

TEST_F(R10DtMgrTest, handle_push_quote_null_tick_is_ignored)
{
    _mgr.reset(new WtDtMgr());
    // L221-222 早退：不得建缓存
    _mgr->handle_push_quote("SHFE.ag.2512", NULL);
    EXPECT_EQ(_mgr->grab_last_tick("SHFE.ag.2512"), nullptr);
}

TEST_F(R10DtMgrTest, handle_push_quote_caches_and_grab_retains)
{
    _mgr.reset(new WtDtMgr());
    WTSTickData* tk = makeAgTick(93100, 7123.5);
    ASSERT_NE(tk, nullptr);

    _mgr->handle_push_quote("SHFE.ag.2512", tk);   // L224-227 建 _rt_tick_map

    WTSTickData* got = _mgr->grab_last_tick("SHFE.ag.2512");
    ASSERT_NE(got, nullptr);
    EXPECT_DOUBLE_EQ(got->getTickStruct().price, 7123.5);
    EXPECT_EQ(strcmp(got->getTickStruct().code, "ag2512"), 0);
    got->release();

    // 未推送过的合约仍然取不到（L248-249）
    EXPECT_EQ(_mgr->grab_last_tick("SHFE.au.2212"), nullptr);

    // 覆盖 _ticks_adjusted==NULL 的跳过分支：同一合约再推一笔
    _mgr->handle_push_quote("SHFE.ag.2512", makeAgTick(93101, 7124.0));
    WTSTickData* got2 = _mgr->grab_last_tick("SHFE.ag.2512");
    ASSERT_NE(got2, nullptr);
    EXPECT_DOUBLE_EQ(got2->getTickStruct().price, 7124.0);
    got2->release();
}

TEST_F(R10DtMgrTest, destructor_releases_rt_tick_cache)
{
    _mgr.reset(new WtDtMgr());
    _mgr->handle_push_quote("SHFE.ag.2512", makeAgTick(93102, 7125.0));
    // L38-48：_rt_tick_map 非空 ⇒ 析构走 release 分支
    _mgr.reset();
    EXPECT_EQ(_mgr, nullptr);
    _mgr.reset(new WtDtMgr());    // 保证 TearDown 不再解引用空指针
}

TEST_F(R10DtMgrTest, on_bar_without_subscription_and_without_cache_returns_early)
{
    _mgr.reset(new WtDtMgr());
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.time = 202501100931;
    bar.open = 7100.0;
    bar.high = 7110.0;   // 两根 bar 用同一组价量，断言只依赖喂进去的输入
    bar.low = 7090.0;
    bar.close = 7105.0;
    bar.vol = 120;

    // _subed_basic_bars 为空 ⇒ 不进 _bar_notifies；_bars_cache==NULL ⇒ L181 早退
    _mgr->on_bar("SHFE.ag.2512", KP_Minute1, &bar);
    // 空队列 ⇒ on_all_bar_updated 在 L107 早退，绝不解引用 _engine
    _mgr->on_all_bar_updated(931);
    EXPECT_EQ(_mgr->grab_last_tick("SHFE.ag.2512"), nullptr);
}

TEST_F(R10DtMgrTest, on_bar_other_periods_also_return_early)
{
    _mgr.reset(new WtDtMgr());
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.time = 202501100930;
    bar.close = 100.0;
    bar.vol = 5;

    // KP_Minute5 ⇒ speriod='m', times=5（L161-164）；KP_DAY ⇒ 'd'/1（L165-168）
    _mgr->on_bar("SHFE.ag.2512", KP_Minute5, &bar);
    _mgr->on_bar("SHFE.ag.2512", KP_DAY, &bar);
    _mgr->on_all_bar_updated(0);
    SUCCEED();
}

TEST_F(R10DtMgrTest, reader_log_passes_every_level)
{
    _mgr.reset(new WtDtMgr());
    // L144-147：纯转发，不 deref 任何成员
    _mgr->reader_log(LL_DEBUG, "debug line");
    _mgr->reader_log(LL_INFO, "info line");
    _mgr->reader_log(LL_WARN, "warn line");
    _mgr->reader_log(LL_ERROR, "error line");
    EXPECT_EQ(_mgr->reader(), nullptr);
}

TEST_F(R10DtMgrTest, kline_slice_null_reader_for_every_period)
{
    _mgr.reset(new WtDtMgr());
    // L421-424：times!=1 也要先过 _reader==NULL，绝不落到 _engine->get_session_info
    EXPECT_EQ(_mgr->get_kline_slice("SHFE.ag.2512", KP_Minute1, 3, 10), nullptr);
    EXPECT_EQ(_mgr->get_kline_slice("SHFE.ag.2512", KP_Minute5, 2, 10), nullptr);
    EXPECT_EQ(_mgr->get_kline_slice("SHFE.ag.2512", KP_DAY, 1, 10), nullptr);
    // 空 tick 切片同理（L279-280）
    EXPECT_EQ(_mgr->get_tick_slice("SHFE.ag.2512+", 1, 20250110093100000ull), nullptr);
}
