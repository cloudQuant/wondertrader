#include "Includes/WTSSessionInfo.hpp"
#include <cstdint>
#include <cstdio>
#include <limits>

int main()
{
    const int32_t offsets[] = {1441, -1441, 2880, -2880,
        (std::numeric_limits<int32_t>::max)(),
        (std::numeric_limits<int32_t>::min)()};
    for (const int32_t offset : offsets)
    {
        wtp::WTSSessionInfo* s = wtp::WTSSessionInfo::create("probe", "probe", offset);
        for (uint32_t time : {0u, 900u, 2359u, 2400u})
        {
            std::printf("offset=%d time=%04u left=%u right=%u inverse=%u date=%u\n",
                offset, time, s->offsetTime(time, true), s->offsetTime(time, false),
                s->originalTime(time), s->getOffsetDate(20240228u, time));
        }
        s->release();
    }
    wtp::WTSSessionInfo* session = wtp::WTSSessionInfo::create("setter", "setter", 0);
    session->addTradingSection(900, 1000);
    for (int32_t offset : {0, 2880, -2880})
    {
        session->setOffsetMins(offset);
        const auto& section = session->getTradingSections().front();
        std::printf("setter=%d section=%u..%u minutes=%u\n", offset,
            section.first, section.second, session->getTradingMins());
    }
    session->release();
}
