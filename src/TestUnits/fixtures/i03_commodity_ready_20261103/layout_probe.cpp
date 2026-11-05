// Prints sizeof/offsets of WTSCommodityInfo members for the header selected by
// the include path (-I <dir containing Includes/WTSContractInfo.hpp>).
// Compare the outputs of the HEAD, pre-edit and current headers.
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <string>
// Parse dependencies first so the access override only affects the target class.
#include "Includes/WTSObject.hpp"
#include "Includes/WTSTypes.h"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/FasterDefs.h"
#define private public
#define protected public
#include "Includes/WTSContractInfo.hpp"
#undef protected
#undef private

#pragma clang diagnostic ignored "-Winvalid-offsetof"
#define OFF(m) std::printf("%-16s %zu\n", #m, offsetof(wtp::WTSCommodityInfo, m))

int main()
{
	std::printf("sizeof %zu alignof %zu\n", sizeof(wtp::WTSCommodityInfo), alignof(wtp::WTSCommodityInfo));
	OFF(m_strName); OFF(m_strFullPid); OFF(m_uVolScale); OFF(m_dPriceTick); OFF(m_dLotTick);
	OFF(m_dMinLots); OFF(m_ccCategory); OFF(m_coverMode); OFF(m_priceMode); OFF(m_tradeMode);
	OFF(m_setCodes); OFF(m_pSession); OFF(m_dOpenFee); OFF(m_dCloseFee); OFF(m_dCloseTFee);
	OFF(m_nFeeAlg);
#ifdef WT_LAYOUT_HAS_STATE
	OFF(m_uConfigState);
#endif
	OFF(m_dMarginRate);
	return 0;
}
