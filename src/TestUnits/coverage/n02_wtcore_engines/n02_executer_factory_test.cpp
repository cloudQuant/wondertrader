/**
 * n02_executer_factory_test.cpp  (lane N02 / WtCore 后半)
 *
 * 覆盖目标:
 *   src/WtCore/WtExecuterFactory.cpp : loadFactories / createExeUnit / createDiffExeUnit /
 *                                      createArbiExeUnit (两个重载)
 *   src/WtCore/WtExecuterFactory.h   : ExeUnitWrapper (ctor / self / dtor) 与 ExecuteUnitPtr 生命周期
 *
 * 跳过:
 *   - create* 的"工厂已加载"成功分支: 需要真实可 dlopen 的执行器插件 dylib(由构建产出),
 *     本 lane 不允许触达构建, 交由主控集成阶段跑通。
 */
#include <gtest/gtest.h>

#include "WtCore/WtExecuterFactory.h"
#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

USING_NS_WTP;
namespace {

namespace fs = std::filesystem;

class TempRoot
{
public:
    explicit TempRoot(const std::string& tag)
    {
        std::string name = "n02_" + tag + "_" + std::to_string(static_cast<long long>(::getpid()));
        _path = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(_path, ec);
        fs::create_directories(_path, ec);
    }

    ~TempRoot()
    {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }

    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    std::string file(const std::string& rel) const { return (_path / rel).string(); }

    void write(const std::string& rel, const std::string& content) const
    {
        std::ofstream ofs((_path / rel).string().c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

    void mkdir(const std::string& rel) const
    {
        std::error_code ec;
        fs::create_directories(_path / rel, ec);
    }

private:
    fs::path _path;
};

//////////////////////////////////////////////////////////////////
// 测试替身: 一个最小 ExecuteUnit + 一个最小 IExecuterFact
//////////////////////////////////////////////////////////////////

class FakeExecuteUnit : public ExecuteUnit
{
public:
    explicit FakeExecuteUnit(const char* name) : _name(name) {}
    ~FakeExecuteUnit() override {}

    const char* getName() override { return _name.c_str(); }
    const char* getFactName() override { return "fake_fact"; }

    void set_position(const char* stdCode, double newVol) override
    {
        _lastCode = stdCode;
        _lastVol = newVol;
        ++_setPositionCalls;
    }

    void on_tick(WTSTickData*) override { ++_tickCalls; }

    void on_trade(uint32_t localid, const char*, bool isBuy, double vol, double) override
    {
        _lastLocalId = localid;
        _lastIsBuy = isBuy;
        _lastTradeVol = vol;
        ++_tradeCalls;
    }

    void on_order(uint32_t localid, const char*, bool isBuy, double leftover, double, bool isCanceled) override
    {
        _lastLocalId = localid;
        _lastIsBuy = isBuy;
        _lastLeftover = leftover;
        _lastCanceled = isCanceled;
        ++_orderCalls;
    }

    void on_entrust(uint32_t localid, const char*, bool bSuccess, const char*) override
    {
        _lastLocalId = localid;
        _lastEntrustOk = bSuccess;
        ++_entrustCalls;
    }

    void on_channel_ready() override { ++_readyCalls; }
    void on_channel_lost() override { ++_lostCalls; }

    int _setPositionCalls = 0;
    int _tickCalls = 0;
    int _tradeCalls = 0;
    int _orderCalls = 0;
    int _entrustCalls = 0;
    int _readyCalls = 0;
    int _lostCalls = 0;
    uint32_t _lastLocalId = 0;
    double _lastVol = 0;
    double _lastTradeVol = 0;
    double _lastLeftover = 0;
    bool _lastIsBuy = false;
    bool _lastCanceled = false;
    bool _lastEntrustOk = false;
    std::string _lastCode;

private:
    std::string _name;
};

class FakeExecuterFact : public IExecuterFact
{
public:
    FakeExecuterFact() : _name("fake_fact") {}
    ~FakeExecuterFact() override {}

    const char* getName() override { return _name.c_str(); }

    void enumExeUnit(FuncEnumUnitCallback) override {}

    ExecuteUnit* createExeUnit(const char* name) override
    {
        ++_plainCalls;
        if (_returnNull)
            return NULL;
        return new FakeExecuteUnit(name);
    }

    ExecuteUnit* createDiffExeUnit(const char* name) override
    {
        ++_diffCalls;
        if (_returnNull)
            return NULL;
        return new FakeExecuteUnit(name);
    }

    ExecuteUnit* createArbiExeUnit(const char* name) override
    {
        ++_arbiCalls;
        if (_returnNull)
            return NULL;
        return new FakeExecuteUnit(name);
    }

    bool deleteExeUnit(ExecuteUnit* unit) override
    {
        delete unit;
        ++_deleteCalls;
        return true;
    }

    int _plainCalls = 0;
    int _diffCalls = 0;
    int _arbiCalls = 0;
    int _deleteCalls = 0;
    bool _returnNull = false;

private:
    std::string _name;
};

//////////////////////////////////////////////////////////////////
// WtExecuterFactory::loadFactories
//////////////////////////////////////////////////////////////////

TEST(N02ExecuterFactoryTest, LoadFactoriesFromMissingDirectoryReturnsFalse)
{
    TempRoot root("exefact");
    const std::string missing = root.file("no_such_dir");
    WtExecuterFactory factory;
    EXPECT_FALSE(factory.loadFactories(missing.c_str()));
}

TEST(N02ExecuterFactoryTest, LoadFactoriesFromEmptyDirectoryReturnsTrue)
{
    TempRoot root("exefact");
    root.mkdir("empty_plugins");
    WtExecuterFactory factory;
    EXPECT_TRUE(factory.loadFactories(root.file("empty_plugins").c_str()));
}

TEST(N02ExecuterFactoryTest, LoadFactoriesSkipsDirectoriesAndForeignExtensions)
{
    TempRoot root("exefact");
    root.mkdir("plugins");
    root.mkdir("plugins/sub.dylib");      // 目录即使名字带后缀也要跳过
    root.write("plugins/notes.txt", "hello");
    root.write("plugins/WtSomething.so", "elf payload");
    root.write("plugins/bogus.dylib", "this is definitely not mach-o");

    WtExecuterFactory factory;
    // 只有 .dylib 会被尝试 dlopen, 且必然失败 -> continue, 整体仍返回 true
    EXPECT_TRUE(factory.loadFactories(root.file("plugins").c_str()));

    // 工厂表为空, 任何创建请求都应返回空指针
    EXPECT_EQ(nullptr, factory.createExeUnit("bogus", "unit").get());
}

TEST(N02ExecuterFactoryTest, LoadFactoriesWithRegularFileThrows)
{
    TempRoot root("exefact");
    root.write("a_plain_file.dylib", "not a directory");
    WtExecuterFactory factory;
    // StdFile::exists 对普通文件为真, 但 directory_iterator 会抛 filesystem_error
    EXPECT_THROW(factory.loadFactories(root.file("a_plain_file.dylib").c_str()),
                 std::exception);
}

//////////////////////////////////////////////////////////////////
// create* 的"工厂未注册"分支（两个重载）
//////////////////////////////////////////////////////////////////

TEST(N02ExecuterFactoryTest, CreatePlainUnitFailsForUnknownFactory)
{
    TempRoot root("exefact");
    root.mkdir("empty_plugins");
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(root.file("empty_plugins").c_str()));

    EXPECT_EQ(nullptr, factory.createExeUnit("not_loaded", "StdExecuter").get());
    EXPECT_EQ(nullptr, factory.createExeUnit("not_loaded.StdExecuter").get());
}

TEST(N02ExecuterFactoryTest, CreateDiffUnitFailsForUnknownFactory)
{
    TempRoot root("exefact");
    root.mkdir("empty_plugins");
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(root.file("empty_plugins").c_str()));

