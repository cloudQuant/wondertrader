#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include "Includes/WTSVariant.hpp"
#include "Share/DllHelper.hpp"
#ifdef WT_PATH_TEST_TRADER
#include "TraderCTPMini/TraderCTPMini.h"
#else
#include "ParserCTPMini/ParserCTPMini.h"
#endif
namespace { int loads = 0; std::string mode, module_path, expected; }
extern "C" int wt_ctpm_dladdr(const void*, Dl_info* info)
{
    if (mode != "valid") return 0;
    *info = {}; info->dli_fname = module_path.c_str(); return 1;
}
extern "C" void* wt_ctpm_dlopen(const char* path, int flags)
{
    ++loads;
    const std::string observed = path ? path : "";
    std::cout << "loader path=" << observed << std::endl;
    if (mode == "empty") std::_Exit(42);
    if (observed != expected) std::_Exit(43);
    if (mode == "valid") {
        void* module = dlopen(path, flags);
        if (!module) { std::cerr << "real fake-module dlopen failed: " << dlerror() << std::endl; std::_Exit(44); }
    }
    // Stop at the observed loader call: ParserCTPMini's existing missing-creator
    // path is unsafe and is intentionally not changed by this fixture.
    std::_Exit(0);
}
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    mode = argv[1];
    if (mode != "empty" && mode != "valid" && mode != "absolute") return 2;
    const std::string dir = argv[2];
    module_path = dir + "/adapter.dylib";
    const std::string input = mode == "absolute" ? dir + "/wt_ctpm_path_fake" : "wt_ctpm_path_fake";
#ifdef WT_PATH_TEST_TRADER
    expected = DLLHelper::wrap_module(input.c_str(), "");
    TraderCTPMini adapter;
#else
    expected = DLLHelper::wrap_module(input.c_str(), "lib");
    ParserCTPMini adapter;
#endif
    if (mode == "valid") expected = dir + "/" + expected;
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("front", "tcp://127.0.0.1:1"); cfg->append("broker", "9999");
    cfg->append("user", "test"); cfg->append("pass", "unused");
    cfg->append("flowdir", "flow"); cfg->append("ctpmodule", input.c_str());
    const bool ok = adapter.init(cfg); cfg->release();
    if (mode != "empty" || ok || loads) return 1;
    // Both adapters must tolerate a caller trying connect after this failure.
    adapter.connect();
    if (loads) return 1;
    std::cout << "PASS empty path rejected; connect made no load" << std::endl;
    return 0;
}
