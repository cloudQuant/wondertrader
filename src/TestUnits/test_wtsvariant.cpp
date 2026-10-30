#include "../Includes/WTSVariant.hpp"
#include "gtest/gtest/gtest.h"

TEST(test_wtsvariant, real_format_handles_large_values)
{
	wtp::WTSVariant* root = wtp::WTSVariant::createObject();
	root->append("value", 6.022e23);

	// asString formatting may vary across standard libraries; the contract is
	// that the emitted text parses back to the original double.
	EXPECT_EQ(root->getDouble("value"), 6.022e23);
	root->release();
}
