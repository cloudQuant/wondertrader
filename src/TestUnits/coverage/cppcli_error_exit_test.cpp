#include <gtest/gtest.h>

#include "Share/cppcli.hpp"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <system_error>
#include <vector>

namespace {

void parseTestCommand(std::initializer_list<const char *> args)
{
	std::cout.rdbuf(std::cerr.rdbuf());
	std::vector<char *> argv;
	argv.reserve(args.size());
	for (const char *arg : args)
		argv.push_back(const_cast<char *>(arg));

	cppcli::Option options(static_cast<int>(argv.size()), argv.data());
	options("-r", "--required", "Required value", true);
	options("-i", "--integer", "Integer value")->limitInt();
	options("-m", "--mode", "Mode")->limitOneOf("fast", "safe");
	options("-n", "--number", "Number")->limitInt()->limitNumRange(1, 9);
	options("-h", "--help", "Show help")->asHelpParam();
	options.parse();

	std::exit(EXIT_SUCCESS);
}

} // namespace

TEST(cppcli_path_init, current_and_executable_directories_are_available)
{
	auto args = ::testing::internal::GetArgvs();
	ASSERT_FALSE(args.empty());
	std::vector<char *> argv;
	argv.reserve(args.size());
	for (std::string &arg : args)
		argv.push_back(arg.data());

	cppcli::Option options(static_cast<int>(argv.size()), argv.data());
	std::error_code error;
	const std::filesystem::path currentDirectory = std::filesystem::current_path(error);
	ASSERT_FALSE(error);
	EXPECT_EQ(options.getExecPath(), currentDirectory.u8string());

	std::string executablePath;
	ASSERT_TRUE(cppcli::detail::getExecutablePath(executablePath));
	EXPECT_EQ(options.getWorkPath(),
	          cppcli::detail::executableDirectoryOrFallback(executablePath, options.getExecPath()));
	const std::filesystem::path executableDirectory = std::filesystem::u8path(options.getWorkPath());
	EXPECT_FALSE(executableDirectory.empty());
	EXPECT_TRUE(executableDirectory.is_absolute());
	EXPECT_TRUE(std::filesystem::is_directory(executableDirectory, error)) << error.message();
	EXPECT_FALSE(error);
}

TEST(cppcli_path_init, fallback_paths_are_defined_for_failed_lookups)
{
	EXPECT_EQ(cppcli::detail::currentDirectoryOrFallback(""), ".");
	EXPECT_EQ(cppcli::detail::currentDirectoryOrFallback("/work"), "/work");
	EXPECT_EQ(cppcli::detail::executableDirectoryOrFallback("", "/work"), "/work");
	EXPECT_EQ(cppcli::detail::executableDirectoryOrFallback("runner", "/work"), "/work");
	EXPECT_EQ(cppcli::detail::executableDirectoryOrFallback("/runner", "/work"), "/");
	EXPECT_EQ(cppcli::detail::executableDirectoryOrFallback("/opt/wt/bin/runner", "/work"),
	          "/opt/wt/bin");
#if defined(WIN32) || defined(_WIN32) || defined(_WIN64) || defined(__WIN32__)
	EXPECT_EQ(cppcli::detail::executableDirectoryOrFallback("C:\\runner.exe", "D:\\work"), "C:\\");
#endif
}

TEST(cppcli_error_exit, missing_required_option_returns_failure)
{
	EXPECT_EXIT(parseTestCommand({"cppcli-test"}), ::testing::ExitedWithCode(EXIT_FAILURE),
	            "Must enter this param");
}

TEST(cppcli_error_exit, invalid_type_returns_failure)
{
	EXPECT_EXIT(parseTestCommand({"cppcli-test", "--required", "value", "--integer", "abc"}),
	            ::testing::ExitedWithCode(EXIT_FAILURE), "correct type");
}

TEST(cppcli_error_exit, invalid_one_of_returns_failure)
{
	EXPECT_EXIT(parseTestCommand({"cppcli-test", "--required", "value", "--mode", "turbo"}),
	            ::testing::ExitedWithCode(EXIT_FAILURE), "one of these values");
}

TEST(cppcli_error_exit, out_of_range_returns_failure)
{
	EXPECT_EXIT(parseTestCommand({"cppcli-test", "--required", "value", "--number", "10"}),
	            ::testing::ExitedWithCode(EXIT_FAILURE), "within this range");
}

TEST(cppcli_error_exit, valid_arguments_return_success)
{
	EXPECT_EXIT(parseTestCommand({"cppcli-test", "--required", "value", "--integer", "7",
	                              "--mode", "safe", "--number", "9"}),
	            ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(cppcli_error_exit, help_returns_success_before_required_validation)
{
	EXPECT_EXIT(parseTestCommand({"cppcli-test", "--help"}), ::testing::ExitedWithCode(EXIT_SUCCESS),
	            "options:");
}
