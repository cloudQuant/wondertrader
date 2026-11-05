#include <cstdio>
#include <cstdlib>

namespace {
void mark(char c) {
    const char* path = std::getenv("R78_PARSER_MODULE_MARKER");
    if (!path) return;
    if (FILE* file = std::fopen(path, "a")) { std::fputc(c, file); std::fclose(file); }
}
struct ModuleMarker {
    ModuleMarker() { mark('L'); }
    ~ModuleMarker() { mark('U'); }
} marker;
}
extern "C" int r78_no_parser_symbol() { return 78; }
