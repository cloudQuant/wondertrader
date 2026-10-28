/**
 * r101_event_notifier_loader_test.cpp — lane R101 / wave18
 *
 * 目标：src/WtCore/EventNotifier.cpp 的模块兼容性错误分支。
 *
 * 覆盖行段：:78-84（库可 dlopen 但不导出 create_server）
 *
 * 做法：在系统临时目录放一个 libWtMsgQue 符号链接，指向构建目录里
 * 确实存在但并不导出 create_server 的 libWtDataStorage；init 成功 dlopen
 * 后 get_symbol 失败，走兼容性拒绝分支。不起 worker、不建端口。
 */
#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WtCore/EventNotifier.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

class R101EventNotifierLoader : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
};

TEST_F(R101EventNotifierLoader, IncompatibleModuleRejected)
{
    const std::string wrapped = DLLHelper::wrap_module("WtMsgQue", "lib");
    if (fs::exists(fs::path(WtHelper::getCWD()) / wrapped))
        GTEST_SKIP() << "cwd contains a real WtMsgQue; cannot force incompatibility";

    const std::string src = r71::bin_dir() + "/" + DLLHelper::wrap_module("WtDataStorage", "lib");
    if (!fs::exists(src))
        GTEST_SKIP() << "libWtDataStorage not found";

    const std::string dir = r71::temp_root() + "r101_bad_mq/";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::create_symlink(src, dir + wrapped, ec);
    ASSERT_FALSE(ec);

    const std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir(dir.c_str());

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"active\":true,\"url\":\"inproc://r101_bad_mq\"}");
    ASSERT_NE(cfg, nullptr);

    EventNotifier notifier;
    EXPECT_FALSE(notifier.init(cfg));
    cfg->release();

    WtHelper::setInstDir(oldInst.c_str());
}

}  // namespace
