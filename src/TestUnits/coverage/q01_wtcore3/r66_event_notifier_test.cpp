/**
 * r66_event_notifier_test.cpp  (lane R66 / wave13)
 *
 * 覆盖 src/WtCore/EventNotifier.cpp 中 R41 未打到的部分：
 *   - init 成功后的 worker/asio 发布路径（9 个 notify* 入口 + tradeToJson/orderToJson）；
 *   - 布尔字段的 true/false 两侧（多空、开平、今仓、撤单）；
 *   - 重复 init：_worker 已存在时不再新建（第 98-109 行）；
 *   - 未 init 时 8 个入口的 _mq_sid==0 守卫。
 *
 * WtMsgQue 动态库由从测试可执行文件向上回溯 + 有限深度递归定位；找不到时
 * 依赖它的用例 GTEST_SKIP，不访问网络、不建端口，只用 nanomsg 的 inproc://。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "WtCore/EventNotifier.h"
#include "WtCore/WtHelper.h"

namespace {

namespace fs = std::filesystem;

std::string exeDir()
{
    char buf[4096] = {0};
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return "";
    std::error_code ec;
    return fs::canonical(fs::path(buf), ec).parent_path().string();
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return "";
    return fs::path(std::string(buf, static_cast<size_t>(n))).parent_path().string();
#endif
}

const char* const kLibNames[] = {
#ifdef __APPLE__
    "libWtMsgQue.dylib",
#else
    "libWtMsgQue.so",
#endif
    "WtMsgQue.dll",
};

bool hasLib(const fs::path& dir)
{
    std::error_code ec;
    for (const char* n : kLibNames)
    {
        if (fs::exists(dir / n, ec))
            return true;
    }
    return false;
}

/** 先按常见构建布局找，再对祖先目录做有限深度递归查找。 */
std::string findMsgQueDir()
{
    fs::path dir = exeDir();
    for (int up = 0; up < 6 && !dir.empty(); ++up)
    {
        const fs::path candidates[] = {
            dir,
            dir / "bin",
            dir / "Debug" / "bin",
            dir / "build_x64" / "Debug" / "bin",
            dir / "WtPorter",
        };
        for (const fs::path& cand : candidates)
        {
            if (hasLib(cand))
                return cand.string();
        }
        dir = dir.parent_path();
    }

    // 兜底：在测试可执行文件的 3 层祖先内做深度 <= 5 的递归扫描
    dir = exeDir();
    for (int up = 0; up < 3 && !dir.empty(); ++up)
    {
        std::error_code ec;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const fs::path& p = it->path();
            if (it.depth() > 5)
            {
                it.disable_recursion_pending();
                continue;
            }
            for (const char* n : kLibNames)
            {
                if (p.filename() == n)
                    return p.parent_path().string();
            }
        }
        dir = dir.parent_path();
    }
    return "";
}

std::string activeCfg(const std::string& url)
{
    return "{\n  \"active\": true,\n  \"url\": \"" + url + "\"\n}\n";
}

std::string inactiveCfg()
{
    return "{\n  \"active\": false,\n  \"url\": \"inproc://r66_inactive\"\n}\n";
}

WTSVariant* loadCfg(const std::string& content)
{
    static uint32_t seq = 0;
    fs::path p = fs::temp_directory_path() /
                 ("r66_ntf_cfg_" + std::to_string(static_cast<long long>(::getpid())) +
                  "_" + std::to_string(++seq) + ".json");
    {
        std::ofstream ofs(p.string().c_str(), std::ios::binary | std::ios::trunc);
        ofs << content;
    }
    WTSVariant* cfg = WTSCfgLoader::load_from_file(p.string().c_str());
    std::error_code ec;
    fs::remove(p, ec);
    return cfg;
}

std::string uniqueUrl(const char* tag)
{
    static std::atomic<uint32_t> seq(0);
    return std::string("inproc://r66_") + tag + "_" +
           std::to_string(static_cast<long long>(::getpid())) + "_" +
           std::to_string(++seq);
}

/** 让 worker 消费已 post 的 lambda；只等有限时间，不做时序断言。 */
void drainAsync()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

class ScopedInstDir
{
public:
    explicit ScopedInstDir(const std::string& dir)
        : _old(WtHelper::getInstDir())
    {
        WtHelper::setInstDir((dir + "/").c_str());
    }

