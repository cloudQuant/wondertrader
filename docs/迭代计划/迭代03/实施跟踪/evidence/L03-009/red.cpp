#include "Includes/WTSSessionInfo.hpp"
#include <cstdint>
#include <limits>
int main(int argc, char**) {
    const int32_t offset = argc > 1 ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int32_t>::max();
    wtp::WTSSessionInfo* s = wtp::WTSSessionInfo::create("probe", "probe", offset);
    volatile auto o = s->offsetTime(900, true);
    volatile auto i = s->originalTime(1000);
    volatile auto d = s->getOffsetDate(20261101, 900);
    (void)o; (void)i; (void)d;
    s->release();
}
