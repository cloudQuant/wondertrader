#include "Share/TimeUtils.hpp"
#include <cstdio>
int main() {
    const auto value = TimeUtils::getNextDate(20240228u, 1491308);
    std::printf("date=%u\n", value);
}
