#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Includes/WTSVariant.hpp"
#if defined(TEST_XTP)
#include "ParserXTP/ParserXTP.h"
using ParserUnderTest = ParserXTP;
#else
#include "ParserFemas/ParserFemas.h"
using ParserUnderTest = ParserFemas;
#endif

static unsigned load_calls = 0;
static std::string loaded_path;

extern "C" int wt_test_dladdr(const void*, Dl_info* info)
{
    const char* dir = std::getenv("WT_PARSER_TEST_DIR");
    if (!dir || !*dir) return 0;
    static std::string module_path;
    module_path = std::string(dir) + "/parser-under-test";
    *info = {};
    info->dli_fname = module_path.c_str();
    return 1;
}

extern "C" void* wt_test_dlopen(const char* path, int flags)
{
    ++load_calls;
    loaded_path = path ? path : "";
    return ::dlopen(path, flags);
}

int main(int argc, char** argv)
{
    if (argc != 2) return 90;
    const bool expect_directory = std::strcmp(argv[1], "valid") == 0;
    const bool explicit_absolute = std::strcmp(argv[1], "absolute") == 0;
    const char* directory = std::getenv("WT_PARSER_TEST_DIR");
    const char* marker = std::getenv("WT_PARSER_TEST_MARKER");
    if (!marker || (expect_directory && (!directory || !*directory))) return 91;
    auto* config = WTSVariant::createObject();
    config->append("user", "fixture-user");
    config->append("pass", "fixture-pass");
    config->append("flowdir", "flow");
#if defined(TEST_XTP)
    config->append("host", "127.0.0.1");
    config->append("port", static_cast<int32_t>(1));
    config->append("protocol", static_cast<uint32_t>(0));
    config->append("clientid", static_cast<uint32_t>(1));
    config->append("hbinterval", static_cast<uint32_t>(1));
    config->append("buffsize", static_cast<uint32_t>(1));
    config->append("local_ip", "127.0.0.1");
    config->append("xtpmodule", explicit_absolute ? "/private/tmp/fixture" : "fixture");
#else
    config->append("front", "tcp://127.0.0.1:1");
    config->append("broker", "fixture-broker");
    config->append("ctpmodule", explicit_absolute ? "/private/tmp/fixture" : "fixture");
#endif
    ParserUnderTest parser;
    const bool result = parser.init(config);
    config->release();
    // The real fake shared object lacks the provider creator: both branches
    // return false, while only the valid directory is allowed to load it.
    if (result) return 1;
    if (explicit_absolute)
    {
        const std::string expected = DLLHelper::wrap_module("/private/tmp/fixture", "lib");
        if (expected.empty() || expected[0] != '/' || load_calls != 1 || loaded_path != expected) return 6;
        return 0;
    }
    if (!expect_directory)
    {
        if (load_calls != 0) return 2;
        if (FILE* file = std::fopen(marker, "rb")) { std::fclose(file); return 3; }
        return 0;
    }
#if defined(__APPLE__)
    const std::string expected = std::string(directory) + "/libfixture.dylib";
#else
    const std::string expected = std::string(directory) + "/libfixture.so";
#endif
    if (load_calls != 1 || loaded_path != expected) return 4;
    FILE* file = std::fopen(marker, "rb");
    if (!file) return 5;
    std::fclose(file);
    return 0;
}
