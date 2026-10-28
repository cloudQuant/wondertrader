/**
 * r110_trader_init_test.cpp — lane R110 / wave19
 *
 * 目标：src/WtCore/TraderAdapter.cpp 的 init() 成功链（此前 R78/R90 只覆盖
 * 模块装载失败 / createTrader 缺失 / 重复 init）。
 *
 * 覆盖行段：:99-207（模块装载、createTrader、deleteTrader、api->init 成功）
 *
 * 接缝：构建产物 libTraderMocker.dylib 软链到临时模块根的 traders/ 下；
 * TraderMocker::init 只在当前目录创建 mocker_<id>/ 并构造 UDP endpoint，
 * 不建 socket、不连柜台。用例期间把 cwd 切到临时目录，结束后恢复并整棵删除。
 */
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <filesystem>
#include <string>

#include "r90_env.h"
#include "../p02_wtcore_engines2/r42_core_fakes.h"

#include <WtCore/TraderAdapter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <Includes/WTSVariant.hpp>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

class R110CwdGuard
{
public:
    explicit R110CwdGuard(const fs::path& dir)
    {
        std::error_code ec;
        _old = fs::current_path(ec);
        fs::current_path(dir, ec);
        _ok = !ec;
    }

    ~R110CwdGuard()
    {
        if (_old.empty())
            return;
        std::error_code ec;
        fs::current_path(_old, ec);
    }

    bool ok() const { return _ok; }

private:
    fs::path _old;
    bool _ok = false;
};

TEST(R110TraderInit, LoadsTraderModuleAndCreatesApi)
{
    const std::string bin = r90::find_bin_dir();
    if (bin.empty())
        GTEST_SKIP() << "build bin dir not found";

    const fs::path root = fs::path(r90::temp_root()) / "r110_trader_module";
    std::error_code ec;
    fs::create_directories(root / "traders", ec);
    ASSERT_FALSE(ec);

    const fs::path src = fs::path(bin) / "libTraderMocker.dylib";
    if (!fs::exists(src, ec))
        GTEST_SKIP() << "libTraderMocker.dylib not found";

    void* h = dlopen(src.c_str(), RTLD_NOW);
    if (h == nullptr)
        GTEST_SKIP() << "libTraderMocker cannot be dlopened";
    const bool symbols = dlsym(h, "createTrader") != nullptr
        && dlsym(h, "deleteTrader") != nullptr;
    dlclose(h);
    if (!symbols)
        GTEST_SKIP() << "libTraderMocker lacks trader entry symbols";

    fs::remove(root / "traders" / "libTraderMocker.dylib", ec);
    fs::create_symlink(src, root / "traders" / "libTraderMocker.dylib", ec);
    ASSERT_FALSE(ec);

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((root.string() + "/").c_str());
    WtHelper::setGenerateDir(r90::temp_root().c_str());

    // TraderMocker::init 会在 cwd 下建 mocker_9911/
    R110CwdGuard cwd(root);
    ASSERT_TRUE(cwd.ok());

    r42fakes::R42BaseDataMgr bd;
    bd.regisSession("TRADING");

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"savedata\":false,\"ignore_selfmatch\":true,"
        "\"span\":1000,\"newpx\":false,\"mockerid\":9911,"
        "\"maxqty\":10,\"minqty\":1,\"udp_port\":19999,"
        "\"module\":\"TraderMocker\"}");
    ASSERT_NE(nullptr, cfg);

    TraderAdapter trader;
    EXPECT_TRUE(trader.init("r110_mocker", cfg, &bd, NULL));
    EXPECT_STREQ("r110_mocker", trader.id());
    EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, trader.state());

    trader.release();
    cfg->release();

    WtHelper::setInstDir(oldInst.c_str());
}

}  // namespace