    EXPECT_EQ(nullptr, factory.createDiffExeUnit("not_loaded", "QuantExecuter").get());
    EXPECT_EQ(nullptr, factory.createDiffExeUnit("not_loaded.QuantExecuter").get());
}

TEST(N02ExecuterFactoryTest, CreateArbiUnitFailsForUnknownFactory)
{
    TempRoot root("exefact");
    root.mkdir("empty_plugins");
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(root.file("empty_plugins").c_str()));

    EXPECT_EQ(nullptr, factory.createArbiExeUnit("not_loaded", "ArbiExecuter").get());
    EXPECT_EQ(nullptr, factory.createArbiExeUnit("not_loaded.ArbiExecuter").get());
}

TEST(N02ExecuterFactoryTest, SingleNameOverloadRejectsNameWithoutFactoryDot)
{
    TempRoot root("exefact");
    root.mkdir("empty_plugins");
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(root.file("empty_plugins").c_str()));

    // split(name, ".") 少于两段 -> 直接返回空, 不查表
    EXPECT_EQ(nullptr, factory.createExeUnit("OnlyUnitName").get());
    EXPECT_EQ(nullptr, factory.createDiffExeUnit("OnlyUnitName").get());
    EXPECT_EQ(nullptr, factory.createArbiExeUnit("OnlyUnitName").get());
    // 空串同样少于两段
    EXPECT_EQ(nullptr, factory.createExeUnit("").get());
}

