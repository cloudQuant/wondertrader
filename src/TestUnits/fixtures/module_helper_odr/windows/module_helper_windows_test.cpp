#include "Share/ModuleHelper.hpp"

#include <filesystem>
#include <iostream>
#include <system_error>

using PathFunction = const char* (*)();

static bool sameDirectory(const char* actual, const std::filesystem::path& expected)
{
    if (actual == nullptr || actual[0] == '\0')
        return false;
    std::error_code error;
    return std::filesystem::equivalent(actual, expected, error) && !error;
}

static bool check(HMODULE module, const char* function, const std::filesystem::path& expected)
{
    auto f = reinterpret_cast<PathFunction>(GetProcAddress(module, function));
    // MSVC x86 may export a C symbol with its leading underscore.
    if (f == nullptr && sizeof(void*) == 4) {
        const std::string decorated = std::string("_") + function;
        f = reinterpret_cast<PathFunction>(GetProcAddress(module, decorated.c_str()));
    }
    return f != nullptr && sameDirectory(f(), expected);
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 1;
    const auto dirA = std::filesystem::absolute(argv[1]).parent_path();
    const auto dirB = std::filesystem::absolute(argv[2]).parent_path();
    char exePath[32768] = {};
    const DWORD exeLength = GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
    if (exeLength == 0 || exeLength >= sizeof(exePath) ||
        !sameDirectory(getBinDir(), std::filesystem::path(exePath).parent_path()))
        return 2;

    for (int iteration = 0; iteration < 2; ++iteration) {
        HMODULE a = LoadLibraryA(argv[1]);
        HMODULE b = LoadLibraryA(argv[2]);
        if (!a || !b)
            return 3;
        const bool ok = check(a, "module_a_first_tu", dirA) &&
            check(a, "module_a_second_tu", dirA) &&
            check(a, "module_a_static_initialization", dirA) &&
            check(b, "module_b_path", dirB) &&
            !check(a, "module_a_first_tu", dirB) &&
            !check(b, "module_b_path", dirA) &&
            !sameDirectory(getBinDir(), dirA) && !sameDirectory(getBinDir(), dirB);
        const bool unloaded = FreeLibrary(b) && FreeLibrary(a);
        if (!ok || !unloaded)
            return 4;
    }
    return 0;
}
