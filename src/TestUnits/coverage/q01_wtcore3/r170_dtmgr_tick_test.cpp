/**
 * r170_dtmgr_tick_test.cpp — lane R170 / wave25
 *
 * 目标：src/WtCore/WtDtMgr.cpp handle_push_quote 的后复权缓存分支里
 * R160 未走到的两条早退：
 *   - _ticks_adjusted 已建立但该合约没有缓存 -> :231-233
 *   - 缓存存在、isValidOnly() 且推送 tick 的 volume==0 -> :235-236
 * 再补一条 volume>0 的 append（:238）作为对照，证明前面确实早退。
 *
 * _ticks_adjusted 是 private，沿用 r160 的 `#define private public` 手法。
 */
#include <gtest/gtest.h>

#include <cstring>

#include <Includes/FasterDefs.h>
#include <Includes/IDataManager.h>
#include <Includes/IDataReader.h>
#include <Includes/WTSCollection.hpp>
#include <Includes/WTSDataDef.hpp>

#define private public
#include <WtCore/WtDtMgr.h>
#undef private

using namespace wtp;

namespace {

WTSTickData* makeTick(const char* code, double volume)
{
    WTSTickStruct st;
    memset(&st, 0, sizeof(st));
    wt_strcpy(st.exchg, "SHFE");
    wt_strcpy(st.code, code);
    st.action_date = 20250110;
    st.trading_date = 20250110;
    st.action_time = 93000000;
    st.price = 3500.0;
    st.volume = volume;
    return WTSTickData::create(st);
}

}  // namespace

TEST(R170DtMgrTick, AdjustedCacheSkipsUnknownAndZeroVolumeTicks)
{
    WtDtMgr mgr;
    mgr._ticks_adjusted = WtDtMgr::DataCacheMap::create();
    ASSERT_NE(nullptr, mgr._ticks_adjusted);

    // 1) 复权缓存已建立，但没有该合约 -> 直接 return，不建条目
    WTSTickData* t0 = makeTick("SHFE.ag.2512", 5);
    ASSERT_NE(nullptr, t0);
    mgr.handle_push_quote("SHFE.ag.2512", t0);
    t0->release();
    EXPECT_EQ(0u, mgr._ticks_adjusted->size());

    // 2) 缓存标记 validOnly，零成交量推送必须被过滤
    WTSHisTickData* his = WTSHisTickData::create("SHFE.ag.2512", true, 1.0);
    ASSERT_NE(nullptr, his);
    mgr._ticks_adjusted->add("SHFE.ag.2512", his, false);

    WTSTickData* tz = makeTick("SHFE.ag.2512", 0);
    ASSERT_NE(nullptr, tz);
    mgr.handle_push_quote("SHFE.ag.2512", tz);
    tz->release();
    EXPECT_EQ(0u, his->getDataRef().size());

    // 3) 有成交量的 tick 正常 append
    WTSTickData* tv = makeTick("SHFE.ag.2512", 3);
    ASSERT_NE(nullptr, tv);
    mgr.handle_push_quote("SHFE.ag.2512", tv);
    tv->release();
    EXPECT_EQ(1u, his->getDataRef().size());
    EXPECT_DOUBLE_EQ(3500.0, his->getDataRef().front().price);
}
