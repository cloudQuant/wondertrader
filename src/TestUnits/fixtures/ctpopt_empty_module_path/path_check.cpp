#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include "Includes/WTSVariant.hpp"
#ifdef WT_PATH_TEST_TRADER
#include "TraderCTPOpt/TraderCTPOpt.h"
#else
#include "ParserCTPOpt/ParserCTPOpt.h"
#endif

namespace {
int loads = 0;
bool opened = false;
std::string loaded_path;
std::string module_path;
}

extern "C" int wt_path_test_dladdr(const void*, Dl_info* info)
{
    const char* mode = std::getenv("WT_PATH_TEST_MODE");
	if (!mode || std::strcmp(mode, "valid") != 0) return 0;
    const char* path = std::getenv("WT_PATH_TEST_MODULE_DIR");
    if (!path) return 0;
    module_path = std::string(path) + "/adapter.dylib";
    *info = {};
    info->dli_fname = module_path.c_str();
    return 1;
}

extern "C" void* wt_path_test_dlopen(const char* path, int flags)
{
    ++loads;
    loaded_path = path ? path : "";
    void* result = dlopen(path, flags);
    opened = result != nullptr;
    return result;
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    const bool expect_empty = std::strcmp(argv[1], "empty") == 0;
    const bool explicit_absolute = std::strcmp(argv[1], "absolute") == 0;
    if (!expect_empty && !explicit_absolute && std::strcmp(argv[1], "valid") != 0) return 2;
    setenv("WT_PATH_TEST_MODE", argv[1], 1);
    setenv("WT_PATH_TEST_MODULE_DIR", argv[2], 1);
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("front", "tcp://127.0.0.1:1");
    cfg->append("broker", "9999");
    cfg->append("user", "path-test");
    cfg->append("pass", "unused");
    cfg->append("flowdir", "flow");
    const std::string module = explicit_absolute ? std::string(argv[2]) + "/ctpopt_path_fixture" : "ctpopt_path_fixture";
    cfg->append("ctpmodule", module.c_str());
#ifdef WT_PATH_TEST_TRADER
    TraderCTPOpt adapter;
#else
    ParserCTPOpt adapter;
#endif
    const bool ok = adapter.init(cfg);
    cfg->release();
    // The physical fake module deliberately has no SDK creator symbol.
    if (ok) { std::cerr << "unexpected successful SDK initialization\n"; return 1; }
    if (expect_empty) {
        if (loads != 0) { std::cerr << "loader called with empty directory: " << loaded_path << '\n'; return 1; }
    } else {
#ifdef __APPLE__
        const std::string expected = std::string(argv[2]) + "/ctpopt_path_fixture.dylib";
#else
        const std::string expected = std::string(argv[2]) + "/ctpopt_path_fixture.so";
#endif
        if (loads != 1 || !opened || loaded_path != expected) {
            std::cerr << "load count=" << loads << " opened=" << opened << " path=" << loaded_path << '\n';
            return 1;
        }
    }
    std::cout << "PASS " << argv[1] << " loads=" << loads << " opened=" << opened << '\n';
    return 0;
}
