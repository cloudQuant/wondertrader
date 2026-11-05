// I03-NEW-045 fixture (iteration 03, s5-runners): a local V2 parser provider
// whose callback drain can never be proven (wait_drained reports ERROR), so the
// host quarantines it per card 9a. Reuses the data-kit fake provider verbatim
// and only flips its wait mode at load time. No vendor SDK or network is used.
#include "../i03_dt_parser_host_20261104s5/fake_provider.cpp"

namespace
{
struct UncertainDrainByDefault
{
	UncertainDrainByDefault() { g_wait_mode.store(2); }
} g_uncertain_drain_by_default;
}
