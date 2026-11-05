/**
 * BUG-I02-NEW-130 (card 15 final review): metadata accessors of an engine
 * that has not been initialized.
 *
 * WtRtRunner owns one CTA, one SEL and one HFT engine but configures only the
 * engine selected by env.name; a second configuration of the same runner is
 * rejected (card 9c=A). A context can still be created on the unconfigured
 * sibling engine, and context calls then reach WtEngine::get_commodity_info
 * and friends. Before the fix WtEngine left _base_data_mgr/_hot_mgr/_data_mgr
 * uninitialized and dereferenced them unconditionally: a zero-initialized
 * engine (the porter's process-static runner) crashed with SIGSEGV at
 * WtEngine.cpp:132, and a heap engine read indeterminate pointers.
 *
 * Contract checked here: an engine whose init() never ran reports "no
 * metadata" (NULL / 0 / empty) and does not dereference missing managers.
 * Each case runs in a child process so that the pre-fix red control reports a
 * failed assertion instead of terminating the whole test binary.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <WtCore/WtSelEngine.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtHftEngine.h>

USING_NS_WTP;

namespace
{
template <typename Engine>
int probeUninitializedEngine(unsigned char fill)
{
	// Emulates both the zero-initialized static runner (fill 0x00) and a heap
	// engine with indeterminate member storage (fill 0xA5).
	alignas(Engine) static unsigned char storage[sizeof(Engine)];
	std::memset(storage, fill, sizeof(storage));
	Engine* engine = new (storage) Engine();

	int failures = 0;
	if (engine->get_basedata_mgr() != NULL) failures |= 1;
	if (engine->get_hot_mgr() != NULL) failures |= 2;
	if (engine->get_commodity_info("SHFE.au.2406") != NULL) failures |= 4;
	if (engine->get_contract_info("SHFE.au.2406") != NULL) failures |= 8;
	if (engine->get_session_info("TRADING", false) != NULL) failures |= 16;
	if (engine->get_session_info("SHFE.au.2406", true) != NULL) failures |= 32;
	if (!engine->get_rawcode("SHFE.au.HOT").empty()) failures |= 64;
	if (engine->get_last_tick(1, "SHFE.au.2406") != NULL) failures |= 128;
	if (engine->get_tick_slice(1, "SHFE.au.2406", 5) != NULL) failures |= 256;
	if (engine->get_kline_slice(1, "SHFE.au.2406", "m1", 5) != NULL) failures |= 512;
	if (engine->get_cur_price("SHFE.au.2406") != 0.0) failures |= 1024;
	if (engine->get_day_price("SHFE.au.2406", 3) != 0.0) failures |= 2048;
	if (engine->get_exright_factor("SHFE.au.2406") != 1.0) failures |= 4096;
	engine->sub_tick(1, "SHFE.au.HOT");
	engine->sub_tick(1, "SHFE.au.2406");

	engine->~Engine();
	return failures;
}
}

TEST(S5EngineUninitMetadata, ZeroInitializedSelEngineReportsNoMetadata)
{
	EXPECT_EXIT(std::exit(probeUninitializedEngine<WtSelEngine>(0x00)),
		::testing::ExitedWithCode(0), "");
}

TEST(S5EngineUninitMetadata, IndeterminateStorageSelEngineReportsNoMetadata)
{
	EXPECT_EXIT(std::exit(probeUninitializedEngine<WtSelEngine>(0xA5)),
		::testing::ExitedWithCode(0), "");
}

TEST(S5EngineUninitMetadata, ZeroInitializedCtaAndHftEnginesReportNoMetadata)
{
	EXPECT_EXIT(std::exit(probeUninitializedEngine<WtCtaEngine>(0x00)),
		::testing::ExitedWithCode(0), "");
	EXPECT_EXIT(std::exit(probeUninitializedEngine<WtHftEngine>(0x00)),
		::testing::ExitedWithCode(0), "");
}
