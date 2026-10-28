/**
 * r41_event_notifier_test.cpp  (lane R41 / wave10)
 *
 * 覆盖 src/WtCore/EventNotifier.cpp:
 *   - init: active=false / 模块缺失 / 成功加载 WtMsgQue
 *   - notify_* 的 _mq_sid==0 守卫与真实 post 到 asio 后生成的 JSON
 *     (tradeToJson / orderToJson / chart marker / chart index / stra trade)
 *
 * 关于 WtMsgQue: 测试二进制所在目录的上一级就是 build 的 bin 目录, 里面通常有
 * libWtMsgQue.dylib/.so。找到才驱动真实发布路径, 找不到直接 GTEST_SKIP,
 * 不访问外网、不建端口, 使用 nanomsg 的 inproc:// 本地地址。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WtCore/EventNotifier.h"
#include "WtCore/WtHelper.h"

namespace {

namespace fs = std::filesystem;

class NtfTempDir
{
public:
    explicit NtfTempDir(const std::string& tag)
    {
        static uint32_t seq = 0;
        _path = fs::temp_directory_path() /
                ("wt_r41_ntf_" + tag + "_" + std::to_string(static_cast<long long>(::getpid())) +
                 "_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(_path, ec);
        fs::create_directories(_path, ec);
    }

    ~NtfTempDir()
    {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }

    std::string file(const std::string& name) const { return (_path / name).string(); }

private:
    fs::path _path;
};

std::string exeDir()
{
    char buf[4096] = {0};
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return "";
    std::error_code ec;
    fs::path p = fs::canonical(fs::path(buf), ec);
    return p.parent_path().string();
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return "";
    return fs::path(std::string(buf, static_cast<size_t>(n))).parent_path().string();
#endif
}

/** 在可执行文件目录及上一级里找 WtMsgQue 动态库。 */
std::string findMsgQueDir()
{
    const char* names[] = {
#ifdef __APPLE__
        "libWtMsgQue.dylib",
#else
        "libWtMsgQue.so",
#endif
        "WtMsgQue.dll",
    };

    fs::path dir = exeDir();
    for (int up = 0; up < 3 && !dir.empty(); ++up)
    {
        for (const char* n : names)
        {
            std::error_code ec;
            if (fs::exists(dir / n, ec))
                return dir.string();
        }
        dir = dir.parent_path();
    }
    return "";
}

