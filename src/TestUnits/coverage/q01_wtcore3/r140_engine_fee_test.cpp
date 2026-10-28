/**
 * r140_engine_fee_test.cpp — lane R140 / wave22
 *
 * 目标：src/WtCore/WtEngine.cpp 的手续费算术面 :879-916。
 * 基线里 `calc_fee` 的 890-916 全未覆盖：R130 的同名用例挂在 P01CtaEngine 上，
 * 运行日志停在 [ RUN ]（进程未干净退出，profraw 未贡献计数），因此本 lane 用
 * r73 的确定性引擎替身重做。WtCtaEngine::_cfg 只能由 init 赋值而析构无条件
 * release，故每条用例先 init 一次最小配置；不 run/不起线程。
 *
 * 覆盖行段：
 *   :879-888（无模板早退，反向确认）
 *   :890-902（按手数 offset 0/1/2/default）
 *   :903-913（按金额 offset 0/1/2/default，amount = price*qty*volScale）
 *   :915（(int32_t)(ret*100+0.5)/100 两位小数四舍五入）
 *
 * 只通过测试子类向 protected `_fee_map` 注入模板；未用 #define private public、
 * 未镜像类布局、未改生产宏。临时目录只落在系统临时目录并在析构清理。
 */
#include <gtest/gtest.h>

#include <string>

#include "r130_env.h"
#include "../n02_wtcore_engines/r73_fixture.h"

#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/WtEngine.h>

USING_NS_WTP;

namespace {

const char* const kAg = "SHFE.ag.2512";

/** 暴露 protected 手续费模板表的引擎替身（r73::CtaEngine 已有 _port_fund 生命周期） */
class R140FeeEngine : public r73::CtaEngine
{
public:
    void seedFee(const char* fullPid, double open, double close,
                 double closeToday, bool byVolume)
    {
        FeeItem item;
        item._open = open;
        item._close = close;
        item._close_today = closeToday;
        item._by_volume = byVolume;
        _fee_map[fullPid] = item;
    }

    std::size_t feeCount() const { return _fee_map.size(); }
};

/**
 * r73::CtaEngine 的 attach 只赋 _base_data_mgr/_hot_mgr，而 WtCtaEngine::_cfg
 * 仅由 init 赋值，~WtCtaEngine 却会无条件 `_cfg->release()`。不 init 直接析构
 * 等于释放未初始化的指针，用例在 [ RUN ] 后即 SIGSEGV（本波两条 crash 的根因）。
 * 这里补一次最小 init（fees/filters 为空），临时目录由 r130::TempEnv 提供。
 */
void initEngine(R140FeeEngine& engine, r73::BaseDataMgr& bd, r73::HotMgr& hot)
{
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"product\":{\"session\":\"TRADING\"},\"fees\":\"\",\"filters\":\"\"}");
    ASSERT_NE(nullptr, cfg);
    engine.init(cfg, &bd, NULL, &hot, NULL);
    cfg->release();
    // init 不设置引擎时钟，保留 attach 以维持原有固定日期
    engine.attach(&bd, &hot, NULL);
}

void prepareEnv(r73::BaseDataMgr& bd)
{
    WTSSessionInfo* sess = bd.regisSession("TRADING", 0, 2359);
    ASSERT_NE(nullptr, sess);
    WTSCommodityInfo* ag = r73::makeComm("SHFE", "ag", "TRADING", TM_Both);
    ag->setSessionInfo(sess);
    bd.regis(ag, r73::makeCt("SHFE", "ag", "ag2512"));
}

}  // namespace

//////////////////////////////////////////////////////////////////
// 按手数计费 + offset 三分支 + 四舍五入
// src/WtCore/WtEngine.cpp:890-902, 915
//////////////////////////////////////////////////////////////////
TEST(R140EngineFee, ByVolumeOffsetsAndRounding)
{
    r130::TempEnv env("r140fee");
    r73::BaseDataMgr bd;
    r73::HotMgr hot;
    prepareEnv(bd);
    ASSERT_NE(nullptr, bd.getCommodity("SHFE", "ag"));

    R140FeeEngine engine;
    initEngine(engine, bd, hot);
    engine.seedFee("SHFE.ag", 3.0, 2.0, 1.0, true);
    ASSERT_EQ(1u, engine.feeCount());

    EXPECT_DOUBLE_EQ(6.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(4.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(2.0, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 9));

    // 0.333 手 -> (int32_t)(33.3 + 0.5) / 100 = 0.33
    engine.seedFee("SHFE.ag", 0.333, 0.0, 0.0, true);
    EXPECT_DOUBLE_EQ(0.33, engine.calc_fee(kAg, 3500.0, 1.0, 0));

    // 模板键是「交易所.品种」，不含月份；另一品种必须早退
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee("SHFE.au.2512", 3500.0, 2.0, 0));
}

//////////////////////////////////////////////////////////////////
// 按金额计费：amount = price * qty * volScale(10)
// src/WtCore/WtEngine.cpp:903-913, 915
//////////////////////////////////////////////////////////////////
TEST(R140EngineFee, ByAmountOffsetsUseVolumeScale)
{
    r130::TempEnv env("r140fee");
    r73::BaseDataMgr bd;
    r73::HotMgr hot;
    prepareEnv(bd);
    ASSERT_NE(nullptr, bd.getCommodity("SHFE", "ag"));

    R140FeeEngine engine;
    initEngine(engine, bd, hot);
    engine.seedFee("SHFE.ag", 0.0002, 0.0001, 0.00005, false);

    // 3500 * 2 * 10 = 70000
    EXPECT_DOUBLE_EQ(14.0, engine.calc_fee(kAg, 3500.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(7.0, engine.calc_fee(kAg, 3500.0, 2.0, 1));
    EXPECT_DOUBLE_EQ(3.5, engine.calc_fee(kAg, 3500.0, 2.0, 2));
    EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kAg, 3500.0, 2.0, 3));
}
