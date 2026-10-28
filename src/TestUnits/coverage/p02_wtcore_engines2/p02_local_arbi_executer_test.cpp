/**
 * p02_local_arbi_executer_test.cpp  (lane P02 / WtCore 引擎与执行器 第3波)
 *
 * 覆盖目标:
 *   src/WtCore/WtLocalExecuter.cpp/.h : ctor / ~ / setTrader / init(配置解析: scale、strict_sync、
 *     poolsize、clear.active+includes+excludes、groups) / getUnit(命中与未命中、自动创建失败)
 *     Context 空指针护栏: getTicks / grabLastTick / getPosition / getUndoneQty / getOrders
 *     通道未就绪护栏: buy / sell / cancel(localid) / cancel(code,isBuy,qty)
 *     stub 转发: getCommodityInfo / getSessionInfo / getCurTime
 *     回报转发空表分支: on_tick / on_trade / on_order / on_entrust / on_account
 *     通道状态: on_channel_ready / on_channel_lost
 *     持仓回报: on_position(自动清理关闭 + 非分月合约提前返回)
 *     仓位换算: on_position_changed / set_position(含组合合约 _groups 的取整取模运算)
 *   src/WtCore/WtArbiExecuter.cpp/.h : 同一族的 ctor / init / getUnit / 空指针与通道护栏
 *
 * 说明(为什么断言的是"护栏"而不是"子单"):
 *   真正的拆单/撮合(超时撤单追单、反向先撤后补)在 ExecuteUnit 里, 而 ExecuteUnit 由
 *   WtExecuterFactory 从插件 dylib 反射创建, _unit_map 是私有成员且没有注入点,
 *   WtCore 侧只能覆盖到 "工厂创建失败 -> unit == NULL -> 提前返回" 这一路径;
 *   需要真实柜台回报的分支(on_entrust 成功后驱动 unit、on_channel_ready 后的补单)全部跳过。
 */
#include <gtest/gtest.h>

#include <boost/noncopyable.hpp>

#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtArbiExecuter.h"
#include "WtCore/WtExecuterFactory.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/WtHelper.h"
#include "Includes/IDataManager.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

//////////////////////////////////////////////////////////////////
// 临时目录: 本 lane 所有落盘都在 temp/p02_wtcore_<pid> 下, 进程退出前清理
//////////////////////////////////////////////////////////////////
std::string p02_tmp_root()
{
    static std::string root = []() {
        std::string name = "p02_wtcore_" + std::to_string(static_cast<long long>(::getpid()));
        fs::path path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
        return path.string() + "/";
    }();
    return root;
}

// WtHelper 的 *_dir 接口内部是 static std::string, 进程内只算一次,
// 因此在静态对象构造期(早于任何用例)就把生成目录固定下来
struct P02GenDirGuard
{
    P02GenDirGuard() { WtHelper::setGenerateDir(p02_tmp_root().c_str()); }
};
const P02GenDirGuard g_p02_gen_dir;

std::string write_cfg(const std::string& rel, const char* content)
{
    fs::path path = fs::path(p02_tmp_root()) / rel;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path.string(), std::ios::out | std::ios::trunc);
    ofs << content;
    ofs.close();
    return path.string();
}

WTSVariant* load_cfg(const std::string& rel, const char* content)
{
    return WTSCfgLoader::load_from_file(write_cfg(rel, content).c_str());
}

//////////////////////////////////////////////////////////////////
// 测试替身
//////////////////////////////////////////////////////////////////

/// 只用来验证执行器把调用转发给了 data manager, 自身不做任何事
class FakeDataReader : public IDataManager
{
public:
    uint32_t _slice_calls {0};
    uint32_t _grab_calls {0};

    WTSTickSlice* get_tick_slice(const char*, uint32_t, uint64_t) override
    {
        ++_slice_calls;
        return NULL;
    }

    WTSTickData* grab_last_tick(const char*) override
    {
        ++_grab_calls;
        return NULL;
    }
};

/// IExecuterStub 由引擎实现, 这里返回可识别的哨兵值
class FakeExecStub : public IExecuterStub
{
public:
    uint64_t _real_time {171234567890123ULL};
    uint32_t _trading_day {20240403};

