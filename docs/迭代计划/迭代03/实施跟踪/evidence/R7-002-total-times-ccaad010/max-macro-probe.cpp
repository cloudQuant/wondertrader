#include <limits>
#include <cstdint>
#define max(a, b) windows_max_macro(a, b)
static_assert((std::numeric_limits<uint32_t>::max)() == UINT32_MAX, "max macro collision");
int main() { return 0; }
