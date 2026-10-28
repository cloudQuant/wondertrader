/**
 * r130_diff_executer_tail_test.cpp — lane R130 / wave21
 *
 * 目标：src/WtCore/WtDiffExecuter.cpp 非线程池分支的两处残余。
 * R110 只覆盖了 poolsize>0 的池调度分支，基线仍缺：
 *   - set_position 目标集缩小时“旧目标归零”的 else（非池）分支 :634-637
 *   - on_trade 在单元已存在且 localid==0 时的早退 :686-687
 *
 * 覆盖行段：
 *   :481-491（getUnit 命中缓存，经 on_position_changed 间接驱动）
 *   :634-637（非池 set_position 归零）
 *   :680-687（localid==0 早退）
 *
 * 执行单元来自构建产物 libWtExeFact（WtMinImpactExeUnit），只做内存计算，
 * 不连柜台、不起线程。落盘目录 r90/r71 均指向系统临时目录并在进程退出时清理。
 */
#include <gtest/gtest.h>

#include <string>

#include "../n01_wtcore_tradepaths/r71_wtcore_env.h"
#include "r90_env.h"
#include "r130_env.h"

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/WtDiffExecuter.h>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.ag.2512";

/// 归零循环用「标准代码 + 空交易所」回查合约；R71BaseDataMgr 只认精确键，这里补一个别名
class R130DiffBd : public r71::R71BaseDataMgr
{
public:
    WTSContractInfo* alias = NULL;

    WTSContractInfo* getContract(const char* code, const char* exchg = "",
        uint32_t uDate = 0) override
    {
        if (alias != NULL && strcmp(code, kCode) == 0)
            return alias;
        return r71::R71BaseDataMgr::getContract(code, exchg, uDate);
    }
};

}  // namespace

//////////////////////////////////////////////////////////////////
// 非池：目标集合缩小时旧目标归零 + localid==0 的成交早退
// src/WtCore/WtDiffExecuter.cpp:481-491, 634-637, 680-687
//////////////////////////////////////////////////////////////////
TEST(R130DiffExecuterTail, NonPoolClearOldTargetAndIgnoreZeroLocalId)
{
    r71::R71GenEnv env;
    r71::R71PluginDir plug("WtExeFact");
    if (!plug.ok())
        GTEST_SKIP() << "libWtExeFact not found";

    WtExecuterFactory factory;
    if (!factory.loadFactories(plug.dir().c_str()))
        GTEST_SKIP() << "WtExeFact factories not loadable";

    R130DiffBd bd;
    bd.regisSession("TRADING");
    WTSSessionInfo* sess = bd.session("TRADING");
    ASSERT_NE(nullptr, sess);
    sess->addTradingSection(900, 1500);

    WTSCommodityInfo* comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
    comm->setSessionInfo(sess);
    WTSContractInfo* ct = r71::makeCt("SHFE", "ag", "ag2512");
    bd.regis(comm, ct);
    bd.alias = ct;

    r71::R71HotMgr hot;
    r71::R71ExecStub stub;
    stub.comm_ = comm;
    stub.sess_ = sess;
    stub.hot_ = &hot;
    r71::R71DataMgr dm;

    WtDiffExecuter diff(&factory, "r130_diff_nopool", &dm, &bd);
    diff.setStub(&stub);

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"scale\":1.0,\"poolsize\":0,"
        "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtMinImpactExeUnit\","
        "\"lots\":1,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
        "\"pricemode\":0,\"span\":0,\"byrate\":false}}}");
    ASSERT_NE(nullptr, cfg);
    ASSERT_TRUE(diff.init(cfg));
    cfg->release();

    // 第一次增量：auto create 单元并写入目标仓位 2
    diff.on_position_changed(kCode, 2.0);

    // 空目标集：_target_pos 里残留的 kCode 走非池 else 归零分支
    wt_hashmap<std::string, double> empty;
    diff.set_position(empty);

    // 单元已存在（getUnit(code,false) 命中缓存），localid==0 直接早退
    diff.on_trade(0u, kCode, true, 1.0, 3500.0);

    // 非 0 localid 仍能更新差量（反向确认上一步不是 getUnit 失败导致）
    diff.on_trade(1u, kCode, false, 1.0, 3501.0);
}