TEST(N02ExecuterFactoryTest, ThreeSegmentNameTakesFirstTwoParts)
{
    TempRoot root("exefact");
    root.mkdir("empty_plugins");
    WtExecuterFactory factory;
    ASSERT_TRUE(factory.loadFactories(root.file("empty_plugins").c_str()));

    // "fact.unit.extra" 会被切成 fact / unit, 但工厂不存在, 仍返回空
    EXPECT_EQ(nullptr, factory.createExeUnit("fact.unit.extra").get());
    EXPECT_EQ(nullptr, factory.createDiffExeUnit("fact.unit.extra").get());
    EXPECT_EQ(nullptr, factory.createArbiExeUnit("fact.unit.extra").get());
}

//////////////////////////////////////////////////////////////////
// ExeUnitWrapper 生命周期
//////////////////////////////////////////////////////////////////

TEST(N02ExecuterFactoryTest, WrapperExposesUnitAndReleasesItThroughFact)
{
    FakeExecuterFact fact;
    FakeExecuteUnit* raw = new FakeExecuteUnit("StdExecuter");
    {
        ExecuteUnitPtr unit(new ExeUnitWrapper(raw, &fact));
        ASSERT_NE(nullptr, unit.get());
        ASSERT_EQ(raw, unit->self());
        EXPECT_STREQ("StdExecuter", unit->self()->getName());
        EXPECT_STREQ("fake_fact", unit->self()->getFactName());
        EXPECT_EQ(0, fact._deleteCalls);
    }
    // 离开作用域后必须由工厂的 deleteExeUnit 回收, 而不是直接 delete
    EXPECT_EQ(1, fact._deleteCalls);
}

TEST(N02ExecuterFactoryTest, WrapperReleasesExactlyOnceForSharedOwner)
{
    FakeExecuterFact fact;
    ExecuteUnitPtr unit(new ExeUnitWrapper(new FakeExecuteUnit("QuantExecuter"), &fact));
    ExecuteUnitPtr alias = unit;
    ASSERT_EQ(unit.get(), alias.get());
    EXPECT_EQ(2L, unit.use_count());
    unit.reset();
    EXPECT_EQ(0, fact._deleteCalls);      // 仍有一个引用
    alias.reset();
    EXPECT_EQ(1, fact._deleteCalls);
}

TEST(N02ExecuterFactoryTest, WrapperWithNullUnitDoesNotTouchFact)
{
    FakeExecuterFact fact;
    {
        ExecuteUnitPtr unit(new ExeUnitWrapper(NULL, &fact));
        EXPECT_EQ(nullptr, unit->self());
    }
    EXPECT_EQ(0, fact._deleteCalls);
}

TEST(N02ExecuterFactoryTest, FakeUnitCallbacksAreReachableThroughBasePointer)
{
    FakeExecuterFact fact;
    ExecuteUnitPtr unit(new ExeUnitWrapper(fact.createExeUnit("StdExecuter"), &fact));
    ASSERT_EQ(1, fact._plainCalls);
    ASSERT_NE(nullptr, unit->self());

    ExecuteUnit* raw = unit->self();
    raw->set_position("SHFE.rb2310", 4);
    raw->on_tick(NULL);
    raw->on_trade(1001, "SHFE.rb2310", true, 3, 4000.0);
    raw->on_order(1001, "SHFE.rb2310", false, 1, 4010.0, true);
    raw->on_entrust(1002, "SHFE.rb2310", false, "rejected");
    raw->on_channel_ready();
    raw->on_channel_lost();

    FakeExecuteUnit* probe = static_cast<FakeExecuteUnit*>(raw);
    EXPECT_EQ(1, probe->_setPositionCalls);
    EXPECT_EQ(1, probe->_tickCalls);
    EXPECT_EQ(1, probe->_tradeCalls);
    EXPECT_EQ(1, probe->_orderCalls);
    EXPECT_EQ(1, probe->_entrustCalls);
    EXPECT_EQ(1, probe->_readyCalls);
    EXPECT_EQ(1, probe->_lostCalls);
    EXPECT_STREQ("SHFE.rb2310", probe->_lastCode.c_str());
    EXPECT_DOUBLE_EQ(4, probe->_lastVol);
    EXPECT_EQ(1002u, probe->_lastLocalId);
    EXPECT_TRUE(probe->_lastCanceled);
    EXPECT_FALSE(probe->_lastEntrustOk);

    EXPECT_EQ(1, fact._diffCalls + fact._arbiCalls);   // 0, 上面只创建了普通单元
}

TEST(N02ExecuterFactoryTest, FactCanReturnNullForUnknownUnitName)
{
    FakeExecuterFact fact;
    fact._returnNull = true;
    EXPECT_EQ(NULL, fact.createExeUnit("missing"));
    EXPECT_EQ(NULL, fact.createDiffExeUnit("missing"));
    EXPECT_EQ(NULL, fact.createArbiExeUnit("missing"));
    EXPECT_EQ(3, fact._plainCalls + fact._diffCalls + fact._arbiCalls);
}

}  // namespace
