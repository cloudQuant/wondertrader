// A deterministic API seam for non-Windows hosts; this is not an MSVC or
// loader integration test. Each scenario runs as a separate process because
// getBinDir caches its result.
#include "Share/StrUtil.hpp"
#include <iostream>
#include <cstring>

#define _MSC_VER 1930
#include "Share/ModuleHelper.hpp"
#undef _MSC_VER

namespace {
std::string mode;
int handleCalls = 0;
int fileCalls = 0;
bool validFlags = false;
std::string modulePath;
}

BOOL GetModuleHandleExA(DWORD flags, LPCSTR address, HMODULE* module)
{
    ++handleCalls;
    validFlags = flags == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) && address != nullptr;
    if (mode == "handle_failure")
        return 0;
    *module = mode == "null_handle" ? nullptr : reinterpret_cast<HMODULE>(1);
    return 1;
}

DWORD GetModuleFileNameA(HMODULE module, char* path, DWORD size)
{
    ++fileCalls;
    if (!module || mode == "file_failure")
        return 0;
    if (mode == "always_truncated" || modulePath.size() >= size) {
        std::memset(path, 'x', size);
        return size;
    }
    std::memcpy(path, modulePath.c_str(), modulePath.size() + 1);
    return static_cast<DWORD>(modulePath.size());
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 10;
    mode = argv[1];
    if (mode == "normal")
        modulePath = "C:\\fixture\\library.dll";
    else if (mode == "long")
        modulePath = "C:\\fixture\\" + std::string(600, 'a') + "\\library.dll";
    else if (mode == "no_separator")
        modulePath = "library.dll";
    const std::string result = getBinDir();
    if (!validFlags || handleCalls != 1 || result != getBinDir() || handleCalls != 1)
        return 11;
    if (mode == "handle_failure" || mode == "null_handle")
        return result.empty() && fileCalls == 0 ? 0 : 12;
    if (mode == "file_failure" || mode == "no_separator")
        return result.empty() && fileCalls == 1 ? 0 : 13;
    if (mode == "always_truncated")
        return result.empty() && fileCalls > 1 && fileCalls <= 12 ? 0 : 14;
    if (mode == "normal")
        return result == "C:/fixture/" && fileCalls == 1 ? 0 : 15;
    if (mode == "long")
        return result == "C:/fixture/" + std::string(600, 'a') + "/" && fileCalls > 1 ? 0 : 16;
    return 17;
}
