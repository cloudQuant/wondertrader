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
    if (!success && scenario != "missing-library" && scenario != "missing-symbol" && scenario != "null-api" && scenario != "null-config" && scenario != "cross-instance") return 2;
    Sink sink;
    ParserCTPMini parser;
    parser.registerSpi(&sink);
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("front", "tcp://127.0.0.1:1"); cfg->append("broker", "9999");
    cfg->append("user", "test"); cfg->append("pass", "unused"); cfg->append("flowdir", "flow");
    if (scenario == "cross-instance") {
        if (parser.init(cfg)) return 41;
        ParserCTPMini other;
        if (other.init(cfg)) return 42;
        cfg->release();
        if (opens != 1) {
            std::cerr << "UNRESOLVED: same module was loaded again from another parser (opens=" << opens << ")\n";
            return 40;
        }
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
        parser.release(); parser.release();
        if (parser.connect()) return 17;
        // Model a late callback whose code would have been unmapped by close.
        if (closes != 0 || (loadedFailure && failedModuleUnloaded)) return 11;
        scenario = "success";
        WTSVariant* good = WTSVariant::createObject();
        good->append("front", "tcp://127.0.0.1:1"); good->append("broker", "9999");
        good->append("user", "test"); good->append("pass", "unused"); good->append("flowdir", "flow");
        good->append("ctpmodule", "distinct_healthy_module");
        if (loadedFailure) {
            // Even a different target is fail-closed on the quarantined object.
            if (parser.init(good) || opens != expectedOpens || !sink.has("initialization disabled")) return 12;
            ParserCTPMini other;
            if (!other.init(good) || !other.isConnected() || lastPath == failedPath) return 13;
            other.release(); other.release();
        } else {
            if (!parser.init(good) || !parser.isConnected()) return 14;
            parser.release(); parser.release();
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