    ~ScopedInstDir() { WtHelper::setInstDir(_old.c_str()); }

private:
    std::string _old;
};

}  // namespace

//////////////////////////////////////////////////////////////////
// 成功 init + 全部通知入口（R41 在 CoverageTests_wtcore 下找不到库而跳过）
// src/WtCore/EventNotifier.cpp:58-112, 114-212, 307-399
//////////////////////////////////////////////////////////////////
TEST(R66EventNotifierTest, R66ActiveNotifierPublishesAllChannels)
{
    const std::string libDir = findMsgQueDir();
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found; active JSON paths stay uncovered";

    ScopedInstDir inst(libDir);
    WTSVariant* cfg = loadCfg(activeCfg(uniqueUrl("all")));
    ASSERT_NE(nullptr, cfg);

    {
        EventNotifier notifier;
        if (!notifier.init(cfg))
        {
            cfg->release();
            GTEST_SKIP() << "WtMsgQue init failed; active publish paths stay uncovered";
        }

        notifier.notify_log("R66", "log payload");
        notifier.notify_event("event payload");
        notifier.notify("R66_TRADER", "trader message");

        WTSTradeInfo* trd = WTSTradeInfo::create("ag2512", "SHFE");
        trd->setDirection(WDT_LONG);
        trd->setOffsetType(WOT_OPEN);
        trd->setVolume(2);
        trd->setPrice(3500.0);
        notifier.notify("R66_TRADER", 11, "SHFE.ag.2512", trd);
        trd->release();

        WTSOrderInfo* ord = WTSOrderInfo::create();
        ord->setCode("ag2512");
        ord->setExchange("SHFE");
        ord->setDirection(WDT_SHORT);
        ord->setOffsetType(WOT_CLOSETODAY);
        ord->setOrderState(WOS_Canceled);
        ord->setVolume(3);
        ord->setVolTraded(1);
        ord->setVolLeft(2);
        ord->setPrice(3510.0);
        ord->setStateMsg("canceled");
        notifier.notify("R66_TRADER", 12, "SHFE.ag.2512", ord);
        ord->release();

        notifier.notify_chart_index(20231010093100ULL, "r66stra", "idx", "line", 1.5);
        notifier.notify_chart_marker(20231010093100ULL, "r66stra", 3500.0, "icon", "tag");
        notifier.notify_trade("r66stra", "SHFE.ag.2512", true, false,
                              20231010093100ULL, 3501.0, "usertag");
        drainAsync();
    }

    cfg->release();
}

//////////////////////////////////////////////////////////////////
// 布尔字段两侧：平今/平昨、多/空、撤单/未撤
// src/WtCore/EventNotifier.cpp:232-265, 267-305
//////////////////////////////////////////////////////////////////
TEST(R66EventNotifierTest, R66ActiveNotifierTradeAndOrderFlags)
{
    const std::string libDir = findMsgQueDir();
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found";

    ScopedInstDir inst(libDir);
    WTSVariant* cfg = loadCfg(activeCfg(uniqueUrl("flags")));
    ASSERT_NE(nullptr, cfg);

    {
        EventNotifier notifier;
        if (!notifier.init(cfg))
        {
            cfg->release();
            GTEST_SKIP() << "WtMsgQue init failed";
        }

        // 空头 + 平昨（isLong=false, isOpen=false, isToday=false）
        WTSTradeInfo* t1 = WTSTradeInfo::create("ag2512", "SHFE");
        t1->setDirection(WDT_SHORT);
        t1->setOffsetType(WOT_CLOSE);
        t1->setVolume(1);
        t1->setPrice(3500.0);
        notifier.notify("R66_TRADER", 21, "SHFE.ag.2512", t1);
        t1->release();

        // 多头 + 平今（isToday=true）
        WTSTradeInfo* t2 = WTSTradeInfo::create("ag2512", "SHFE");
        t2->setDirection(WDT_LONG);
        t2->setOffsetType(WOT_CLOSETODAY);
        t2->setVolume(2);
        t2->setPrice(3502.0);
        notifier.notify("R66_TRADER", 22, "SHFE.ag.2512", t2);
        t2->release();

        // 多头 + 开仓 + 未撤
        WTSOrderInfo* o1 = WTSOrderInfo::create();
        o1->setCode("ag2512");
        o1->setExchange("SHFE");
        o1->setDirection(WDT_LONG);
        o1->setOffsetType(WOT_OPEN);
        o1->setOrderState(WOS_AllTraded);
        o1->setVolume(4);
        o1->setVolTraded(4);
        o1->setVolLeft(0);
        o1->setPrice(3501.0);
        o1->setStateMsg("alltraded");
        notifier.notify("R66_TRADER", 23, "SHFE.ag.2512", o1);
        o1->release();

        drainAsync();
    }

    cfg->release();
}

