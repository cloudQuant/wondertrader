#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "Includes/WTSVariant.hpp"
#include "ParserCTPMini/ParserCTPMini.h"
#include "TestUnits/coverage/t_ParserCTPMini/r105_ctpmini_api_stub.h"

namespace {
std::string scenario;
int opens = 0, symbols = 0, creators = 0, closes = 0;
R105CtpMiniMdApiStub api;
int failedModule, healthyModule;
std::string lastPath;
bool failedModuleUnloaded = false;
CThostFtdcMdApi* createApi(const char*, bool, bool) {
    ++creators;
    return scenario == "null-api" ? nullptr : &api;
}
struct Sink : wtp::IParserSpi {
    std::vector<std::string> logs;
    void handleSymbolList(const WTSArray*) override {}
    void handleQuote(WTSTickData*, uint32_t) override {}
    void handleParserLog(WTSLogLevel, const char* s) override { logs.emplace_back(s ? s : ""); }
    IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
    bool has(const std::string& needle) const {
        for (const auto& s : logs) if (s.find(needle) != std::string::npos) return true;
        return false;
    }
};
}
extern "C" int wt_mini_init_dladdr(const void*, Dl_info* info) {
    *info = {}; info->dli_fname = "/tmp/mini-init-fixture/adapter.dylib"; return 1;
}
extern "C" void* wt_mini_init_dlopen(const char* path, int) {
    ++opens; lastPath = path ? path : "";
    if (scenario == "missing-library") return nullptr;
    return scenario == "success" || scenario == "success-one" ? &healthyModule : &failedModule;
}
extern "C" void* wt_mini_init_dlsym(void* handle, const char* name) {
    ++symbols;
    if ((handle != &failedModule && handle != &healthyModule) || !name || std::string(name) != "_ZN15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb") return nullptr;
    return scenario == "missing-symbol" || scenario == "cross-instance" ? nullptr : reinterpret_cast<void*>(&createApi);
}
extern "C" int wt_mini_init_dlclose(void* handle) {
    if (handle != &failedModule && handle != &healthyModule) std::abort();
    if (handle == &failedModule) failedModuleUnloaded = true;
    ++closes; return 0;
}
extern "C" char* wt_mini_init_dlerror() {
    static char error[] = "injected missing module"; return error;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    scenario = argv[1];
    const bool success = scenario == "success" || scenario == "success-one";
    const bool k53Retry = scenario == "retry-after-missing-library" || scenario == "retry-after-null-config";
    if (!success && !k53Retry && scenario != "missing-library" && scenario != "missing-symbol" && scenario != "null-api" && scenario != "null-config" && scenario != "cross-instance") return 2;
    Sink sink;
    ParserCTPMini parser;
    parser.registerSpi(&sink);
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("front", "tcp://127.0.0.1:1"); cfg->append("broker", "9999");
    cfg->append("user", "test"); cfg->append("pass", "unused"); cfg->append("flowdir", "flow");
    if (k53Retry) {
        // Decision K53 (2026-11-06, I03-NEW-121 sub-item): a failure that loaded no module
        // (missing library, null configuration) ran no vendor code, so the same instance may
        // retry and succeed. After that successful init, card 9 9c=A applies as usual.
        const bool nullCfg = scenario == "retry-after-null-config";
        scenario = nullCfg ? "null-config" : "missing-library";
        if (parser.init(nullCfg ? nullptr : cfg)) return 51;
        const int opensAfterFailure = opens;
        if (opensAfterFailure != (nullCfg ? 0 : 1) || symbols != 0 || creators != 0 || closes != 0) return 52;
        scenario = "success";
        WTSVariant* good = WTSVariant::createObject();
        good->append("front", "tcp://127.0.0.1:1"); good->append("broker", "9999");
        good->append("user", "test"); good->append("pass", "unused"); good->append("flowdir", "flow");
        good->append("ctpmodule", "distinct_healthy_module");
        if (!parser.init(good) || !parser.isConnected()) {
            std::cerr << "K53: the same instance must be able to retry after a no-module failure\n";
            return 53;
        }
        if (opens != opensAfterFailure + 1 || symbols != 1 || creators != 1) return 54;
        if (parser.init(good) || opens != opensAfterFailure + 1) return 55;	// 9c after success
        parser.release(); parser.release();
        if (api.release_calls != 1 || closes != 0) return 56;
        good->release();
        cfg->release();
        std::cout << "PASS " << argv[1] << " opens=" << opens << " creators=" << creators << "\n";
        return 0;
    }
    if (scenario == "cross-instance") {
        // Decision K16=A (2026-11-04, I03-NEW-003): after one instance failed on a
        // module, another instance may load the same module again; that failure is
        // isolated like the first one (module retained, never closed) and neither
        // instance retries on its own quarantined module.
        if (parser.init(cfg)) return 41;
        if (opens != 1 || symbols != 1 || closes != 0) return 43;
        ParserCTPMini other;
        if (other.init(cfg)) return 42;
        if (opens != 2) {
            std::cerr << "K16=A: the second instance must make its own load attempt (opens=" << opens << ")\n";
            return 40;
        }
        if (symbols != 2 || creators != 0 || closes != 0 || failedModuleUnloaded) return 44;
        if (!sink.has("creator symbol")) return 45;
        if (parser.init(cfg) || other.init(cfg)) return 46;	// both stay disabled
        if (opens != 2 || closes != 0) return 47;
        if (parser.connect() || other.connect()) return 48;
        parser.release(); other.release();
        if (closes != 0 || failedModuleUnloaded) return 49;
        cfg->release();
        return 0;
    }
    const bool result = parser.init(scenario == "null-config" ? nullptr : cfg);
    if (!success) {
        const int expectedOpens = scenario == "null-config" ? 0 : 1;
        const int expectedSymbols = scenario == "missing-library" || scenario == "null-config" ? 0 : 1;
        const int expectedCreators = scenario == "null-api" ? 1 : 0;
        const bool loadedFailure = scenario == "missing-symbol" || scenario == "null-api";
        const char* expectedLog = scenario == "null-config" ? "without configuration" : scenario == "missing-library" ? "Failed to load" : scenario == "missing-symbol" ? "creator symbol" : "creator returned null";
        if (result || parser.isConnected() || opens != expectedOpens || symbols != expectedSymbols || creators != expectedCreators || closes != 0 || !sink.has(expectedLog)) return 10;
        if (parser.connect()) return 16;
        const std::string failedPath = lastPath;
        WTSVariant* good = WTSVariant::createObject();
        good->append("front", "tcp://127.0.0.1:1"); good->append("broker", "9999");
        good->append("user", "test"); good->append("pass", "unused"); good->append("flowdir", "flow");
        good->append("ctpmodule", "distinct_healthy_module");
        if (loadedFailure) {
            // Card 9 9c=A (I03-NEW-121): a failed instance that retained a module is
            // fail-closed: a second init, even for a different target, is rejected
            // before any load attempt.
            if (parser.init(good) || opens != expectedOpens || !sink.has("initialization disabled")) return 12;
        }
        parser.release(); parser.release();
        if (parser.connect()) return 17;
        // Model a late callback whose code would have been unmapped by close.
        if (closes != 0 || (loadedFailure && failedModuleUnloaded)) return 11;
        scenario = "success";
        // Card 9 9c=A (I03-NEW-121): after the failure and release the same instance
        // stays closed: init is rejected and makes no load attempt, in every mode.
        if (parser.init(good) || parser.isConnected() || opens != expectedOpens) return 14;
        {
            // A new instance may retry (here with a distinct healthy module).
            ParserCTPMini other;
            if (!other.init(good) || !other.isConnected() || opens != expectedOpens + 1 || lastPath == failedPath) return 13;
            other.release(); other.release();
        }
        good->release();
        if (api.release_calls != 1 || closes != 0 || (loadedFailure && failedModuleUnloaded)) return 15;
    } else {
        if (!result || !parser.isConnected() || opens != 1 || symbols != 1 || creators != 1 || closes != 0 || api.spi_regs != 1 || api.front_regs != 1) return 20;
        if (scenario == "success" && (parser.init(cfg) || opens != 1 || creators != 1)) return 21;
        if (!parser.connect() || api.init_calls != 1) return 22;
        parser.release(); parser.release();
        if (api.spi_regs != 2 || api.release_calls != 1 || parser.isConnected() || closes != 0) return 23;
        if (parser.connect()) return 25;
        if (scenario == "success" && (parser.init(cfg) || opens != 1 || creators != 1)) return 24;
    }
    cfg->release();
    std::cout << "PASS " << argv[1] << " opens=" << opens << " symbols=" << symbols << " creators=" << creators << " closes=" << closes << " releases=" << api.release_calls << "\n";
    return 0;
}
