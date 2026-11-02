#if defined(_WIN32)
#define WT_T1_CTPOPT_EXPORT __declspec(dllexport)
#else
#define WT_T1_CTPOPT_EXPORT __attribute__((visibility("default")))
#endif

extern "C" WT_T1_CTPOPT_EXPORT void wt_t1_ctpopt_missing_symbol_fixture()
{
}
