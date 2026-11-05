#include "Includes/IParserApi.h"
#include "Includes/IParserLifecycleV2.h"
#include <cstdint>
#include <stdexcept>
#ifndef P_MODE
#define P_MODE 0
#endif
namespace {
int counts[8]{};
int init_ok = 1, pending = 0, wait_ok = 1, required_stops = 1, destroy_mode = 0, preflight_result = 0;
void (*reentry)() = nullptr;
class Parser final : public wtp::IParserApi {
public:
 bool init(wtp::WTSVariant*) override { return init_ok != 0; }
 bool connect() override { return true; }
 void release() override { ++counts[5]; }
};
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL preflight() { ++counts[1]; return preflight_result; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL stop(void*) { ++counts[2]; if (reentry) reentry(); return WT_PARSER_LIFECYCLE_V2_OK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL canwait(void*) { ++counts[3]; return wait_ok ? WT_PARSER_LIFECYCLE_V2_OK : WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK; }
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL wait(void*, uint32_t timeout) {
 ++counts[4]; if (timeout != 0 || counts[2] < required_stops) ++counts[6];
 if (counts[2] < required_stops || pending-- > 0) return WT_PARSER_LIFECYCLE_V2_PENDING;
 return WT_PARSER_LIFECYCLE_V2_DRAINED;
}
WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL destroy(void** value) {
 ++counts[7]; if (destroy_mode == 1) return WT_PARSER_LIFECYCLE_V2_OK;
 if (destroy_mode == 2) return WT_PARSER_LIFECYCLE_V2_ERROR;
 delete static_cast<Parser*>(*value); *value = nullptr; return WT_PARSER_LIFECYCLE_V2_OK;
}
}
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT wtp::IParserApi* createParser() { ++counts[0]; if (P_MODE == 8) throw std::runtime_error("factory failure"); return new Parser; }
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void deleteParser(wtp::IParserApi*& p) { ++counts[5]; delete p; p = nullptr; }
#if P_MODE != 1
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major) {
 static WtParserLifecycleApiV2 table = {sizeof(table), WT_PARSER_LIFECYCLE_V2_ABI_VERSION, WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0,0}, preflight, stop, canwait, wait, destroy};
 if (major != 2) return nullptr;
 if (P_MODE == 2) table.struct_size = 8;
 if (P_MODE == 3) table.capabilities = WT_PARSER_LIFECYCLE_V2_CAP_FACTORY_PREFLIGHT;
 if (P_MODE == 4) table.abi_version = 1u << 16;
 if (P_MODE == 5) table.reserved[0] = 1;
 if (P_MODE == 6) table.destroy_api = nullptr;
 if (P_MODE == 7) preflight_result = WT_PARSER_LIFECYCLE_V2_UNSUPPORTED;
 if (P_MODE == 9) table.preflight_factory = nullptr;
 if (P_MODE == 10) table.request_stop = nullptr;
 if (P_MODE == 11) table.can_wait = nullptr;
 if (P_MODE == 12) table.wait_drained = nullptr;
 return &table;
}
#endif
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void probe_reset() { for (int &n : counts) n = 0; init_ok=1; pending=0; wait_ok=1; required_stops=1; destroy_mode=0; preflight_result=0; reentry=nullptr; }
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT int probe_count(int i) { return counts[i]; }
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void probe_set(int i, int value) { switch(i) {case 0:init_ok=value;break;case 1:pending=value;break;case 2:wait_ok=value;break;case 3:required_stops=value;break;case 4:destroy_mode=value;break;case 5:preflight_result=value;break;} }
extern "C" WT_PARSER_LIFECYCLE_V2_EXPORT void probe_reentry(void (*fn)()) { reentry=fn; }