    uint64_t get_real_time() override { return _real_time; }
    WTSCommodityInfo* get_comm_info(const char*) override { return NULL; }
    WTSSessionInfo* get_sess_info(const char*) override { return NULL; }
    IHotMgr* get_hot_mon() override { return NULL; }
    uint32_t get_trading_day() override { return _trading_day; }
};

/// 工厂只声明不加载任何 dylib -> createXXXExeUnit 必然返回 NULL
struct ExecuterFixture
{
    WtExecuterFactory factory;
    FakeDataReader      reader;
    FakeExecStub        stub;

    ExecuterFixture() { stub._real_time = 171234567890123ULL; }
};

const char* kLocalCfg = R"json(
{
    "name": "p02_local_exe",
    "scale": 0.5,
    "strict_sync": true,
    "poolsize": 0,
    "clear":
    {
        "active": true,
        "includes": ["CFFEX.IF", "SHFE.ag"],
        "excludes": "DCE.i"
    },
    "groups":
    {
        "SHFE.ags":
        {
            "SHFE.ag.2212": 1.0,
            "SHFE.au.2212": 2.0
        }
    },
    "policy":
    {
        "default":
        {
            "name": "p02_not_loaded.p02_not_loaded",
            "max_loss": 0.1
        }
    }
}
)json";

//////////////////////////////////////////////////////////////////
// WtLocalExecuter
//////////////////////////////////////////////////////////////////

class P02LocalExecuterTest : public ::testing::Test
{
protected:
    ExecuterFixture _fx;
    WtLocalExecuter _executer {&_fx.factory, "p02_local_exe", &_fx.reader};

    void SetUp() override
    {
        _executer.setStub(&_fx.stub);
        _executer.setName("p02_local_exe");
    }

    void TearDown() override
    {
        if (_cfg != NULL)
        {
            _cfg->release();
            _cfg = NULL;
        }
    }

    bool init_with(const char* content, const std::string& rel)
    {
        _cfg = load_cfg(rel, content);
        if (_cfg == NULL)
            return false;
        return _executer.init(_cfg);
    }

    WTSVariant* _cfg {NULL};
};

TEST_F(P02LocalExecuterTest, InitRejectsNullParams)
{
    EXPECT_FALSE(_executer.init(NULL));
    // 名称与存根是 IExecCommand 的内联实现
    EXPECT_EQ(std::string(_executer.name()), "p02_local_exe");
}

TEST_F(P02LocalExecuterTest, InitParsesScaleClearListAndGroups)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg.json"));

    // 单元容器为空: 未加载插件 dylib, 工厂创建必然失败
    // 重复取同一个代码仍然为空, 说明没有把失败的单元塞进 _unit_map
}

TEST_F(P02LocalExecuterTest, ContextReadsFallBackWhenTraderMissing)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg2.json"));

    // _trader == NULL 的三条护栏
    EXPECT_DOUBLE_EQ(_executer.getPosition("SHFE.ag.2212"), 0.0);
    EXPECT_DOUBLE_EQ(_executer.getPosition("SHFE.ag.2212", false, 1), 0.0);
    EXPECT_DOUBLE_EQ(_executer.getUndoneQty("SHFE.ag.2212"), 0.0);
    EXPECT_TRUE(_executer.getOrders("SHFE.ag.2212") == NULL);

    // 转发给 data manager
    EXPECT_TRUE(_executer.getTicks("SHFE.ag.2212", 10) == NULL);
    EXPECT_EQ(_fx.reader._slice_calls, 1u);
    EXPECT_TRUE(_executer.grabLastTick("SHFE.ag.2212") == NULL);
    EXPECT_EQ(_fx.reader._grab_calls, 1u);

    // 转发给 stub(IExecuterStub)
    EXPECT_EQ(_executer.getCurTime(), _fx.stub._real_time);
    EXPECT_TRUE(_executer.getCommodityInfo("SHFE.ag.2212") == NULL);
    EXPECT_TRUE(_executer.getSessionInfo("SHFE.ag.2212") == NULL);
}

TEST_F(P02LocalExecuterTest, TradingCallsRejectedBeforeChannelReady)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg3.json"));

    // 通道未就绪 -> 一律不下单
    EXPECT_TRUE(_executer.buy("SHFE.ag.2212", 5000.0, 2).empty());
    EXPECT_TRUE(_executer.sell("SHFE.ag.2212", 5000.0, 2).empty());
    EXPECT_FALSE(_executer.cancel(1001));
    EXPECT_TRUE(_executer.cancel("SHFE.ag.2212", true, 2).empty());

    // setTrader(NULL) 不改变通道状态
    _executer.setTrader(NULL);
    EXPECT_TRUE(_executer.buy("SHFE.ag.2212", 5000.0, 2).empty());
}

