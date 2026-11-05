#include <thread>
#include <cstdlib>
#include <cstring>
__attribute__((noinline)) void leak() { void *p = std::malloc(4096); std::memset(p, 1, 4096); asm volatile("" : : "r"(p) : "memory"); }
int main() { std::thread t(leak); t.join(); return 0; }
