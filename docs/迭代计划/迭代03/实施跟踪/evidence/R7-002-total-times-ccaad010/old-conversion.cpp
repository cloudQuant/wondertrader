#include "Includes/WTSVariant.hpp"
int main() {
    wtp::WTSVariant* cfg = wtp::WTSVariant::createObject();
    cfg->append("total_times", int32_t(-1));
    volatile uint32_t result = cfg->getUInt32("total_times");
    cfg->release();
    return result == 0;
}
