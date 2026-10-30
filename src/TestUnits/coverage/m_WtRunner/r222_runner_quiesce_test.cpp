#include <gtest/gtest.h>

#include <memory>

#include "WtRunner/WtRunner.h"

TEST(WtRunnerQuiesceTest, EmptyRuntimeShutdownCanBeRetried)
{
	// WtRunner is large because it embeds all engine managers. Keep it off the
	// test thread's stack, matching the existing runner test fixtures.
	std::unique_ptr<WtRunner> runner(new WtRunner());

	EXPECT_TRUE(runner->shutdown());
	EXPECT_TRUE(runner->shutdown());
}
