#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#ifdef WT_TEST_FEMAS
#include "TraderFemas/TraderFemas.h"
#else
#include "TraderXTP/TraderXTP.h"
#endif

namespace {
int loadCount = 0;
int lookupCount = 0;
int creatorCount = 0;
int closeCount = 0;
std::string loadedPath;
#ifdef WT_TEST_FEMAS
void fakeCreator() {}
#else
XTP::API::TraderApi* fakeCreator(uint8_t, const char*, XTP_LOG_LEVEL)
{
    ++creatorCount;
    return nullptr;
}
#endif
}

extern "C" int wt_path_test_dladdr(const void*, Dl_info* info)
{
    ++lookupCount;
    std::memset(info, 0, sizeof(*info));
    // A slash-free, nonempty module name makes getBinDir() return empty.
    info->dli_fname = std::getenv("WT_PATH_FAKE_IMAGE");
    return 1;
}
extern "C" void* wt_path_test_dlopen(const char* path, int)
{
    ++loadCount;
    loadedPath = path ? path : "<null>";
    return reinterpret_cast<void*>(0x1234);
}
extern "C" void* wt_path_test_dlsym(void*, const char*)
{
    return reinterpret_cast<void*>(&fakeCreator);
}
extern "C" int wt_path_test_dlclose(void*) { ++closeCount; return 0; }

int main(int argc, char** argv)
{
    if (argc != 2) return 90;
    const std::string mode(argv[1]);
    const bool emptyDir = mode == "empty" || mode == "empty-absolute" || mode == "default";
    setenv("WT_PATH_FAKE_IMAGE", emptyDir ? "self.dylib" : "/fake/module/self.dylib", 1);
    auto* cfg = WTSVariant::createObject();
#ifdef WT_TEST_FEMAS
    if (mode != "default") cfg->append("ctpmodule", mode == "empty-absolute" ? "/fake/absolute" : "fakeFEMAS");
    auto trader = std::make_unique<TraderFemas>();
    const bool result = trader->init(cfg);
    const std::string expected = mode == "empty-absolute" ? DLLHelper::wrap_module("/fake/absolute", "") : mode == "default"
        ? DLLHelper::wrap_module("USTPtraderapiAF", "lib")
        : std::string("/fake/module/") + DLLHelper::wrap_module("fakeFEMAS", "");
#else
    cfg->append("xtpmodule", mode == "empty-absolute" ? "/fake/absolute" : "fakeXTP");
    auto trader = std::make_unique<TraderXTP>();
    const bool result = trader->init(cfg);
    trader->connect();
    const std::string expected = mode == "empty-absolute" ? DLLHelper::wrap_module("/fake/absolute", "lib") : std::string("/fake/module/") + DLLHelper::wrap_module("fakeXTP", "lib");
#endif
    cfg->release();
    trader.reset();
    std::cout << "mode=" << mode << " init=" << result << " dladdr=" << lookupCount
              << " dlopen=" << loadCount << " creator=" << creatorCount << " dlclose=" << closeCount << " path=" << loadedPath << '\n';
    if (closeCount != 0) return 6;
    if (mode == "empty") return !result && loadCount == 0 && lookupCount == 1 && creatorCount == 0 ? 0 : 1;
#ifndef WT_TEST_FEMAS
    if (creatorCount != 1) return 5;
#endif
    if (mode == "empty-absolute") return result && loadCount == 1 && lookupCount == 1 && loadedPath == expected ? 0 : 4;
    if (mode == "good") return result && loadCount == 1 && lookupCount == 1 && loadedPath == expected ? 0 : 2;
#ifdef WT_TEST_FEMAS
    if (mode == "default") return result && loadCount == 1 && lookupCount == 0 && loadedPath == expected ? 0 : 3;
#endif
    return 91;
}
