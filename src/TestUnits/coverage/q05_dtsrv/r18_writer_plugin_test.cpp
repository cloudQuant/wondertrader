#include <gtest/gtest.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define WT_R18_TEST_PID _getpid
#else
#include <unistd.h>
#define WT_R18_TEST_PID getpid
#endif

namespace fs = std::filesystem;

namespace {

void set_trace_path(const std::string& path)
{
#ifdef _WIN32
	_putenv_s("WT_R18_WRITER_TRACE", path.c_str());
#else
	setenv("WT_R18_WRITER_TRACE", path.c_str(), 1);
#endif
}

void clear_trace_path()
{
#ifdef _WIN32
	_putenv_s("WT_R18_WRITER_TRACE", "");
#else
	unsetenv("WT_R18_WRITER_TRACE");
#endif
}

class R18WriterPluginTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static std::atomic<unsigned> sequence{ 0 };
		_dir = fs::temp_directory_path() / ("r18_writer_plugin_"
			+ std::to_string(WT_R18_TEST_PID()) + "_" + std::to_string(sequence++));
		fs::create_directories(_dir);
		_oldModuleDir = WtHelper::get_module_dir();
		WtHelper::set_module_dir(WT_R18_WRITER_MODULE_DIR);
		_trace = _dir / "events.log";
		set_trace_path(_trace.string());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir(_oldModuleDir.c_str());
		clear_trace_path();
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadParams(const char* module)
	{
		const fs::path config = _dir / "writer.json";
		std::ofstream(config.string()) << "{\"module\":\"" << module << "\"}";
		return WTSCfgLoader::load_from_file(config.string().c_str());
	}

	std::vector<std::string> events() const
	{
		std::vector<std::string> result;
		std::ifstream input(_trace.string());
		std::string line;
		while (std::getline(input, line))
			result.push_back(line);
		return result;
	}

	void expectEvents(std::initializer_list<const char*> expected) const
	{
		std::vector<std::string> values;
		for (const char* value : expected)
			values.emplace_back(value);
		EXPECT_EQ(events(), values);
	}

	fs::path _dir;
	fs::path _trace;
	std::string _oldModuleDir;
};

TEST_F(R18WriterPluginTest, SuccessfulDynamicWriterIsReleasedAndDestroyedOnce)
{
	WTSVariant* params = loadParams(WT_R18_WRITER_SUCCESS_MODULE);
	ASSERT_NE(params, nullptr);
	{
		DataManager manager;
		ASSERT_TRUE(manager.init(params, nullptr, nullptr));
		EXPECT_TRUE(manager.writeTick(nullptr, 0));
		manager.release();
		manager.release();
		EXPECT_FALSE(manager.writeTick(nullptr, 0));
	}
	params->release();

	expectEvents({ "module-load", "factory", "init-true", "release", "delete", "destroy" });
}

TEST_F(R18WriterPluginTest, MissingCreateSymbolRejectsLoadedModule)
{
	WTSVariant* params = loadParams(WT_R18_WRITER_MISSING_CREATE_MODULE);
	ASSERT_NE(params, nullptr);
	DataManager manager;
	EXPECT_FALSE(manager.init(params, nullptr, nullptr));
	manager.release();
	params->release();

	expectEvents({ "module-load" });
}

TEST_F(R18WriterPluginTest, MissingDeleteSymbolRejectsBeforeCallingFactory)
{
	WTSVariant* params = loadParams(WT_R18_WRITER_MISSING_DELETE_MODULE);
	ASSERT_NE(params, nullptr);
	DataManager manager;
	EXPECT_FALSE(manager.init(params, nullptr, nullptr));
	manager.release();
	params->release();

	expectEvents({ "module-load" });
}

TEST_F(R18WriterPluginTest, NullFactoryResultIsRejected)
{
	WTSVariant* params = loadParams(WT_R18_WRITER_NULL_FACTORY_MODULE);
	ASSERT_NE(params, nullptr);
	DataManager manager;
	EXPECT_FALSE(manager.init(params, nullptr, nullptr));
	manager.release();
	params->release();

	expectEvents({ "module-load", "factory" });
}

TEST_F(R18WriterPluginTest, InitFalseReleasesAndDeletesWriter)
{
	WTSVariant* params = loadParams(WT_R18_WRITER_INIT_FALSE_MODULE);
	ASSERT_NE(params, nullptr);
	DataManager manager;
	EXPECT_FALSE(manager.init(params, nullptr, nullptr));
	EXPECT_FALSE(manager.writeTick(nullptr, 0));
	manager.release();
	params->release();

	expectEvents({ "module-load", "factory", "init-false", "release", "delete", "destroy" });
}

} // namespace
