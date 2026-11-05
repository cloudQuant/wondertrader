#include <dlfcn.h>
#include <cstdio>
int main(int argc, char** argv) {
  if (argc < 2) return 90;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { std::fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 91; }
  auto fn = reinterpret_cast<int (*)(int)>(dlsym(h, "r10005_probe"));
  int v = fn ? fn(1) : -1;
  dlclose(h);
  std::printf("probe=%d\n", v);
  return v == 2 ? 0 : 92;
}
