#include <unistd.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSObject.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "Share/StdUtils.hpp"
#include "WTSUtils/WTSCfgLoader.h"

#define private public
#include "WtCore/EventNotifier.h"
#undef private

#include "WtCore/WtHelper.h"

namespace
{
namespace fs = std::filesystem;

class TempDir
{
public:
	TempDir()
	{
		static std::atomic<uint64_t> sequence{ 0 };
		_path = fs::temp_directory_path() /
			("wt_r10_mq_missing_register_" +
				std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
				"_" + std::to_string(++sequence));
		std::error_code ec;
		fs::remove_all(_path, ec);
		fs::create_directories(_path, ec);
	}

	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(_path, ec);
	}

	std::string file(const char* name) const { return (_path / name).string(); }

private:
	fs::path _path;
};

void set_environment(const std::string& name, const char* value)
{
#if defined(_WIN32)
	_putenv_s(name.c_str(), value == nullptr ? "" : value);
#else
	if (value == nullptr)
		unsetenv(name.c_str());
	else
		setenv(name.c_str(), value, 1);
#endif
}

class ScopedEnvironment
{
public:
	ScopedEnvironment(std::string name, const std::string& value)
		: _name(std::move(name))
	{
		const char* previous = std::getenv(_name.c_str());
		if (previous != nullptr)
		{
			_hadPrevious = true;
			_previous = previous;
		}
		set_environment(_name, value.c_str());
	}

	~ScopedEnvironment()
	{
		set_environment(_name, _hadPrevious ? _previous.c_str() : nullptr);
	}

private:
	std::string _name;
	std::string _previous;
	bool _hadPrevious{ false };
};

class ScopedInstDir
{
public:
	ScopedInstDir() : _previous(WtHelper::getInstDir()) {}
	~ScopedInstDir() { WtHelper::setInstDir(_previous.c_str()); }
	void set(const std::string& path) { WtHelper::setInstDir(path.c_str()); }

private:
	std::string _previous;
};

struct ReleaseVariant
{
	void operator()(WTSVariant* value) const
	{
		if (value != nullptr)
			value->release();
	}
};

using VariantPtr = std::unique_ptr<WTSVariant, ReleaseVariant>;

VariantPtr active_config(const char* url)
{
	const std::string content = std::string("{\"active\":true,\"url\":\"") + url + "\"}";
	return VariantPtr(WTSCfgLoader::load_from_content(content.c_str()));
}

struct ScopedDll
{
	explicit ScopedDll(DllHandle value) : handle(value) {}
	~ScopedDll() { DLLHelper::free_library(handle); }
	DllHandle handle;
};
} // namespace

