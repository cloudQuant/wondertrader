// Standalone focused test; its module directory must contain the selected fake.
#include "WtBtCore/EventNotifier.h"
#include "WtBtCore/WtHelper.h"
#include "Includes/WTSVariant.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

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
        else if (mode == "release")
        {
            // R20_wtbtcore#3 / card 9: explicit idempotent release destroys the
            // server exactly once; later notifications and init are rejected
            // and the destructor does not destroy again.
            ok &= check(notifier.init(cfg), "valid module must initialize");
            notifier.notifyEvent("12345");
            notifier.release();
            notifier.notifyEvent("after");
            notifier.notifyData("X", nullptr, 0);
            notifier.release();
            ok &= check(!notifier.init(cfg), "init after release must be rejected");
        }
        else if (mode == "release-before-init")
        {
            notifier.release();
            ok &= check(!notifier.init(cfg), "init after release must be rejected");
            notifier.notifyEvent("after");
        }
        else if (mode == "concurrent-release")
        {
            // Publications are in flight (the fake sleeps inside publish) while
            // another thread releases; release must drain them before destroy.
            ok &= check(notifier.init(cfg), "valid module must initialize");
            std::atomic<bool> stop{false};
            std::atomic<unsigned> started{0};
            std::vector<std::thread> publishers;
            for (int i = 0; i < 4; ++i)
                publishers.emplace_back([&notifier, &stop, &started]() {
                    started.fetch_add(1);
                    while (!stop.load())
                        notifier.notifyEvent("12345");
                });
            while (started.load() < 4) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            notifier.release();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stop.store(true);
            for (std::thread& t : publishers) t.join();
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
    else if (mode == "release")
        expected = "register 0 0\ncreate 0 0\npublish 4045620583 5\ndestroy 4045620583 0\n";
    if (mode == "concurrent-release")
    {
        // register, create, >=1 publish, exactly one destroy as the last line,
        // and no protocol-violation lines from the fake.
        const std::string head = "register 0 0\ncreate 0 0\n";
        const std::string tail = "destroy 4045620583 0\n";
        std::size_t publishes = 0, destroys = 0, pos = 0;
        while ((pos = log.find("publish 4045620583 5\n", pos)) != std::string::npos) { ++publishes; ++pos; }
        pos = 0;
        while ((pos = log.find("destroy", pos)) != std::string::npos) { ++destroys; ++pos; }
        ok &= check(log.compare(0, head.size(), head) == 0, ("unexpected head: " + log).c_str());
        ok &= check(publishes >= 1, "no publication observed before release");
        ok &= check(destroys == 1, ("destroy count must be exactly one: " + log).c_str());
        ok &= check(log.size() >= tail.size() && log.compare(log.size() - tail.size(), tail.size(), tail) == 0,
            "destroy must be the last call");
        ok &= check(log.find("after-destroy") == std::string::npos && log.find("while-publishing") == std::string::npos,
            ("publication overlapped destroy: " + log).c_str());
        if (ok) std::cout << "publications=" << publishes << '\n';
    }
    else
        ok &= check(log == expected, ("unexpected call log: " + log).c_str());
    if (ok) std::cout << "PASS " << mode << '\n';
    return ok ? 0 : 1;
}
