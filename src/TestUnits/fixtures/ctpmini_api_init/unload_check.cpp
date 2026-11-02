#include <dlfcn.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include "Includes/WTSVariant.hpp"
#include "ParserCTPMini/ParserCTPMini.h"

extern "C" int wt_mini_real_dladdr(const void*, Dl_info* info)
{
    static const std::string path = std::string(std::getenv("WT_MINI_REAL_MODULE_DIR")) + "/adapter.dylib";
    *info = {}; info->dli_fname = path.c_str(); return 1;
}
int main(int argc, char** argv)
{
    if (argc != 2 || !std::getenv("WT_MINI_UNLOAD_MARKER") || !std::getenv("WT_MINI_REAL_MODULE_DIR")) return 2;
    const std::string mode = argv[1];
    if (mode != "missing-symbol" && mode != "null-api") return 2;
    const auto marker = std::filesystem::path(std::getenv("WT_MINI_UNLOAD_MARKER"));
    if (std::filesystem::exists(marker)) return 3;
    WTSVariant* cfg = WTSVariant::createObject();
    cfg->append("front", "tcp://127.0.0.1:1"); cfg->append("broker", "9999");
    cfg->append("user", "test"); cfg->append("pass", "unused"); cfg->append("flowdir", "flow");
    cfg->append("ctpmodule", mode == "missing-symbol" ? "fake_missing" : "fake_null");
    {
        ParserCTPMini parser;
        if (parser.init(cfg)) return 10;
        if (std::filesystem::exists(marker)) { std::cerr << "FAIL unloaded after failed init\n"; return 31; }
        parser.release(); parser.release();
        if (std::filesystem::exists(marker)) { std::cerr << "FAIL unloaded during release\n"; return 32; }
        if (parser.init(cfg)) return 11;
        if (std::filesystem::exists(marker)) return 33;
    }
    if (std::filesystem::exists(marker)) { std::cerr << "FAIL unloaded after parser destruction\n"; return 34; }
    cfg->release();
    std::cout << "PASS " << mode << " loader reference survived failed init, repeated release and parser destruction\n";
    return 0;
}
