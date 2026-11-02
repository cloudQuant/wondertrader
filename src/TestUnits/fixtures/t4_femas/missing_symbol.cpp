#if defined(_WIN32)
#define T4_FEMAS_FIXTURE_EXPORT __declspec(dllexport)
#else
#define T4_FEMAS_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" T4_FEMAS_FIXTURE_EXPORT void wt_t4_femas_missing_symbol_marker()
{
}
