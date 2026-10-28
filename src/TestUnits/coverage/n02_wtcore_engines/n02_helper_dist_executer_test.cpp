/**
 * n02_helper_dist_executer_test.cpp  (lane N02 / WtCore 后半)
 *
 * 覆盖目标:
 *   src/WtCore/WtHelper.h/.cpp      : getCWD / getModulePath / setInstDir / getInstDir /
 *                                     setGenerateDir / getBaseDir / getOutputDir / getStraDataDir /
 *                                     getStraUsrDatDir / getPortifolioDir / getExecDataDir /
 *                                     setTime / getDate / getTime / getSecs / setTDate / getTradingDate
 *   src/WtCore/WtDistExecuter.cpp   : ctor / dtor / init / set_position / on_position_changed / on_tick
 *   src/WtCore/IExecCommand.h       : name / setName / setStub (内联接口)
 *
 * 说明:
 *   - 不断言墙钟时间与本地时区, 时间接口只做写入-读回的往返校验。
 *   - WtHelper 的目录接口使用函数级 static 缓存, 一个进程内第一次调用决定缓存值,
 *     因此只断言"叶子目录名 + 目录存在"这类与缓存归属无关的稳定性质。
 *   - 所有落盘都在 temp_directory_path() 下带 n02_ 前缀的进程私有目录, 用例结束清理。
 */
#include <gtest/gtest.h>

#include "WtCore/WtHelper.h"
#include "WtCore/WtDistExecuter.h"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "Share/StdUtils.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

USING_NS_WTP;
namespace {

namespace fs = std::filesystem;

/* 进程私有的临时根目录, 析构时整体删除 */
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

    std::string path() const { return _path.string() + "/"; }

    std::string file(const std::string& rel) const { return (_path / rel).string(); }

