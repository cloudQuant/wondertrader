#include <dlfcn.h>
#include <cstdio>
#include <cstddef>
int main(int argc, char** argv)
{
    const char* path = argv[1];
    const bool doQuarantine = argc > 2;
    void* h = dlopen(path, RTLD_NOW);
    if (!h) { std::printf("dlopen failed\n"); return 2; }
    if (doQuarantine) {
        auto fn = (size_t(*)())dlsym(h, "quarantine_dummy");
        if (!fn) return 3;
        std::printf("retained=%zu\n", fn());
    }
    dlclose(h);
    void* again = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    std::printf("still_loaded_after_dlclose=%d\n", again != nullptr ? 1 : 0);
    return 0;
}
