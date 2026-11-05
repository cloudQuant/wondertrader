#include <cstdlib>
__attribute__((noinline)) void leak() { void* p = malloc(4096); asm volatile("" : : "r"(p) : "memory"); }
int main() { leak(); return 0; }