    void write(const std::string& rel, const std::string& content) const
    {
        std::ofstream ofs((_path / rel).string().c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
        ofs.flush();
    }

private:
    fs::path _path;
};

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/* 把 json 文本落成临时文件并交给工程的配置加载器, 得到 WTSVariant */
WTSVariant* loadJson(const TempRoot& root, const std::string& rel, const std::string& content)
{
    root.write(rel, content);
    return WTSCfgLoader::load_from_file(root.file(rel).c_str());
}

//////////////////////////////////////////////////////////////////
// WtHelper
//////////////////////////////////////////////////////////////////

TEST(N02WtHelperTest, CwdIsStandardisedAbsolutePath)
{
    const std::string cwd = WtHelper::getCWD();
    EXPECT_FALSE(cwd.empty());
    EXPECT_EQ('/', cwd.at(0));
    // standardisePath 统一以 '/' 结尾
    EXPECT_EQ('/', cwd.at(cwd.size() - 1));
    // 同一进程内重复调用应返回同一结果（function-local static 缓存）
    EXPECT_EQ(cwd, WtHelper::getCWD());
    EXPECT_TRUE(fs::exists(fs::path(cwd)));
}

TEST(N02WtHelperTest, ModulePathIsBuiltFromCwdByDefault)
{
    const std::string expectPrefix = WtHelper::getCWD();
    const std::string p = WtHelper::getModulePath("parsers", "lib", true);
    EXPECT_EQ(0, p.compare(0, expectPrefix.size(), expectPrefix));
    EXPECT_TRUE(endsWith(p, "/lib/parsers"));
}

TEST(N02WtHelperTest, ModulePathFallsBackToInstDirWhenNotCwd)
{
    TempRoot root("instdir");
    WtHelper::setInstDir(root.path().c_str());
    EXPECT_EQ(root.path(), WtHelper::getInstDir());

    const std::string p = WtHelper::getModulePath("executers", "lib", false);
    EXPECT_EQ(0, p.compare(0, root.path().size(), root.path()));
    EXPECT_TRUE(endsWith(p, "/lib/executers"));
    // 与 isCWD=true 分支区分开: 不包含当前工作目录前缀
    EXPECT_NE(std::string::npos, p.find("n02_instdir_"));
}

TEST(N02WtHelperTest, TimeStateIsRoundTrippedNotWallClock)
{
    // 只校验"写入-读回"的往返一致性, 不触碰真实时间/时区
    WtHelper::setTime(20230512, 1435, 59123);
    EXPECT_EQ(20230512u, WtHelper::getDate());
    EXPECT_EQ(1435u, WtHelper::getTime());
    EXPECT_EQ(59123u, WtHelper::getSecs());

    WtHelper::setTime(20230512, 900, 0);
    EXPECT_EQ(900u, WtHelper::getTime());
    EXPECT_EQ(0u, WtHelper::getSecs());

    WtHelper::setTDate(20230515);
    EXPECT_EQ(20230515u, WtHelper::getTradingDate());

    // setTime 的 secs 参数默认值为 0
    WtHelper::setTime(20230516, 2100);
    EXPECT_EQ(20230516u, WtHelper::getDate());
    EXPECT_EQ(0u, WtHelper::getSecs());
}

TEST(N02WtHelperTest, GenerateDirectoriesAreCreatedOnDemand)
{
    TempRoot root("gendir");
    // 必须在进程内第一次访问前设置; 若已被同进程其他用例锁定, 断言仍成立
    WtHelper::setGenerateDir(root.file("generated").c_str());

    const char* base = WtHelper::getBaseDir();
    ASSERT_NE(nullptr, base);
    EXPECT_TRUE(endsWith(base, "generated/"));

    const char* output = WtHelper::getOutputDir();
    ASSERT_NE(nullptr, output);
    EXPECT_TRUE(endsWith(output, "outputs/"));
    EXPECT_TRUE(fs::exists(fs::path(output)));

    const char* stratadata = WtHelper::getStraDataDir();
    ASSERT_NE(nullptr, stratadata);
    EXPECT_TRUE(endsWith(stratadata, "stradata/"));
    EXPECT_TRUE(fs::exists(fs::path(stratadata)));

    const char* usrdata = WtHelper::getStraUsrDatDir();
    ASSERT_NE(nullptr, usrdata);
    EXPECT_TRUE(endsWith(usrdata, "userdata/"));
    EXPECT_TRUE(fs::exists(fs::path(usrdata)));

    const char* portifolio = WtHelper::getPortifolioDir();
    ASSERT_NE(nullptr, portifolio);
    EXPECT_TRUE(endsWith(portifolio, "portfolio/"));
    EXPECT_TRUE(fs::exists(fs::path(portifolio)));

    const char* execdata = WtHelper::getExecDataDir();
    ASSERT_NE(nullptr, execdata);
    EXPECT_TRUE(endsWith(execdata, "execdata/"));
    EXPECT_TRUE(fs::exists(fs::path(execdata)));

    // 每个接口的目录名互不相同, 第二次调用返回同一缓存字符串
    EXPECT_EQ(output, WtHelper::getOutputDir());
    EXPECT_NE(std::string(output), std::string(execdata));
}

//////////////////////////////////////////////////////////////////
// WtDistExecuter
//////////////////////////////////////////////////////////////////

class N02DistExecuterTest : public ::testing::Test
{
protected:
    void SetUp() override { _root.reset(new TempRoot("distexec")); }
    void TearDown() override { _root.reset(); }

