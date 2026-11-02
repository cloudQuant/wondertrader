#include "r61_ctpmini_api_stub.h"

#if !defined(WT_T3_006_CTPMINI_MISSING_SYMBOL)
namespace
{
	R61CtpMiniApiStub g_api;
}

CThostFtdcTraderApi* CThostFtdcTraderApi::CreateFtdcTraderApi(const char* /*flowPath*/)
{
#if defined(WT_T3_006_CTPMINI_NULL_CREATOR)
	return nullptr;
#else
	return &g_api;
#endif
}
#endif