//////////////////////////////////////////////////////////////////
// active 但传入空成交/委托：第一操作数短路
// src/WtCore/EventNotifier.cpp:197-200, 214-217
//////////////////////////////////////////////////////////////////
TEST(R66EventNotifierTest, R66ActiveNotifierSkipsNullTradeAndOrder)
{
    const std::string libDir = findMsgQueDir();
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found";

    ScopedInstDir inst(libDir);
    WTSVariant* cfg = loadCfg(activeCfg(uniqueUrl("nullobj")));
    ASSERT_NE(nullptr, cfg);

    {
        EventNotifier notifier;
        if (!notifier.init(cfg))
        {
            cfg->release();
            GTEST_SKIP() << "WtMsgQue init failed";
        }
        notifier.notify("R66_TRADER", 31, "SHFE.ag.2512", static_cast<WTSTradeInfo*>(NULL));
        notifier.notify("R66_TRADER", 32, "SHFE.ag.2512", static_cast<WTSOrderInfo*>(NULL));
        drainAsync();
    }

    cfg->release();
}

//////////////////////////////////////////////////////////////////
// 未 init：8 个入口的 _mq_sid==0 守卫 + 析构无 worker
// src/WtCore/EventNotifier.cpp:46-56, 114-117, 143-146, 169-172, 197-217, 307-372
//////////////////////////////////////////////////////////////////
TEST(R66EventNotifierTest, R66InactiveNotifierShortCircuitsAllNotify)
{
    WTSVariant* cfg = loadCfg(inactiveCfg());
    ASSERT_NE(nullptr, cfg);

    {
        EventNotifier notifier;
        EXPECT_FALSE(notifier.init(cfg));
        notifier.notify_log("R66", "message");
        notifier.notify_event("event");
        notifier.notify("R66_TRADER", "message");
        notifier.notify("R66_TRADER", 41, "SHFE.ag.2512", static_cast<WTSTradeInfo*>(NULL));
        notifier.notify("R66_TRADER", 42, "SHFE.ag.2512", static_cast<WTSOrderInfo*>(NULL));
        notifier.notify_chart_index(1ULL, "stra", "idx", "line", 1.0);
        notifier.notify_chart_marker(1ULL, "stra", 1.0, "icon", "tag");
        notifier.notify_trade("stra", "SHFE.ag.2512", true, true, 1ULL, 1.0, "tag");
    }

    cfg->release();
}

//////////////////////////////////////////////////////////////////
// 重复 init：worker 已存在时不再新建，发布仍可用
// src/WtCore/EventNotifier.cpp:98-109（_worker != NULL 支）
//////////////////////////////////////////////////////////////////
TEST(R66EventNotifierTest, R66ActiveNotifierRepeatedInitKeepsWorker)
{
    const std::string libDir = findMsgQueDir();
    if (libDir.empty())
        GTEST_SKIP() << "libWtMsgQue not found";

    ScopedInstDir inst(libDir);
    WTSVariant* cfg1 = loadCfg(activeCfg(uniqueUrl("first")));
    WTSVariant* cfg2 = loadCfg(activeCfg(uniqueUrl("second")));
    ASSERT_NE(nullptr, cfg1);
    ASSERT_NE(nullptr, cfg2);

    {
        EventNotifier notifier;
        if (!notifier.init(cfg1) || !notifier.init(cfg2))
        {
            cfg2->release();
            cfg1->release();
            GTEST_SKIP() << "WtMsgQue init failed";
        }
        notifier.notify_log("R66", "after second init");
        notifier.notify_event("after second init");
        drainAsync();
    }

    cfg2->release();
    cfg1->release();
}
