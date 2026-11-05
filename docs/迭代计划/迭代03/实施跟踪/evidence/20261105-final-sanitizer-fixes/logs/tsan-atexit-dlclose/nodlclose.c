// Interposes dlclose() with a no-op so modules stay mapped until exit (diagnostic only).
#include <dlfcn.h>
int my_dlclose(void* h) { (void)h; return 0; }
__attribute__((used)) static struct { const void* repl; const void* orig; } interposers[]
  __attribute__((section("__DATA,__interpose"))) = { { (const void*)my_dlclose, (const void*)dlclose } };
