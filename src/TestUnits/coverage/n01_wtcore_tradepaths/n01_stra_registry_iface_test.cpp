/**
 * n01_stra_registry_iface_test.cpp
 * 覆盖 WtCore 的「注册表 + 接口默认实现」层：
 *   - CtaStrategyMgr / HftStrategyMgr / SelStrategyMgr:
 *     loadFactories 目录校验、createStrategy(两段名/三段名) 工厂缺失分支、getStrategy 未命中分支
 *   - CtaStraWrapper / HftStraWrapper / SelStraWrapper（头文件内联）: 构造、self()、析构的空指针保护
 *   - IExecCommand（纯头文件）: 构造、name()/setName()、setStub()、
 *     set_position/on_position_changed/on_tick 三个默认空实现
 *   - ITrdNotifySink（纯头文件）: on_position/on_entrust/on_account 三个默认空实现
 * 说明：真实策略工厂是 SHARED 插件（hidden 可见性），loadFactories 的成功路径与
 * createStrategy 的命中路径都需要动态库，属于跳过目标，见 manifest。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <WtCore/CtaStrategyMgr.h>
#include <WtCore/HftStrategyMgr.h>
#include <WtCore/SelStrategyMgr.h>
#include <WtCore/IExecCommand.h>
#include <WtCore/ITrdNotifySink.h>

USING_NS_WTP;

namespace {

/// 只为把 IExecCommand 的默认空实现「实例化并调用」，不做任何事
class BareExecCommand : public IExecCommand
{
public:
    BareExecCommand() : IExecCommand("bare") {}
};

/// 只为调用 ITrdNotifySink 的三个默认空实现
class BareSink : public ITrdNotifySink
{
public:
    virtual void on_trade(uint32_t, const char*, bool, double, double) override {}
    virtual void on_order(uint32_t, const char*, bool, double, double, double, bool) override {}
    virtual void on_channel_ready() override {}
    virtual void on_channel_lost() override {}
};

std::filesystem::path makeEmptyDir(const char* tag)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path()
        / ("n01_stra_reg_" + std::string(tag) + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST(N01StraRegistry, cta_mgr_empty_registry_lookups_miss)
{
    CtaStrategyMgr mgr;
    EXPECT_EQ(mgr.createStrategy("NoDotName", "id01"), nullptr);
    EXPECT_EQ(mgr.createStrategy("UnknownFact", "UnknownUnit", "id01"), nullptr);
    EXPECT_EQ(mgr.getStrategy("id01"), nullptr);

    // 名字里有点但工厂未加载 -> 走 _factories.end() 分支
    EXPECT_EQ(mgr.createStrategy("MyFact.MyUnit", "id02"), nullptr);
    EXPECT_EQ(mgr.getStrategy("id02"), nullptr);
}

TEST(N01StraRegistry, hft_mgr_empty_registry_lookups_miss)
{
    HftStrategyMgr mgr;
    EXPECT_EQ(mgr.createStrategy("NoDotName", "hid01"), nullptr);
    EXPECT_EQ(mgr.createStrategy("UnknownFact", "UnknownUnit", "hid01"), nullptr);
    EXPECT_EQ(mgr.createStrategy("MyFact.MyUnit", "hid02"), nullptr);
    EXPECT_EQ(mgr.getStrategy("hid02"), nullptr);
}

TEST(N01StraRegistry, sel_mgr_empty_registry_lookups_miss)
{
    SelStrategyMgr mgr;
    EXPECT_EQ(mgr.createStrategy("NoDotName", "sid01"), nullptr);
    EXPECT_EQ(mgr.createStrategy("UnknownFact", "UnknownUnit", "sid01"), nullptr);
    EXPECT_EQ(mgr.createStrategy("MyFact.MyUnit", "sid02"), nullptr);
    EXPECT_EQ(mgr.getStrategy("sid02"), nullptr);
}

// 名字拆分只接受 "工厂.单元" 两段以上；单段名必须直接失败而不是越界
TEST(N01StraRegistry, single_segment_name_never_reaches_the_factory_table)
{
    CtaStrategyMgr cta;
    EXPECT_EQ(cta.createStrategy("", ""), nullptr);
    EXPECT_EQ(cta.createStrategy("OnlyFact", ""), nullptr);
    EXPECT_EQ(cta.createStrategy("a.b.c", "id_abc"), nullptr);

    SelStrategyMgr sel;
    EXPECT_EQ(sel.createStrategy("", ""), nullptr);
    EXPECT_EQ(sel.createStrategy("a.b.c", "id_abc"), nullptr);
}

// loadFactories 的两个目录分支：空目录（没有任何 dll）与不存在的目录
TEST(N01StraRegistry, load_factories_tolerates_empty_and_missing_directories)
{
    const auto emptyDir = makeEmptyDir("empty");
    const auto missing = std::filesystem::temp_directory_path()
        / ("n01_stra_reg_missing_" + std::to_string(::getpid()));

    CtaStrategyMgr cta;
    EXPECT_NO_THROW(cta.loadFactories(emptyDir.string().c_str()));
    EXPECT_NO_THROW(cta.loadFactories(missing.string().c_str()));
    EXPECT_EQ(cta.getStrategy("nobody"), nullptr);

    HftStrategyMgr hft;
    EXPECT_NO_THROW(hft.loadFactories(emptyDir.string().c_str()));
    EXPECT_NO_THROW(hft.loadFactories(missing.string().c_str()));

    SelStrategyMgr sel;
    EXPECT_NO_THROW(sel.loadFactories(emptyDir.string().c_str()));
    EXPECT_NO_THROW(sel.loadFactories(missing.string().c_str()));

    std::error_code ec;
    std::filesystem::remove_all(emptyDir, ec);
}

// 三个 Wrapper 的构造/self()/析构（析构里有 if(_stra) 保护，传 NULL 不会回调工厂）
TEST(N01StraRegistry, wrappers_expose_self_and_guard_null_strategy)
{
    {
        CtaStraWrapper ctaWrap(NULL, NULL);
        EXPECT_EQ(ctaWrap.self(), nullptr);
    }
    {
        HftStraWrapper hftWrap(NULL, NULL);
        EXPECT_EQ(hftWrap.self(), nullptr);
    }
    {
        SelStraWrapper selWrap(NULL, NULL);
        EXPECT_EQ(selWrap.self(), nullptr);
    }
}

TEST(N01ExecCommand, name_and_stub_accessors)
{
    BareExecCommand cmd;
    EXPECT_STREQ(cmd.name(), "bare");

    cmd.setName("renamed-executor");
    EXPECT_STREQ(cmd.name(), "renamed-executor");

    cmd.setName("");
    EXPECT_STREQ(cmd.name(), "");

    cmd.setStub(NULL);
    EXPECT_STREQ(cmd.name(), "");
}

// IExecCommand 的三个默认实现必须是「可调用的空操作」，不是纯虚
TEST(N01ExecCommand, default_hooks_are_callable_noops)
{
    BareExecCommand cmd;
    wt_hashmap<std::string, double> targets;
    targets["SHFE.rb2510"] = 5.0;
    targets["DCE.a2509"] = -3.0;

    cmd.set_position(targets);
    cmd.set_position(wt_hashmap<std::string, double>());
    cmd.on_position_changed("SHFE.rb2510", 2.0);
    cmd.on_position_changed("DCE.a2509", -2.0);
    cmd.on_tick("SHFE.rb2510", NULL);
    EXPECT_STREQ(cmd.name(), "bare");
}

// ITrdNotifySink 的三个默认空实现（on_position / on_entrust / on_account）
TEST(N01TrdNotifySink, default_optional_callbacks_are_noops)
{
    BareSink sink;
    sink.on_position("SHFE.rb2510", true, 10, 8, 2, 2, 20250110);
    sink.on_entrust(1001, "SHFE.rb2510", true, "ok");
    sink.on_entrust(1002, "SHFE.rb2510", false, "rejected");
    sink.on_account("CNY", 100000, 100000, 101000, 90000, 500, 1000, 9000, 30, 0, 0);

    // 纯虚的四个必须走 BareSink 的覆盖实现
    sink.on_trade(1001, "SHFE.rb2510", true, 2, 3500);
    sink.on_order(1001, "SHFE.rb2510", true, 5, 3, 3500, false);
    sink.on_channel_ready();
    sink.on_channel_lost();
    SUCCEED();
}

TEST(N01TrdNotifySink, default_on_order_is_called_through_base_reference)
{
    BareSink derived;
    // on_order 的 isCanceled 有默认实参，经基类引用调用时必须能编译并命中覆盖版本
    ITrdNotifySink* sink = &derived;
    sink->on_order(2001, "DCE.a2509", false, 3, 0, 4000);
    sink->on_position("DCE.a2509", false, 0, 0, 3, 3, 20250110);
    SUCCEED();
}