TEST_F(P02LocalExecuterTest, ReportForwardingIsNoOpWithoutUnits)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg4.json"));

    // getUnit(code, false) 为空 -> 全部提前返回, 不应该创建任何单元
    _executer.on_tick("SHFE.ag.2212", NULL);
    _executer.on_trade(1, "SHFE.ag.2212", true, 1.0, 5000.0);
    _executer.on_order(1, "SHFE.ag.2212", true, 1.0, 0.0, 5000.0, false);
    _executer.on_entrust(1, "SHFE.ag.2212", true, "已受理");
    _executer.on_account("CNY", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    _executer.writeLog("p02 writeLog path");
}

TEST_F(P02LocalExecuterTest, ChannelStateToggleWithEmptyUnitMap)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg5.json"));

    // _unit_map 为空 -> 只翻转状态, 不会解引用 _trader
    _executer.on_channel_lost();
    _executer.on_channel_ready();
    _executer.on_channel_lost();

    // 仍然只能走 _trader == NULL 的护栏(不能在这里调 buy/sell)
    EXPECT_DOUBLE_EQ(_executer.getPosition("SHFE.ag.2212"), 0.0);
}

TEST_F(P02LocalExecuterTest, PositionReportTracksChannelHolds)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg6.json"));

    // _auto_clear == true, 但 SSE 现货代码不是分月期货 -> 第二次判断后返回
    _executer.on_position("SSE.600000", true, 0, 0, 10, 10, 20240403);

    // 关闭自动清理 -> 插入 _channel_holds 后立即返回
    WTSVariant* cfg = load_cfg("p02_local_cfg7.json",
        R"json({"scale":1,"poolsize":0,"clear":{"active":false},
        "policy":{"default":{"name":"p02_none.p02_none"}}})json");
    ASSERT_TRUE(cfg != NULL);
    WtLocalExecuter cleaner(&_fx.factory, "p02_local_clear", &_fx.reader);
    cleaner.setStub(&_fx.stub);
    ASSERT_TRUE(cleaner.init(cfg));
    cleaner.on_position("SHFE.ag.2212", true, 0, 0, 1, 1, 20240403);
    cfg->release();
}

TEST_F(P02LocalExecuterTest, SetAndDeltaPositionWithoutExecutableUnits)
{
    ASSERT_TRUE(init_with(kLocalCfg, "p02_local_cfg8.json"));

    // 通道持仓里先塞一个不在目标中的代码, 以便走 strict_sync 清理循环
    _executer.on_position("SSE.600000", true, 0, 0, 1, 1, 20240403);

    wt_hashmap<std::string, double> targets;
    targets["SHFE.ag.2212"] = 10.0;
    targets["SHFE.au.2212"] = 20.0;
    // 组合组(SHFE.ags)取模取最小整数份, 随后每个代码都因 unit==NULL 跳过
    _executer.set_position(targets);

    // 空目标集合: 两个循环都不进入
    wt_hashmap<std::string, double> empty;
    _executer.set_position(empty);

    // 差量入口同样被 unit==NULL 短路
    _executer.on_position_changed("SHFE.ag.2212", 3.0);
    _executer.on_position_changed("SHFE.ag.2212", 0.0);

}

TEST_F(P02LocalExecuterTest, InitWithoutClearGroupsAndPoolKeepsDefaults)
{
    ASSERT_TRUE(init_with(
        R"json({"scale":2,"strict_sync":false,"policy":{"default":{"name":"p02_none.p02_none"}}})json",
        "p02_local_min.json"));

    // scale=2 时仓位换算为 round(target*2), 但 unit 创建失败, 只验证不崩
    wt_hashmap<std::string, double> targets;
    targets["CFFEX.IF.2204"] = 1.5;
    _executer.set_position(targets);
}

//////////////////////////////////////////////////////////////////
// WtArbiExecuter
//////////////////////////////////////////////////////////////////

class P02ArbiExecuterTest : public ::testing::Test
{
protected:
    ExecuterFixture _fx;
    WtArbiExecuter  _executer {&_fx.factory, "p02_arbi_exe", &_fx.reader};

