#include <cstdio>
#include <cstdlib>
#include "API/CTPMini1.5.8/ThostFtdcMdApi.h"

__attribute__((destructor)) static void markUnload()
{
    const char* path = std::getenv("WT_MINI_UNLOAD_MARKER");
    if (!path) return;
    if (FILE* file = std::fopen(path, "ab")) { std::fputs("unloaded\n", file); std::fclose(file); }
}
#ifdef WT_MINI_NULL_CREATOR
CThostFtdcMdApi* CThostFtdcMdApi::CreateFtdcMdApi(const char*, const bool, const bool)
{
    return nullptr;
}
#endif
