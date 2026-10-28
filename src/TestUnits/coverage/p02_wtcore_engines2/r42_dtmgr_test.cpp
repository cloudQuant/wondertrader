/**
 * r42_dtmgr_test.cpp  (lane R42 / WtCore 数据管理器补测)
 *
 * 覆盖目标 src/WtCore/WtDtMgr.cpp：
 *   initStore 的加载失败分支（store 缺失 / 模块缺失）
 *   init 的配置读取与返回
 *   get_basedata_mgr / get_hot_mgr / get_date / get_min_time / get_secs / reader_log
 *   handle_push_quote / grab_last_tick 的缓存路径
 *   get_*_slice / get_kline_slice / get_adjusting_* 在 reader 缺失时的守卫
 *   on_bar 的周期映射与 on_all_bar_updated 的空通知队列
 *
 * 引擎使用 R42HftEngine（不 init，不加载数据存储插件），因此 init 必然止于
 * initStore 失败，这正是本轮要补齐的分支。
 */
#include <gtest/gtest.h>

#include <string>

#include "r42_core_fakes.h"

#include <WtCore/WtDtMgr.h>
#include <WTSUtils/WTSCfgLoader.h>
#include <Includes/WTSVariant.hpp>

using namespace wtp;

namespace {

class R42DtMgrTest : public ::testing::Test
{
protected:
    r42fakes::R42GenDir      gen;
    r42fakes::R42BaseDataMgr bd;
    r42fakes::R42HotMgr      hot;
    r42fakes::R42HftEngine   engine;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r42fakes::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        bd.regis(rb, r42fakes::makeCt("SHFE", "rb", "2510"));

        engine.attach(&bd, &hot);
    }
};

// 覆盖 WtDtMgr.cpp L50-102 initStore/init 的全部失败出口
TEST_F(R42DtMgrTest, InitReportsStoreFailures)
{
    WtDtMgr mgr;

    // 完全没有 store 段
    WTSVariant* cfg = WTSCfgLoader::load_from_content(R"json({"align_by_section": true})json");
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(mgr.init(cfg, &engine, true));
    cfg->release();

    // store 段是空对象时，initStore 会按 WtDtMgr.cpp:55-59 回退到默认的 WtDataStorage，
    // 并不是失败输入（真正的 NULL 入口只来自缺少 store 键，见上一段）。
    // 这里改用不存在的模块名走 load_library 失败出口。
    cfg = WTSCfgLoader::load_from_content(R"json({"store": {"module": "r42_missing_store_module"}})json");
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(mgr.init(cfg, &engine));
    cfg->release();

    // 指定一个不存在的存储模块 -> load_library 失败
    cfg = WTSCfgLoader::load_from_content(
        R"json({"store": {"module": "r42_no_such_data_storage"}, "align_by_section": false})json");
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(mgr.init(cfg, &engine, false));
    cfg->release();

    // 失败后 reader 仍为空
    EXPECT_TRUE(mgr.reader() == NULL);
    EXPECT_TRUE(mgr.loader() == NULL);
}

// 覆盖 L119-147 引擎转发、L242-275 缓存/复权守卫、L277-400 切片守卫
TEST_F(R42DtMgrTest, ForwardersAndReaderGuards)
{
    WtDtMgr mgr;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(R"json({})json");
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(mgr.init(cfg, &engine));
    cfg->release();

    EXPECT_EQ(mgr.get_basedata_mgr(), &bd);
    EXPECT_EQ(mgr.get_hot_mgr(), &hot);
    EXPECT_EQ(mgr.get_date(), 20250110u);
    EXPECT_EQ(mgr.get_min_time(), 931u);
    EXPECT_EQ(mgr.get_secs(), 0u);

    mgr.reader_log(LL_INFO, "r42 reader log");
    mgr.reader_log(LL_ERROR, "r42 reader error");

    EXPECT_DOUBLE_EQ(mgr.get_adjusting_factor("SHFE.rb.2510", 20250110), 1.0);
    EXPECT_EQ(mgr.get_adjusting_flag(), 0u);

    EXPECT_TRUE(mgr.get_tick_slice("SHFE.rb.2510", 10) == NULL);
    EXPECT_TRUE(mgr.get_order_queue_slice("SHFE.rb.2510", 10) == NULL);
    EXPECT_TRUE(mgr.get_order_detail_slice("SHFE.rb.2510", 10) == NULL);
    EXPECT_TRUE(mgr.get_transaction_slice("SHFE.rb.2510", 10) == NULL);
    EXPECT_TRUE(mgr.get_kline_slice("SHFE.rb.2510", KP_Minute1, 1, 10) == NULL);
    EXPECT_TRUE(mgr.grab_last_tick("SHFE.rb.2510") == NULL);
}

// 覆盖 L219-253 handle_push_quote/grab_last_tick 的缓存读写
TEST_F(R42DtMgrTest, PushQuoteUpdatesRealtimeCache)
{
    WtDtMgr mgr;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(R"json({})json");
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(mgr.init(cfg, &engine));
    cfg->release();

    // 空指针被忽略
    mgr.handle_push_quote("SHFE.rb.2510", NULL);
    EXPECT_TRUE(mgr.grab_last_tick("SHFE.rb.2510") == NULL);

    WTSTickData* tick = r42fakes::makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3500.0);
    ASSERT_NE(tick, nullptr);
    mgr.handle_push_quote("SHFE.rb.2510", tick);
    tick->release();

    WTSTickData* got = mgr.grab_last_tick("SHFE.rb.2510");
    ASSERT_NE(got, nullptr);
    EXPECT_DOUBLE_EQ(got->price(), 3500.0);
    got->release();

    WTSTickData* got2 = mgr.grab_last_tick("SHFE.rb.2510");
    ASSERT_NE(got2, nullptr);
    EXPECT_EQ(got2->tradingdate(), 20250110u);
    got2->release();

    EXPECT_TRUE(mgr.grab_last_tick("SHFE.ag.2512") == NULL);
}

// 覆盖 L160-217 on_bar 的周期映射与 L104-117 on_all_bar_updated 空队列
TEST_F(R42DtMgrTest, BarCallbacksWithoutSubscriptionAreNoop)
{
    WtDtMgr mgr;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(R"json({})json");
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(mgr.init(cfg, &engine));
    cfg->release();

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3500.0;

    // 未订阅 + 无缓存：三种周期映射都只走前置判断
    mgr.on_bar("SHFE.rb.2510", KP_Minute1, &bar);
    mgr.on_bar("SHFE.rb.2510", KP_Minute5, &bar);
    mgr.on_bar("SHFE.rb.2510", KP_DAY, &bar);
    mgr.on_bar("SHFE.rb.2510", KP_Hour, &bar);

    // 通知队列为空时直接返回
    mgr.on_all_bar_updated(0);
    mgr.on_all_bar_updated(931);
}

} // namespace
