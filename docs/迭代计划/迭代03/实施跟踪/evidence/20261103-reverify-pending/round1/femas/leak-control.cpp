#include <cstdlib>
int main(){ void* p = std::malloc(77); (void)p; p = nullptr; return 0; }