TEST(R10EventNotifier, MissingRegisterExportUnloadsCandidateAndPreservesActiveNotifier)
{
	const std::string goodDir = WT_R160_FAKE_MQ_DIR;
	const std::string badDir = WT_R10_MISSING_REGISTER_MQ_DIR;
	const std::string module = DLLHelper::wrap_module("WtMsgQue", "lib");
	if (fs::exists(fs::path(WtHelper::getCWD()) / module))
		GTEST_SKIP() << "cwd contains WtMsgQue; cannot select controlled fixture deterministically";
	ASSERT_TRUE(fs::exists(fs::path(goodDir) / module));
	ASSERT_TRUE(fs::exists(fs::path(badDir) / module));

	ScopedInstDir instDir;
	instDir.set(goodDir + "/");
	ScopedDll inspector(DLLHelper::load_library((goodDir + "/" + module).c_str()));
	ASSERT_NE(nullptr, inspector.handle);

	using Reset = void(*)();
	using Count = uint32_t(*)();
	using Id = WtUInt32(*)();
	using Text = const char*(*)();
	const auto reset = reinterpret_cast<Reset>(DLLHelper::get_symbol(inspector.handle, "r160_fake_reset"));
	const auto createCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector.handle, "r160_fake_create_count"));
	const auto destroyCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector.handle, "r160_fake_destroy_count"));
	const auto publishCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector.handle, "r160_fake_publish_count"));
	const auto serverId = reinterpret_cast<Id>(DLLHelper::get_symbol(inspector.handle, "r160_fake_server_id"));
	const auto destroyedId = reinterpret_cast<Id>(DLLHelper::get_symbol(inspector.handle, "r160_fake_destroyed_id"));
	const auto publishedId = reinterpret_cast<Id>(DLLHelper::get_symbol(inspector.handle, "r160_fake_published_id"));
	const auto topic = reinterpret_cast<Text>(DLLHelper::get_symbol(inspector.handle, "r160_fake_topic"));
	ASSERT_NE(nullptr, reset);
	ASSERT_NE(nullptr, createCount);
	ASSERT_NE(nullptr, destroyCount);
	ASSERT_NE(nullptr, publishCount);
	ASSERT_NE(nullptr, serverId);
	ASSERT_NE(nullptr, destroyedId);
	ASSERT_NE(nullptr, publishedId);
	ASSERT_NE(nullptr, topic);
	reset();

	TempDir temp;
	const std::string unloadedMarker = temp.file("candidate_unloaded");
	const std::string createdMarker = temp.file("candidate_created");
	const std::string destroyedMarker = temp.file("candidate_destroyed");
	const std::string publishedMarker = temp.file("candidate_published");
	ScopedEnvironment unloadEnv("WT_R10_MQ_UNLOAD_MARKER", unloadedMarker);
	ScopedEnvironment createEnv("WT_R10_MQ_CREATE_MARKER", createdMarker);
	ScopedEnvironment destroyEnv("WT_R10_MQ_DESTROY_MARKER", destroyedMarker);
	ScopedEnvironment publishEnv("WT_R10_MQ_PUBLISH_MARKER", publishedMarker);

	constexpr WtUInt32 expectedId = 0xF1234567u;
	const std::string firstUrl = "fake://r10_existing_notifier";
	VariantPtr goodCfg = active_config(firstUrl.c_str());
	ASSERT_NE(nullptr, goodCfg.get());

	{
		EventNotifier notifier;
		auto work = boost::asio::make_work_guard(notifier._core->io);
		ASSERT_TRUE(notifier.init(goodCfg.get()));
		ASSERT_EQ(expectedId, notifier._mq_sid);
		ASSERT_EQ(1u, createCount());
		ASSERT_EQ(0u, destroyCount());

		const auto originalCreator = notifier._creator;
		const auto originalRemover = notifier._remover;
		const auto originalPublisher = notifier._publisher;
		const auto originalRegister = notifier._register;
		const auto originalWorker = notifier._core->worker.get();

		instDir.set(badDir + "/");
		VariantPtr badCfg = active_config("fake://missing_register");
		ASSERT_NE(nullptr, badCfg.get());
		EXPECT_FALSE(notifier.init(badCfg.get()));

		EXPECT_EQ(expectedId, notifier._mq_sid);
		EXPECT_STREQ(firstUrl.c_str(), notifier._url.c_str());
		EXPECT_EQ(originalCreator, notifier._creator);
		EXPECT_EQ(originalRemover, notifier._remover);
		EXPECT_EQ(originalPublisher, notifier._publisher);
		EXPECT_EQ(originalRegister, notifier._register);
		EXPECT_EQ(originalWorker, notifier._core->worker.get());
		EXPECT_EQ(0u, destroyCount());
		EXPECT_TRUE(fs::exists(unloadedMarker));
		EXPECT_FALSE(fs::exists(createdMarker));
		EXPECT_FALSE(fs::exists(destroyedMarker));
		EXPECT_FALSE(fs::exists(publishedMarker));

		notifier.notify_event("existing notifier remains active");
		for (int i = 0; i < 500 && publishCount() == 0; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		EXPECT_EQ(1u, publishCount());
		EXPECT_EQ(expectedId, publishedId());
		EXPECT_STREQ("GRP_EVENT", topic());
		work.reset();
	}

	EXPECT_EQ(1u, destroyCount());
	EXPECT_EQ(expectedId, destroyedId());
}

