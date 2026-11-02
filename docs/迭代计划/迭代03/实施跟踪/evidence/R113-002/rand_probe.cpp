#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
std::vector<uint32_t> splitVolume(uint32_t, uint32_t, uint32_t);
int main() {
    std::srand(1949); const int expected=std::rand();
    std::srand(1949); const auto parts=splitVolume(1000, 1, 100); const int actual=std::rand();
    std::cout << "expected=" << expected << " actual=" << actual << " pieces=" << parts.size() << '\n';
    return expected==actual ? 0 : 1;
}
