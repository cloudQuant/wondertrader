#if defined(_WIN32)
#define R60_FIXTURE_EXPORT __declspec(dllexport)
#else
#define R60_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" R60_FIXTURE_EXPORT void r60_yd_missing_symbol_fixture_marker()
{
}
