#include <WtCore/TraderAdapter.h>

namespace r202_probe
{
bool isTradeEnabled(const wtp::TraderAdapter& adapter, const char* stdCode)
{
	return adapter.isTradeEnabled(stdCode);
}
}
