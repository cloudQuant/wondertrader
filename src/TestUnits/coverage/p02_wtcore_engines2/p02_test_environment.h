#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

#include <WtCore/WtHelper.h>

namespace p02test {

namespace fs = std::filesystem;

inline fs::path temp_root_path()
{
	return fs::temp_directory_path() /
		("p02_wtcore_" + std::to_string(static_cast<long long>(::getpid())));
}

inline std::string temp_root()
{
	return temp_root_path().string() + "/";
}

class GenerateDirEnvironment final : public ::testing::Environment
{
public:
	void SetUp() override
	{
		const fs::path path = temp_root_path();
		std::error_code ec;
		fs::remove_all(path, ec);
		ec.clear();
		fs::create_directories(path, ec);
		if (ec)
			throw std::system_error(ec, "create P02 test temporary root");

		const std::string genDir = path.string() + "/";
		WtHelper::setGenerateDir(genDir.c_str());
	}
};

} // namespace p02test
