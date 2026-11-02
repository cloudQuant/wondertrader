#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#ifdef WT_HUAX_PARSER
#include "ParserHuaX/ParserHuaX.h"
#else
#include "TraderHuaX/TraderHuaX.h"
#endif

static int loaderCalls = 0, lookupCalls = 0, creatorCalls = 0, closeCalls = 0;
static int initCalls = 0, frontCalls = 0, spiCalls = 0, releaseCalls = 0;
static std::string loadedPath;
#ifdef WT_HUAX_PARSER
#include "fake_parser.hpp"
static FakeParserApi fakeApi;
static HuaXParserApi* fakeCreator(const TTORATstpMDSubModeType&, const TTORATstpMDSubModeType&)
#else
#include "fake_trader.hpp"
static FakeTraderApi fakeApi;
static HuaXTraderApi* fakeCreator(const char*, bool)
#endif
{
    ++creatorCalls;
    return &fakeApi;
}
extern "C" int huax_spy_dladdr(const void*, Dl_info* info)
{
    ++lookupCalls;
    std::memset(info, 0, sizeof(*info));
    info->dli_fname = std::getenv("HUAX_TEST_IMAGE");
    return 1;
}
extern "C" void* huax_spy_dlopen(const char* path, int)
{
    ++loaderCalls;
    loadedPath = path ? path : "<null>";
    return reinterpret_cast<void*>(0x1234);
}
extern "C" void* huax_spy_dlsym(void*, const char*) { return reinterpret_cast<void*>(&fakeCreator); }
extern "C" int huax_spy_dlclose(void*) { ++closeCalls; return 0; }
int main(int argc, char** argv)
{
    if (argc != 2) return 90;
    const std::string mode(argv[1]);
    const bool emptyDir = mode == "empty" || mode == "absolute" || mode == "default";
    setenv("HUAX_TEST_IMAGE", emptyDir ? "huax.dylib" : "/isolated/modules/huax.dylib", 1);
    const char* module = mode == "absolute" ? "/isolated/absolute" : "fixture";
    auto* cfg = WTSVariant::createObject();
    cfg->append("user", "test-user");
#ifdef WT_HUAX_PARSER
    if (mode != "default") cfg->append("xtpmodule", module);
    ParserHuaX object;
    const bool result = object.init(cfg);
    const bool connected = object.connect();
    object.release();
    const char* defaultModule = "xfastmdapi";
#else
    if (mode != "default") cfg->append("huaxmodule", module);
    TraderHuaX object;
    const bool result = object.init(cfg);
    // Successful connect starts an existing non-terminating worker thread.
    // Exercise its fake API in a child, and exit that child without destroying
    // a joinable thread; the parent can independently check init/release and
    // destruction. This is not a lifecycle/shutdown test.
    bool connected = false;
    if (mode == "empty" || mode == "default") {
        if (!result) object.connect();
    } else {
        const pid_t child = fork();
        if (child < 0) return 91;
        if (child == 0) {
            object.connect();
            object.release();
            const bool ok = creatorCalls == 1 && frontCalls == 1 && initCalls == 1 && releaseCalls == 1;
            std::_Exit(ok ? 0 : 92);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 93;
        connected = WEXITSTATUS(status) == 0;
    }
    object.release();
    const char* defaultModule = "fasttraderapi";
#endif
    cfg->release();
    std::cout << "mode=" << mode << " init=" << result << " connect=" << connected
              << " dladdr=" << lookupCalls << " dlopen=" << loaderCalls
              << " creator=" << creatorCalls << " front=" << frontCalls
              << " api_init=" << initCalls << " spi=" << spiCalls
              << " release=" << releaseCalls << " dlclose=" << closeCalls
              << " path=" << loadedPath << '\n';
    if (mode == "empty" || mode == "default")
        return !result && !connected && lookupCalls == 1 && loaderCalls == 0 && creatorCalls == 0 && releaseCalls == 0 && closeCalls == 0 ? 0 : 2;
    const std::string expected = (mode == "absolute" ? std::string() : "/isolated/modules/") + DLLHelper::wrap_module(module, "lib");
    if (mode == "absolute" && (expected.empty() || expected[0] != '/')) return 9;
#ifdef WT_HUAX_PARSER
    if (!result || !connected || lookupCalls != 1 || loaderCalls != 1 || loadedPath != expected || creatorCalls != 1 || frontCalls != 1 || initCalls != 1 || releaseCalls != 1 || closeCalls != 0) return 3;
#else
    if (!result || !connected || lookupCalls != 1 || loaderCalls != 1 || loadedPath != expected || creatorCalls != 0 || releaseCalls != 0 || closeCalls != 0) return 3;
#endif
    (void)defaultModule;
    return 0;
}