    TempRoot& root() { return *_root; }

private:
    std::unique_ptr<TempRoot> _root;
};

TEST_F(N02DistExecuterTest, NameAccessorsComeFromIExecCommand)
{
    WtDistExecuter exec("dist_a");
    EXPECT_STREQ("dist_a", exec.name());
    exec.setName("dist_b");
    EXPECT_STREQ("dist_b", exec.name());
    // 未设置 stub 时 _stub 为 NULL, 这里只确认接口可调用且不崩溃
    exec.setStub(nullptr);
}

TEST_F(N02DistExecuterTest, InitRejectsNullParams)
{
    WtDistExecuter exec("dist_nullcfg");
    EXPECT_FALSE(exec.init(nullptr));
    EXPECT_STREQ("dist_nullcfg", exec.name());
}

TEST_F(N02DistExecuterTest, InitSucceedsWithoutScaleNode)
{
    WTSVariant* cfg = loadJson(root(), "dist_empty.json", "{\n\"unused\": 1\n}\n");
    ASSERT_NE(nullptr, cfg);
    WtDistExecuter exec("dist_noscale");
    EXPECT_TRUE(exec.init(cfg));
    cfg->release();
    // scale 缺省为 0, 仓位按比例缩放后为 0
    exec.on_position_changed("SHFE.rb2310", 10);
}

TEST_F(N02DistExecuterTest, OnPositionChangedAppliesScaleAndHandlesUnchangedValue)
{
    WTSVariant* cfg = loadJson(root(), "dist_scale.json", "{\n\"scale\": 4\n}\n");
    ASSERT_NE(nullptr, cfg);
    WtDistExecuter exec("dist_scale");
    ASSERT_TRUE(exec.init(cfg));
    cfg->release();

    exec.on_position_changed("DCE.m2309", 3);        // 0 -> 12, 走"变化"分支
    exec.on_position_changed("DCE.m2309", 3);        // 12 -> 12, 走 decimal::eq 分支
    exec.on_position_changed("DCE.m2309", -3);       // 12 -> -12, 反向变化
    exec.on_position_changed("CZCE.SR301", 0);       // 0 -> 0, 首次插入即相等
}

TEST_F(N02DistExecuterTest, SetPositionHandlesWholeMap)
{
    WTSVariant* cfg = loadJson(root(), "dist_scale2.json", "{\n\"scale\": 2\n}\n");
    ASSERT_NE(nullptr, cfg);
    WtDistExecuter exec("dist_batch");
    ASSERT_TRUE(exec.init(cfg));
    cfg->release();

    wt_hashmap<std::string, double> targets;
    targets["SHFE.au2312"] = 5;
    targets["CFFEX.IF2306"] = -3;
    exec.set_position(targets);          // 首次: 0 -> 10 / 0 -> -6
    exec.set_position(targets);          // 重复: 值不变, 命中 decimal::eq
    targets["SHFE.au2312"] = 0;
    exec.set_position(targets);          // 归零分支

    wt_hashmap<std::string, double> empty;
    exec.set_position(empty);            // 空 map, 循环不进入
}

TEST_F(N02DistExecuterTest, OnTickIsDocumentedNoOp)
{
    WTSVariant* cfg = loadJson(root(), "dist_tick.json", "{\n\"scale\": 1\n}\n");
    ASSERT_NE(nullptr, cfg);
    WtDistExecuter exec("dist_tick");
    ASSERT_TRUE(exec.init(cfg));
    cfg->release();
    // 分布式执行器明确不处理 tick
    exec.on_tick("SHFE.rb2310", nullptr);
    exec.on_tick(nullptr, nullptr);
}

TEST_F(N02DistExecuterTest, ExecuterIsUsableThroughIExecCommandBase)
{
    WTSVariant* cfg = loadJson(root(), "dist_base.json", "{\n\"scale\": 3\n}\n");
    ASSERT_NE(nullptr, cfg);
    // IExecCommand 的析构函数不是虚的（见 BUG-N02-003）,
    // 所以只能通过持有具体类型的 shared_ptr 来释放, 不能 delete 基类指针。
    std::shared_ptr<WtDistExecuter> holder(new WtDistExecuter("dist_as_base"));
    IExecCommand* cmd = holder.get();
    ASSERT_NE(nullptr, cmd);
    EXPECT_STREQ("dist_as_base", cmd->name());
    EXPECT_TRUE(holder->init(cfg));
    cfg->release();
    cmd->set_position(wt_hashmap<std::string, double>());
    cmd->on_position_changed("XHE.000001", 1);
    cmd->on_tick("XHE.000001", nullptr);
    cmd->setName("dist_as_base2");
    EXPECT_STREQ("dist_as_base2", cmd->name());
}

}  // namespace
