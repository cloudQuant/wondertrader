#include "Share/ModuleHelper.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>

extern "C" const char* module_helper_path_from_tu_a();
extern "C" const char* module_helper_path_from_tu_b();

namespace
{
bool sameFile(const char* actual, const char* expected)
{
	if (actual == nullptr || expected == nullptr)
		return false;

	std::error_code error;
	const bool equivalent = std::filesystem::equivalent(actual, expected, error);
	return !error && equivalent;
}
}

TEST(ModuleHelperOdr, EachTranslationUnitReportsItsOwningModule)
{
	EXPECT_TRUE(sameFile(module_helper_path_from_tu_a(), MODULE_HELPER_ODR_FIXTURE_PATH));
	EXPECT_TRUE(sameFile(module_helper_path_from_tu_b(), MODULE_HELPER_ODR_FIXTURE_PATH));
	EXPECT_TRUE(sameFile(getInstPath().c_str(), MODULE_HELPER_ODR_TEST_PATH));
	EXPECT_FALSE(sameFile(module_helper_path_from_tu_a(), MODULE_HELPER_ODR_TEST_PATH));
}
