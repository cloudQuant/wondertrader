#include "Includes/WTSSessionInfo.hpp"
#include <cstdint>
#include <cstdio>

static bool validClock(uint32_t value, bool rightAligned)
{
    return (value < 2400 || (rightAligned && value == 2400)) && value % 100 < 60;
}
int main()
{
    unsigned failures = 0;
    for (int32_t offset : {1441, -1441, 2880, -2880})
    {
        auto* s = wtp::WTSSessionInfo::create("range", "range", offset);
        for (uint32_t t : {0u, 900u, 2359u, 2400u})
        {
            for (bool right : {false, true})
            {
                const uint32_t result = s->offsetTime(t, !right);
                if (!validClock(result, right)) {
                    std::printf("FAIL offset=%d input=%u side=%s result=%u\n", offset, t, right ? "right" : "left", result);
                    ++failures;
                }
            }
        }
        s->release();
    }
    std::printf("failures=%u\n", failures);
    return failures == 0 ? 0 : 1;
}
