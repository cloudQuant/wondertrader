// ABI 2.2 execution plugin that wraps the real WtTWapExeUnit (compiled from
// src/WtExeFact, read-only) for the N05-003 card-5b test. Compiled with
// -DWP1_TWAP_HOST_IDENTITY=1 it advertises WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY;
// with 0 it is an "old identity ABI" plugin around the very same unit code.
// No threads, no provider; orders only reach the test's ExecuteContext.
#include <Includes/ExecuteDefs.h>
#include <Includes/IExecPluginV2.h>
#include <Includes/WTSVariant.hpp>
#define private public
#include <WtExeFact/WtTWapExeUnit.h>
#undef private

#include <cstring>

// WtTWapExeUnit::getFactName() refers to the plugin's factory name symbol.
const char* FACT_NAME = "Wp1Twap";

namespace {
int g_factory_token = 1;

void* WT_EXEC_PLUGIN_V2_CALL create_factory() { return &g_factory_token; }
void WT_EXEC_PLUGIN_V2_CALL destroy_factory(void*) {}
const char* WT_EXEC_PLUGIN_V2_CALL get_factory_name(void*) { return "Wp1Twap"; }
void* WT_EXEC_PLUGIN_V2_CALL create_unit(void*, WtExecPluginV2UnitKind, const char* name)
{
    if (name == nullptr || std::strcmp(name, "TWap") != 0)
        return nullptr;
    return static_cast<wtp::ExecuteUnit*>(new WtTWapExeUnit());
}
void WT_EXEC_PLUGIN_V2_CALL destroy_unit(void*, void* unit) { delete static_cast<wtp::ExecuteUnit*>(unit); }
WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL initialize_unit(void*, void* unit, void* ctx, const char* code,
    void* configuration, char* error_text, uint32_t error_capacity)
{
    if (error_capacity != 0)
        error_text[0] = '\0';
    static_cast<wtp::ExecuteUnit*>(unit)->init(static_cast<wtp::ExecuteContext*>(ctx), code,
        static_cast<wtp::WTSVariant*>(configuration));
    return WT_EXEC_PLUGIN_V2_OK;
}
WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL request_quiesce(void*, void*) { return WT_EXEC_PLUGIN_V2_OK; }
int32_t WT_EXEC_PLUGIN_V2_CALL can_wait_quiesce(void*, void*) { return 1; }
int32_t WT_EXEC_PLUGIN_V2_CALL wait_quiesce(void*, void*) { return 1; }
int32_t WT_EXEC_PLUGIN_V2_CALL can_wait_factory_quiesce(void*) { return 1; }

const WtExecPluginApiV2 API = {
    sizeof(WtExecPluginApiV2), WT_EXEC_PLUGIN_V2_ABI_VERSION, { 0, 0 },
    create_factory, destroy_factory, get_factory_name, create_unit, destroy_unit, initialize_unit,
    WT_EXEC_PLUGIN_V2_CAP_QUIESCE | WT_EXEC_PLUGIN_V2_CAP_FACTORY_QUIESCE_PREFLIGHT
#if WP1_TWAP_HOST_IDENTITY
        | WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY
#endif
    ,
    request_quiesce, can_wait_quiesce, wait_quiesce, can_wait_factory_quiesce
};
}

extern "C" WT_EXEC_PLUGIN_V2_EXPORT const WtExecPluginApiV2* WT_EXEC_PLUGIN_V2_CALL getExecPluginApiV2(uint32_t major)
{
    return major == WT_EXEC_PLUGIN_V2_ABI_MAJOR ? &API : nullptr;
}

// Test hook: put one order of the unit under its own timeout monitor (as if
// the unit had sent it), so the timeout path is reached deterministically.
extern "C" WT_EXEC_PLUGIN_V2_EXPORT void wp1TwapMonitorOrder(void* unit, uint32_t localid, uint64_t now)
{
    WtTWapExeUnit* twap = dynamic_cast<WtTWapExeUnit*>(static_cast<wtp::ExecuteUnit*>(unit));
    if (twap != nullptr)
        twap->_orders_mon.push_order(&localid, 1, now);
}