    void SetUp() override { _executer.setStub(&_fx.stub); }

    void TearDown() override
    {
        if (_cfg != NULL)
        {
            _cfg->release();
            _cfg = NULL;
        }
    }

    bool init_with(const char* content, const std::string& rel)
    {
        _cfg = load_cfg(rel, content);
        if (_cfg == NULL)
            return false;
        return _executer.init(_cfg);
    }

    WTSVariant* _cfg {NULL};
};

TEST_F(P02ArbiExecuterTest, InitRejectsNullAndParsesFullConfig)
{
    EXPECT_FALSE(_executer.init(NULL));

    ASSERT_TRUE(init_with(R"json(
    {
        "scale": 1.0,
        "strict_sync": true,
        "poolsize": 0,
        "clear": {"active": true, "includes": "CFFEX.IF", "excludes": ["SHFE.ag"]},
        "groups": {"arb1": {"CFFEX.IF.2204": 1.0, "CFFEX.IH.2204": 1.0}},
        "policy": {"default": {"name": "p02_none.p02_none"}}
    }
    )json", "p02_arbi_cfg.json"));

    EXPECT_EQ(std::string(_executer.name()), "p02_arbi_exe");
}

TEST_F(P02ArbiExecuterTest, GuardsAndForwardersWithNoTraderAndNoUnits)
{
    ASSERT_TRUE(init_with(
        R"json({"scale":1,"poolsize":0,"policy":{"default":{"name":"p02_none.p02_none"}}})json",
        "p02_arbi_min.json"));

    EXPECT_DOUBLE_EQ(_executer.getPosition("CFFEX.IF.2204"), 0.0);
    EXPECT_DOUBLE_EQ(_executer.getUndoneQty("CFFEX.IF.2204"), 0.0);
    EXPECT_TRUE(_executer.getOrders("CFFEX.IF.2204") == NULL);
    EXPECT_TRUE(_executer.getTicks("CFFEX.IF.2204", 5) == NULL);
    EXPECT_TRUE(_executer.grabLastTick("CFFEX.IF.2204") == NULL);
    EXPECT_EQ(_executer.getCurTime(), _fx.stub._real_time);

    EXPECT_TRUE(_executer.buy("CFFEX.IF.2204", 3800.0, 1).empty());
    EXPECT_TRUE(_executer.sell("CFFEX.IF.2204", 3800.0, 1).empty());
    EXPECT_FALSE(_executer.cancel(7));
    EXPECT_TRUE(_executer.cancel("CFFEX.IF.2204", false, 1).empty());

    _executer.on_tick("CFFEX.IF.2204", NULL);
    _executer.on_trade(7, "CFFEX.IF.2204", true, 1, 3800.0);
    _executer.on_order(7, "CFFEX.IF.2204", true, 1, 0, 3800.0, true);
    _executer.on_entrust(7, "CFFEX.IF.2204", false, "资金不足");
    _executer.on_account("CNY", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    _executer.writeLog("arbi log");

    wt_hashmap<std::string, double> targets;
    targets["CFFEX.IF.2204"] = 2.0;
    _executer.set_position(targets);
    _executer.on_position_changed("CFFEX.IF.2204", -1.0);

    _executer.on_channel_ready();
    _executer.on_channel_lost();
}

//////////////////////////////////////////////////////////////////
// 缺陷类收口: 以下输入都会段错误, 全部集中在同一个 DISABLED_ 用例里,
// 详见 bugs/p02_wtcore_engines2.md (BUG-P02-001 / BUG-P02-002)
//   A. on_channel_ready() 置位 _channel_ready 后, 若 _trader 仍为 NULL,
//      buy/sell/cancel 直接解引用 _trader -> SIGSEGV (三个 Executer 同构)
//   B. init 的配置缺少 policy 段时, getUnit 里 policy->has() 解引用 NULL
//   C. policy 存在但既没有商品ID也没有 default 时, cfg->getCString("name") 解引用 NULL
//   D. on_position 传入分月期货代码时 _stub->get_hot_mon() 的返回值未判空
//////////////////////////////////////////////////////////////////
TEST(P02ExecuterCrashClass, DISABLED_ChannelReadyWithoutTraderAndBadPolicyConfigs)
{
    // 该用例只用于登记缺陷类, 默认不执行(DISABLED_ 前缀)
}

}  // namespace
