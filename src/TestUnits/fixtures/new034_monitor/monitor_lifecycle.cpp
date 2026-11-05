#include "WtRiskMonFact/WtSimpRiskMon.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSRiskDef.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

const char* FACT_NAME = "fixture";
using namespace wtp;
using namespace std::chrono_literals;

struct Context final : WtPortContext {
    WtSimpleRiskMon* mon = nullptr;
    WTSPortFundInfo* fund = WTSPortFundInfo::create();
    std::atomic<bool> entered{false}, proceed{false}, self_returned{false};
    bool self_stop = false;
    ~Context() { fund->release(); }
    WTSPortFundInfo* getFundInfo() override { return fund; }
    void setVolScale(double) override {}
    bool isInTrading() override {
        entered.store(true);
        while (!proceed.load()) std::this_thread::yield();
        if (self_stop) { mon->stop(); self_returned.store(true); }
        return true;
    }
    void writeRiskLog(const char*) override {}
    uint32_t getCurDate() override { return 20261102; }
    uint32_t getCurTime() override { return 1000; }
    uint32_t getTradingDate() override { return 20261102; }
    uint32_t transTimeToMin(uint32_t t) override { return t; }
};

bool await(const std::atomic<bool>& flag) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!flag.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    return flag.load();
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Context ctx;
    WtSimpleRiskMon mon;
    ctx.mon = &mon;
    auto cfg = WTSVariant::createObject();
    mon.init(&ctx, cfg);
    cfg->release();
    if (!std::strcmp(argv[1], "before_start")) {
        mon.stop(); mon.stop(); mon.run(); mon.stop();
        if (ctx.entered.load()) return 3;
    } else if (!std::strcmp(argv[1], "repeat")) {
        ctx.proceed.store(true); mon.run();
        if (!await(ctx.entered)) return 4;
        mon.stop(); mon.stop();
    } else if (!std::strcmp(argv[1], "self_alone")) {
        ctx.self_stop = true;
        ctx.proceed.store(true);
        mon.run();
        if (!await(ctx.self_returned)) return 7;
        mon.stop();
    } else if (!std::strcmp(argv[1], "concurrent") || !std::strcmp(argv[1], "self")) {
        ctx.self_stop = !std::strcmp(argv[1], "self");
        mon.run();
        if (!await(ctx.entered)) return 5;
        std::atomic<int> started{0}, completed{0};
        std::vector<std::thread> callers;
        for (int i = 0; i != 4; ++i) callers.emplace_back([&] {
            started.fetch_add(1); mon.stop(); completed.fetch_add(1);
        });
        while (started.load() != 4) std::this_thread::yield();
        std::this_thread::sleep_for(20ms);
        bool early = completed.load() != 0;
        ctx.proceed.store(true);
        for (auto& t : callers) t.join();
        if (early || completed.load() != 4 || (ctx.self_stop && !ctx.self_returned.load())) return 6;
        mon.stop();
    } else return 2;
    std::cout << argv[1] << " PASS\n";
}
