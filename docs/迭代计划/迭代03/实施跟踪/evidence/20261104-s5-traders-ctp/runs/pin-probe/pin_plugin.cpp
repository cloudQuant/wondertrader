#include "TraderCTP/VendorTdLifecycle.hpp"
static int g_dummy = 0;
extern "C" __attribute__((visibility("default"))) size_t quarantine_dummy() { return wt_vendor_td::quarantine(&g_dummy); }
