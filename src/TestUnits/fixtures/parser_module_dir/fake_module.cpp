#include <cstdio>
#include <cstdlib>
__attribute__((constructor)) static void module_loaded()
{
    const char* path = std::getenv("WT_PARSER_TEST_MARKER");
    if (!path) return;
    if (FILE* file = std::fopen(path, "wb")) {
        std::fputs("loaded\n", file);
        std::fclose(file);
    }
}
