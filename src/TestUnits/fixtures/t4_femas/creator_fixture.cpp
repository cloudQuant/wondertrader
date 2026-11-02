#include "r93_femas_api_stub.h"

#if defined(WT_T4_FEMAS_SUCCESSFUL_CREATOR)
namespace
{
	R93FemasApiStub g_api;
}
#endif

CUstpFtdcTraderApi* CUstpFtdcTraderApi::CreateFtdcTraderApi(const char*)
{
#if defined(WT_T4_FEMAS_NULL_CREATOR)
	return nullptr;
#else
	return &g_api;
#endif
}
