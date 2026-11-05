/**
 * BUG-N05-003 / decision K41 (2026-11-04): a V2 execution plugin that does not
 * declare WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY is accepted with its limits
 * (non-shared owner mode, no automatic timeout cancel) and the host says so
 * once at load time with a WARN; a plugin declaring the capability loads
 * without that diagnostic. Both plugins still load and create units.
 *
 * Fixtures: valid (COV_EXEC_PLUGIN_MODE=0, no identity capability) and
 * host_identity (COV_EXEC_PLUGIN_MODE=28, capability declared), both built
 * from fake_exec_plugin.cpp.
 */
#include <gtest/gtest.h>

#include "WtCore/WtExecuterFactory.h"
#include "Includes/ILogHandler.h"
#include "WTSTools/WTSLogger.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#ifndef WT_COV_EXEC_V2_VALID_PATH
#error WT_COV_EXEC_V2_VALID_PATH must name the valid plugin fixture
#endif
#ifndef WT_COV_EXEC_V2_HOST_IDENTITY_PATH
#error WT_COV_EXEC_V2_HOST_IDENTITY_PATH must name the identity-capable plugin fixture
#endif

namespace
{
namespace fs = std::filesystem;
using namespace wtp;

const char* const kK41Warning = "does not declare host order identity";

class S3K41PluginDir
{
public:
	explicit S3K41PluginDir(const char* source)
	{
		_path = fs::temp_directory_path() /
			("s3_k41_" + std::to_string(static_cast<long long>(::getpid())) +
				"_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		std::error_code ec;
		fs::remove_all(_path, ec);
		fs::create_directories(_path, ec);
		const fs::path input(source);
		fs::copy_file(input, _path / input.filename(), fs::copy_options::overwrite_existing, _copy_error);
	}

	~S3K41PluginDir()
	{
		std::error_code ec;
		fs::remove_all(_path, ec);
	}

	std::string path() const { return _path.string(); }
	const std::error_code& copyError() const { return _copy_error; }

private:
	fs::path _path;
	std::error_code _copy_error;
};

class S3K41WarnCapture final : public ILogHandler
{
public:
	S3K41WarnCapture()
	{
		static const char* loggerConfig = R"({"root":{"level":"debug","async":false,"sinks":[]}})";
		WTSLogger::init(loggerConfig, false, NULL);
		WTSLogger::registerHandler(this);
	}

	~S3K41WarnCapture() { WTSLogger::registerHandler(NULL); }

	void handleLogAppend(WTSLogLevel level, const char* message) override
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_messages.emplace_back(level, message == NULL ? std::string() : std::string(message));
	}

	// Number of WARN messages that carry the K41 diagnostic.
	size_t identityWarnings() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		size_t n = 0;
		for (const auto& m : _messages)
			if (m.first == LL_WARN && m.second.find(kK41Warning) != std::string::npos)
				++n;
		return n;
	}

	// The K41 text must never be emitted at another level either.
	size_t identityMessagesAtAnyLevel() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		size_t n = 0;
		for (const auto& m : _messages)
			if (m.second.find(kK41Warning) != std::string::npos)
				++n;
		return n;
	}

private:
	mutable std::mutex _mutex;
	std::vector<std::pair<WTSLogLevel, std::string>> _messages;
};
}

TEST(S3K41IdentityDiagnostic, PluginWithoutHostIdentityIsLoadedWithOneWarning)
{
	S3K41PluginDir root(WT_COV_EXEC_V2_VALID_PATH);
	ASSERT_FALSE(root.copyError()) << root.copyError().message();
	S3K41WarnCapture capture;
	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
		// The limit is accepted: the plugin still provides units.
		EXPECT_NE(nullptr, factory.createExeUnit("FakeV2Factory.Unit").get());
	}
	EXPECT_EQ(1u, capture.identityWarnings());
	EXPECT_EQ(1u, capture.identityMessagesAtAnyLevel());
}

TEST(S3K41IdentityDiagnostic, PluginDeclaringHostIdentityIsLoadedWithoutTheWarning)
{
	S3K41PluginDir root(WT_COV_EXEC_V2_HOST_IDENTITY_PATH);
	ASSERT_FALSE(root.copyError()) << root.copyError().message();
	S3K41WarnCapture capture;
	{
		WtExecuterFactory factory;
		ASSERT_TRUE(factory.loadFactories(root.path().c_str()));
		EXPECT_NE(nullptr, factory.createExeUnit("FakeV2Factory.Unit").get());
	}
	EXPECT_EQ(0u, capture.identityMessagesAtAnyLevel());
}
