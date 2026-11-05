// Minimal ABI 2.2 execution plugin for the WP1 fixture. Compiled with
// -DWP1_FAKE_HOST_IDENTITY=1 it advertises WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY,
// otherwise it is an "old identity ABI" plugin. initialize_unit records the
// value of the reserved host identity key it receives; no orders, no threads.
#include <Includes/ExecuteDefs.h>
#include <Includes/IExecPluginV2.h>
#include <Includes/WTSVariant.hpp>

#include <cstring>

namespace {
int64_t g_last_key = -1;
int g_factory_token = 1;

class FakeUnit final : public wtp::ExecuteUnit
{
public:
    const char* getName() override { return "unit"; }
    const char* getFactName() override { return "Wp1FakeExec"; }
    void set_position(const char*, double) override {}
    void on_tick(wtp::WTSTickData*) override {}
    void on_trade(uint32_t, const char*, bool, double, double) override {}
    void on_order(uint32_t, const char*, bool, double, double, bool) override {}
    void on_entrust(uint32_t, const char*, bool, const char*) override {}
    void on_channel_ready() override {}
    void on_channel_lost() override {}
};

void* WT_EXEC_PLUGIN_V2_CALL create_factory() { return &g_factory_token; }
void WT_EXEC_PLUGIN_V2_CALL destroy_factory(void*) {}
const char* WT_EXEC_PLUGIN_V2_CALL get_factory_name(void*) { return "Wp1FakeExec"; }
void* WT_EXEC_PLUGIN_V2_CALL create_unit(void*, WtExecPluginV2UnitKind, const char*) { return new FakeUnit(); }
void WT_EXEC_PLUGIN_V2_CALL destroy_unit(void*, void* unit) { delete static_cast<wtp::ExecuteUnit*>(unit); }
WtExecPluginV2Status WT_EXEC_PLUGIN_V2_CALL initialize_unit(void*, void*, void*, const char*,
    void* configuration, char* error_text, uint32_t error_capacity)
{
    wtp::WTSVariant* cfg = static_cast<wtp::WTSVariant*>(configuration);
    g_last_key = (cfg != nullptr && cfg->has(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY))
        ? (int64_t)cfg->getUInt32(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY) : -1;
    if (error_capacity != 0)
        error_text[0] = '\0';
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
#if WP1_FAKE_HOST_IDENTITY
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

extern "C" WT_EXEC_PLUGIN_V2_EXPORT int64_t wp1FakeExecLastHostKey() { return g_last_key; }
