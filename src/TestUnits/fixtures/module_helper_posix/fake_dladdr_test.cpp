#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<unsigned> calls{0};
static std::string mode;
static int fake_dladdr(const void*, Dl_info* info)
{
    ++calls;
    if (mode == "concurrent")
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (mode == "failure")
    {
        info->dli_fname = nullptr;
        return 0;
    }
    if (mode == "null")
    {
        info->dli_fname = nullptr;
        return 1;
    }
    if (mode == "empty")
    {
        info->dli_fname = "";
        return 1;
    }
    info->dli_fname = mode == "no_separator" ? "module.so" : "/opt/wonder/module.so";
    return 1;
}
#define dladdr fake_dladdr
#include "Share/ModuleHelper.hpp"
#undef dladdr

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    mode = argv[1];
    try
    {
        if (mode == "concurrent")
        {
            constexpr unsigned count = 24;
            std::atomic<unsigned> ready{0}, errors{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (unsigned i = 0; i < count; ++i)
                threads.emplace_back([&] {
                    ++ready;
                    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                    try {
                        if (std::string(getBinDir()) != "/opt/wonder/" ||
                            getInstPath() != "/opt/wonder/module.so") ++errors;
                    } catch (...) { ++errors; }
                });
            while (ready.load() != count) std::this_thread::yield();
            go.store(true, std::memory_order_release);
            for (auto& thread : threads) thread.join();
            if (errors != 0 || calls != 1)
            {
                std::cerr << "concurrent errors=" << errors << " dladdr calls=" << calls << '\n';
                return 1;
            }
        }
        else
        {
            const bool noPath = mode == "failure" || mode == "null" || mode == "empty";
            const std::string expectedPath = noPath ? "" : mode == "no_separator" ? "module.so" : "/opt/wonder/module.so";
            const std::string expectedDir = noPath || mode == "no_separator" ? "" : "/opt/wonder/";
            for (int i = 0; i < 3; ++i)
                if (getInstPath() != expectedPath || getBinDir() != expectedDir)
                    return 1;
            if (calls != 1) { std::cerr << "dladdr calls=" << calls << '\n'; return 1; }
        }
    }
    catch (const std::exception& ex) { std::cerr << "exception: " << ex.what() << '\n'; return 1; }
    std::cout << "PASS " << mode << '\n';
    return 0;
}
