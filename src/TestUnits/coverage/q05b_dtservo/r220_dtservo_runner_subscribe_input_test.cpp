/*
 * Public subscription input regressions for WtDtRunner.
 *
 * The runner is initialized with an empty data config: public calls still use
 * the real WtDataManager object, whose absent reader makes bar subscription a
 * deterministic no-I/O path. Tick delivery is observed through the public
 * callback path; no parser, network connection, or storage plugin is loaded.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <WtDtServo/WtDtRunner.h>

namespace {

std::vector<std::string>& tickCodes()
{
	static std::vector<std::string> codes;
	return codes;
}

void recordTick(const char* stdCode, WTSTickStruct*)
{
	tickCodes().emplace_back(stdCode == nullptr ? "<null>" : stdCode);
}

bool initializeWithoutStore(WtDtRunner& runner)
{
	static const char* config = R"JSON({"disable_dump":true,"basefiles":{}})JSON";
	runner.initialize(config, false, "", "/tmp/wt_runner_subscribe_input_no_logcfg.yaml",
		recordTick, nullptr);
	return runner.isInitialized();
}

WTSTickData* makeTick(const char* code = "IC2204")
{
	WTSTickStruct tick {};
	std::strncpy(tick.code, code, sizeof(tick.code) - 1);
	return WTSTickData::create(tick);
}

} // namespace

TEST(WtDtRunnerSubscribeInput, TickRejectsNullAndEmptyTokensWithoutClearingValidSubscriptions)
{
	tickCodes().clear();
	WtDtRunner runner;
	ASSERT_TRUE(initializeWithoutStore(runner));
	WTSTickData* tick = makeTick();
	ASSERT_NE(tick, nullptr);

	EXPECT_NO_THROW(runner.sub_tick(nullptr, true, false));
	EXPECT_NO_THROW(runner.sub_tick("", true, false));
	EXPECT_NO_THROW(runner.sub_tick(",,,", true, false));
	EXPECT_NO_THROW(runner.sub_tick(nullptr, true, true));
	EXPECT_NO_THROW(runner.sub_tick("", true, true));

	// Invalid replace requests are no-ops: the previous valid public
	// subscription stays active and no empty-code subscription is created.
	runner.sub_tick("CFFEX.IC.2204", true, false);
	runner.sub_tick(nullptr, true, false);
	runner.sub_tick("", true, false);
	runner.sub_tick(",,,", true, false);
	runner.trigger_tick("", tick);
	EXPECT_TRUE(tickCodes().empty());
	runner.trigger_tick("CFFEX.IC.2204", tick);
	ASSERT_EQ(tickCodes().size(), 1u);
	EXPECT_EQ(tickCodes().back(), "CFFEX.IC.2204");

	// Empty fields around/between valid codes are skipped while both valid
	// codes from the same public request remain subscribed.
	runner.sub_tick(",CFFEX.IC.2204,,DCE.i.2410,", true, false);
	runner.trigger_tick("", tick);
	runner.trigger_tick("CFFEX.IC.2204", tick);
	runner.trigger_tick("DCE.i.2410", tick);
	ASSERT_EQ(tickCodes().size(), 3u);
	EXPECT_EQ(tickCodes()[1], "CFFEX.IC.2204");
	EXPECT_EQ(tickCodes()[2], "DCE.i.2410");

	tick->release();
}

TEST(WtDtRunnerSubscribeInput, BarRejectsNullAndEmptyCodeOrPeriodAndAcceptsValidPair)
{
	tickCodes().clear();
	WtDtRunner runner;
	ASSERT_TRUE(initializeWithoutStore(runner));
	WTSTickData* tick = makeTick();
	ASSERT_NE(tick, nullptr);

	EXPECT_NO_THROW(runner.sub_bar(nullptr, "m1"));
	EXPECT_NO_THROW(runner.sub_bar("", "m1"));
	EXPECT_NO_THROW(runner.sub_bar("CFFEX.IC.2204", nullptr));
	EXPECT_NO_THROW(runner.sub_bar("CFFEX.IC.2204", ""));

	// With the no-reader manager, the valid call exercises the public bar
	// subscription and its internal tick path without storage/network I/O.
	EXPECT_NO_THROW(runner.sub_bar("CFFEX.IC.2204", "m1"));
	EXPECT_NO_THROW(runner.trigger_tick("CFFEX.IC.2204", tick));

	tick->release();
}

TEST(WtDtRunnerSubscribeInput, OuterSuffixOnlyReplaceKeepsExistingCodeAndSkipsEmptyBase)
{
	tickCodes().clear();
	WtDtRunner runner;
	ASSERT_TRUE(initializeWithoutStore(runner));
	WTSTickData* tick = makeTick();
	ASSERT_NE(tick, nullptr);

	runner.sub_tick("CFFEX.IC.2204", true, false);
	runner.sub_tick("+", true, false);
	runner.trigger_tick("CFFEX.IC.2204", tick);
	EXPECT_EQ(tickCodes().size(), 1u) << "suffix-only '+' must not replace the valid subscription";
	if (!tickCodes().empty())
		EXPECT_EQ(tickCodes().back(), "CFFEX.IC.2204");

	runner.sub_tick("CFFEX.IC.2204", true, false);
	runner.sub_tick("-", true, false);
	runner.trigger_tick("CFFEX.IC.2204", tick);
	EXPECT_EQ(tickCodes().size(), 2u) << "suffix-only '-' must not replace the valid subscription";
	if (tickCodes().size() >= 2)
		EXPECT_EQ(tickCodes().back(), "CFFEX.IC.2204");

	// A suffix-only field must not register the empty base code, while the
	// neighboring valid qfq code remains subscribed and keeps its suffix flag.
	runner.sub_tick("-,CFFEX.IC.2204-", true, false);
	runner.trigger_tick("", tick);
	EXPECT_EQ(tickCodes().size(), 2u);
	runner.trigger_tick("CFFEX.IC.2204", tick);
	EXPECT_EQ(tickCodes().size(), 3u);
	if (tickCodes().size() >= 3)
		EXPECT_EQ(tickCodes().back(), "CFFEX.IC.2204-");

	tick->release();
}

TEST(WtDtRunnerSubscribeInput, InnerSuffixOnlyReplacePreservesValidQfqAndDoesNotMutateEmptyRoute)
{
	tickCodes().clear();
	WtDtRunner runner;
	ASSERT_TRUE(initializeWithoutStore(runner));

	runner.sub_tick("CFFEX.IC.2204-", true, true);
	runner.sub_tick("+", true, true);
	WTSTickData* validTick = makeTick("ORIGINAL");
	ASSERT_NE(validTick, nullptr);
	runner.trigger_tick("CFFEX.IC.2204", validTick);
	EXPECT_STREQ(validTick->code(), "CFFEX.IC.2204-")
		<< "suffix-only '+' must not replace the existing inner qfq subscription";
	EXPECT_TRUE(tickCodes().empty()) << "inner subscriptions must not invoke the external callback";
	validTick->release();

	// Reset the inner map before testing the empty qfq route. On the old code,
	// the preceding '+' leaves an empty-base hfq entry; retaining it would make
	// trigger_tick("", ...) enter hfq factor adjustment instead of the assertion.
	runner.sub_tick("CFFEX.IC.2204-", true, true);
	runner.sub_tick("-", false, true);
	WTSTickData* emptyRouteTick = makeTick("ORIGINAL");
	ASSERT_NE(emptyRouteTick, nullptr);
	runner.trigger_tick("", emptyRouteTick);
	EXPECT_STREQ(emptyRouteTick->code(), "ORIGINAL")
		<< "suffix-only '-' must not create an inner subscription for the empty base code";
	EXPECT_TRUE(tickCodes().empty());
	emptyRouteTick->release();
}