WTSVariant* loadCfg(const NtfTempDir& tmp, const std::string& content)
{
    {
        std::ofstream ofs(tmp.file("ntf.json").c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
    }
    return WTSCfgLoader::load_from_file(tmp.file("ntf.json").c_str());
}

std::string inactiveCfg()
{
    return "{\n  \"active\": false,\n  \"url\": \"inproc://r41_inactive\"\n}\n";
}

std::string activeCfg(const std::string& url)
{
    return "{\n  \"active\": true,\n  \"url\": \"" + url + "\"\n}\n";
}

//////////////////////////////////////////////////////////////////
// active=false: init 提前返回; 所有 notify 被 _mq_sid==0 短路
// src/WtCore/EventNotifier.cpp:58-61, 114-117, 143-146, 169-172,
// 197-200, 214-217, 307-310, 338-341, 369-372
//////////////////////////////////////////////////////////////////
TEST(R41EventNotifierTest, InactiveNotifierShortCircuitsEveryNotify)
{
    NtfTempDir tmp("inactive");
    WTSVariant* cfg = loadCfg(tmp, inactiveCfg());
    ASSERT_NE(nullptr, cfg);

    EventNotifier notifier;
    EXPECT_FALSE(notifier.init(cfg));
    cfg->release();

    notifier.notify_log("TAG", "message");
    notifier.notify_event("event");
    notifier.notify("trader", "message");
    notifier.notify("trader", 1, "SHFE.rb.2310", static_cast<WTSTradeInfo*>(NULL));
    notifier.notify("trader", 2, "SHFE.rb.2310", static_cast<WTSOrderInfo*>(NULL));
    notifier.notify_chart_index(20231010093100ULL, "stra", "idx", "line", 1.0);
    notifier.notify_chart_marker(20231010093100ULL, "stra", 3500.0, "icon", "tag");
    notifier.notify_trade("stra", "SHFE.rb.2310", true, true, 20231010093100ULL, 3500.0, "tag");
}

//////////////////////////////////////////////////////////////////
// 模块缺失: 走 load_library 失败分支
// src/WtCore/EventNotifier.cpp:63-76
//////////////////////////////////////////////////////////////////
TEST(R41EventNotifierTest, MissingMqModuleFailsInit)
{
    NtfTempDir tmp("missing");

    // init 会先查 getCWD(); 如果运行目录恰好也带库, 这个分支无法确定性触发
    if (fs::exists(fs::path(WtHelper::getCWD()) / "libWtMsgQue.dylib"))
        GTEST_SKIP() << "cwd contains libWtMsgQue; cannot force the load-failure branch";

    std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((tmp.file("no_module_here") + "/").c_str());

    WTSVariant* cfg = loadCfg(tmp, activeCfg("inproc://r41_missing_module"));
    ASSERT_NE(nullptr, cfg);

    EventNotifier notifier;
    EXPECT_FALSE(notifier.init(cfg));
    cfg->release();

    WtHelper::setInstDir(oldInst.c_str());
}

//////////////////////////////////////////////////////////////////
// 成功加载 WtMsgQue: 8 个 notify 入口都 post 到 asio, 由 worker 生成 JSON
// src/WtCore/EventNotifier.cpp:78-112, 121-140, 149-166, 176-194,
// 205-211, 222-227, 315-335, 346-366, 377-399
//////////////////////////////////////////////////////////////////
TEST(R41EventNotifierTest, ActiveNotifierPublishesAllJsonPayloads)
{
    std::string libDir = findMsgQueDir();
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found next to test binary; JSON paths stay uncovered";

    NtfTempDir tmp("active");
    std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((libDir + "/").c_str());

    static std::atomic<uint32_t> seq(0);
    std::string url = "inproc://r41_ntf_" + std::to_string(static_cast<long long>(::getpid())) +
                      "_" + std::to_string(++seq);

    WTSVariant* cfg = loadCfg(tmp, activeCfg(url));
    ASSERT_NE(nullptr, cfg);

    {
        EventNotifier notifier;
        ASSERT_TRUE(notifier.init(cfg));

        notifier.notify_log("R41", "log payload");
        notifier.notify_event("event payload");
        notifier.notify("R41_TRADER", "trader message");

        WTSTradeInfo* trd = WTSTradeInfo::create("rb2310", "SHFE");
        trd->setDirection(WDT_LONG);
        trd->setOffsetType(WOT_OPEN);
        trd->setVolume(2);
        trd->setPrice(3500.0);
        notifier.notify("R41_TRADER", 11, "SHFE.rb.2310", trd);
        trd->release();

        WTSOrderInfo* ord = WTSOrderInfo::create();
        ord->setCode("rb2310");
        ord->setExchange("SHFE");
        ord->setDirection(WDT_SHORT);
        ord->setOffsetType(WOT_CLOSETODAY);
        ord->setOrderState(WOS_Canceled);
        ord->setVolume(3);
        ord->setVolTraded(1);
        ord->setVolLeft(2);
        ord->setPrice(3510.0);
        ord->setStateMsg("canceled");
        notifier.notify("R41_TRADER", 12, "SHFE.rb.2310", ord);
        ord->release();

        notifier.notify_chart_index(20231010093100ULL, "r41stra", "idx", "line", 1.5);
        notifier.notify_chart_marker(20231010093100ULL, "r41stra", 3500.0, "icon", "tag");
        notifier.notify_trade("r41stra", "SHFE.rb.2310", true, false,
                              20231010093100ULL, 3501.0, "usertag");

        // 让 asio worker 消费已 post 的 lambda; 这里不做任何时序断言。
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cfg->release();
    WtHelper::setInstDir(oldInst.c_str());
}

//////////////////////////////////////////////////////////////////
// active=false + 非空成交/委托: _mq_sid==0 守卫的后半段
// src/WtCore/EventNotifier.cpp:197-200, 214-217
//////////////////////////////////////////////////////////////////
TEST(R41EventNotifierTest, InactiveNotifierRejectsNonNullTradeAndOrder)
{
    NtfTempDir tmp("inactive_obj");
    WTSVariant* cfg = loadCfg(tmp, inactiveCfg());
    ASSERT_NE(nullptr, cfg);

    EventNotifier notifier;
    EXPECT_FALSE(notifier.init(cfg));
    cfg->release();

    WTSTradeInfo* trd = WTSTradeInfo::create("rb2310", "SHFE");
    WTSOrderInfo* ord = WTSOrderInfo::create();
    notifier.notify("R41_TRADER", 1, "SHFE.rb.2310", trd);
    notifier.notify("R41_TRADER", 2, "SHFE.rb.2310", ord);
    trd->release();
    ord->release();
}

//////////////////////////////////////////////////////////////////
// init 成功但传入空成交/委托: 第一操作数短路
// src/WtCore/EventNotifier.cpp:197-200, 214-217
//////////////////////////////////////////////////////////////////
TEST(R41EventNotifierTest, ActiveNotifierSkipsNullTradeAndOrder)
{
    std::string libDir = findMsgQueDir();
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found next to test binary";

    NtfTempDir tmp("active_null");
    std::string oldInst = WtHelper::getInstDir();
    WtHelper::setInstDir((libDir + "/").c_str());

    static std::atomic<uint32_t> seq(0);
    std::string url = "inproc://r41_ntf_null_" +
                      std::to_string(static_cast<long long>(::getpid())) + "_" +
                      std::to_string(++seq);

    WTSVariant* cfg = loadCfg(tmp, activeCfg(url));
    ASSERT_NE(nullptr, cfg);

    {
        EventNotifier notifier;
        ASSERT_TRUE(notifier.init(cfg));
        notifier.notify("R41_TRADER", 1, "SHFE.rb.2310", static_cast<WTSTradeInfo*>(NULL));
        notifier.notify("R41_TRADER", 2, "SHFE.rb.2310", static_cast<WTSOrderInfo*>(NULL));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    cfg->release();
    WtHelper::setInstDir(oldInst.c_str());
}

}  // namespace
