// Standalone focused test; its module directory must contain the selected fake.
#include "WtBtCore/EventNotifier.h"
#include "WtBtCore/WtHelper.h"
#include "Includes/WTSVariant.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

static bool check(bool result, const char* message)
{
    if (!result) std::cerr << "FAIL: " << message << '\n';
    return result;
}
int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    const std::string mode = argv[1];
    const char* logPath = std::getenv("WT_R20_MQ_LOG");
    if (!logPath || !*logPath) return 2;
    { std::ofstream clear(logPath, std::ios::trunc); if (!clear) return 2; }
    WtHelper::setInstDir((std::string(argv[2]) + "/").c_str());
    wtp::WTSVariant* cfg = wtp::WTSVariant::createObject();
    cfg->append("active", true);
    cfg->append("url", "fake://r20");
    bool ok = true;
    {
        wtp::EventNotifier notifier;
        if (mode == "missing" || mode == "zero" || mode == "retry" || mode == "retry-zero")
        {
            ok &= check(!notifier.init(cfg), "invalid module must fail");
            notifier.notifyEvent("old");
            if (mode == "retry" || mode == "retry-zero")
            {
                if (argc < 4) return 2;
                WtHelper::setInstDir((std::string(argv[3]) + "/").c_str());
                ok &= check(notifier.init(cfg), "valid module must succeed after failure");
                notifier.notifyEvent("12345");
            }
        }
        else
        {
            ok &= check(notifier.init(cfg), "valid module must initialize");
            if (mode != "full-single" && mode != "no-publish-single")
                ok &= check(!notifier.init(cfg), "duplicate init must not create another server");
            notifier.notifyEvent("12345");
            notifier.notifyData("X", nullptr, 0);
        }
    }
    cfg->release();
    std::ifstream stream(logPath);
    const std::string log{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::string expected;
    if (mode == "zero") expected = "register 0 0\ncreate 0 0\n";
    else if (mode == "full" || mode == "full-single" || mode == "retry" || mode == "retry-zero")
        expected = (mode == "retry-zero" ? std::string("register 0 0\ncreate 0 0\n") : std::string()) +
            "register 0 0\ncreate 0 0\npublish 4045620583 5\n" +
            ((mode == "full" || mode == "full-single") ? std::string("publish 4045620583 0\n") : std::string()) +
            "destroy 4045620583 0\n";
    else if (mode == "no-publish" || mode == "no-publish-single")
        expected = "register 0 0\ncreate 0 0\ndestroy 4045620583 0\n";
    ok &= check(log == expected, ("unexpected call log: " + log).c_str());
    if (ok) std::cout << "PASS " << mode << '\n';
    return ok ? 0 : 1;
}
