#include <dlfcn.h>
#include <iostream>
#include <string>
#include "Share/ModuleHelper.hpp"

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(wt_module_helper_path_anchor), &info) || !info.dli_fname)
        return 1;
    const std::string path = info.dli_fname;
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    if (path.empty() || getInstPath() != path || getBinDir() != dir)
        return 1;

    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) { std::cerr << dlerror() << '\n'; return 1; }
    auto modulePath = reinterpret_cast<const char*(*)()>(dlsym(module, "test_module_path"));
    auto moduleDir = reinterpret_cast<const char*(*)()>(dlsym(module, "test_module_dir"));
    if (!modulePath || !moduleDir) return 1;
    const std::string pluginPath = modulePath();
    const auto pluginSlash = pluginPath.find_last_of('/');
    const std::string pluginDir = pluginSlash == std::string::npos ? std::string() : pluginPath.substr(0, pluginSlash + 1);
    if (pluginPath != argv[1] || pluginPath == path || moduleDir() != pluginDir)
        return 1;
    std::cout << "PASS real dladdr: executable=" << path << " plugin=" << pluginPath << '\n';
    dlclose(module);
    return 0;
}