TEST(R10EventNotifier, MissingCreateServerExportUnloadsWithoutStartingWorker)
{
	const std::string goodDir = WT_R160_FAKE_MQ_DIR;
	const std::string badDir = WT_R10_MISSING_CREATE_MQ_DIR;
	const std::string module = DLLHelper::wrap_module("WtMsgQue", "lib");
	if (fs::exists(fs::path(WtHelper::getCWD()) / module))
		GTEST_SKIP() << "cwd contains WtMsgQue; cannot select controlled fixture deterministically";
	ASSERT_TRUE(fs::exists(fs::path(goodDir) / module));
	ASSERT_TRUE(fs::exists(fs::path(badDir) / module));

	ScopedInstDir instDir;
	TempDir temp;
	const std::string loadedMarker = temp.file("missing_create_loaded");
	const std::string unloadedMarker = temp.file("missing_create_unloaded");
	const std::string callbackMarker = temp.file("missing_create_callback");
	const std::string destroyMarker = temp.file("missing_create_destroy");
	const std::string publishMarker = temp.file("missing_create_publish");
	ScopedEnvironment loadEnv("WT_R10_CREATE_MQ_LOAD_MARKER", loadedMarker);
	ScopedEnvironment unloadEnv("WT_R10_CREATE_MQ_UNLOAD_MARKER", unloadedMarker);
	ScopedEnvironment callbackEnv("WT_R10_CREATE_MQ_CALLBACK_MARKER", callbackMarker);
	ScopedEnvironment destroyEnv("WT_R10_CREATE_MQ_DESTROY_MARKER", destroyMarker);
	ScopedEnvironment publishEnv("WT_R10_CREATE_MQ_PUBLISH_MARKER", publishMarker);

	instDir.set(badDir + "/");
	VariantPtr badCfg = active_config("fake://missing_create_server");
	ASSERT_NE(nullptr, badCfg.get());
	{
		EventNotifier notifier;
		EXPECT_FALSE(notifier.init(badCfg.get()));
		EXPECT_EQ(0u, notifier._mq_sid);
		EXPECT_EQ(nullptr, notifier._creator);
		EXPECT_EQ(nullptr, notifier._register);
		EXPECT_EQ(nullptr, notifier._core->worker.get()) << "missing required export must not start a worker";
		EXPECT_TRUE(fs::exists(loadedMarker)) << "the controlled module must have loaded before rejection";
		EXPECT_TRUE(fs::exists(unloadedMarker)) << "the rejected candidate module must be released";
		EXPECT_FALSE(fs::exists(callbackMarker)) << "no plugin function may run before ABI validation";
		EXPECT_FALSE(fs::exists(destroyMarker));
		EXPECT_FALSE(fs::exists(publishMarker));
	}

	// Positive control: the same EventNotifier path accepts the normal fake MQ
	// module, invokes its required exports, and creates the worker.
	instDir.set(goodDir + "/");
	ScopedDll inspector(DLLHelper::load_library((goodDir + "/" + module).c_str()));
	ASSERT_NE(nullptr, inspector.handle);
	using Reset = void(*)();
	using Count = uint32_t(*)();
	using Id = WtUInt32(*)();
	const auto reset = reinterpret_cast<Reset>(DLLHelper::get_symbol(inspector.handle, "r160_fake_reset"));
	const auto createCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector.handle, "r160_fake_create_count"));
	const auto registerCount = reinterpret_cast<Count>(DLLHelper::get_symbol(inspector.handle, "r160_fake_register_count"));
	const auto serverId = reinterpret_cast<Id>(DLLHelper::get_symbol(inspector.handle, "r160_fake_server_id"));
	ASSERT_NE(nullptr, reset);
	ASSERT_NE(nullptr, createCount);
	ASSERT_NE(nullptr, registerCount);
	ASSERT_NE(nullptr, serverId);
	reset();

	constexpr WtUInt32 expectedId = 0xF1234567u;
	VariantPtr goodCfg = active_config("fake://valid_create_server");
	ASSERT_NE(nullptr, goodCfg.get());
	{
		EventNotifier notifier;
		ASSERT_TRUE(notifier.init(goodCfg.get()));
		EXPECT_EQ(expectedId, notifier._mq_sid);
		ASSERT_NE(nullptr, notifier._core->worker.get());
		EXPECT_EQ(1u, createCount());
		EXPECT_EQ(1u, registerCount());
		EXPECT_EQ(expectedId, serverId());
	}
}
