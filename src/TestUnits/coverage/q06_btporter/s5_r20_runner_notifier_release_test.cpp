/*
 * R20_wtbtcore#3 (decision card 9: 9a=A+C, 9b=A, 9c=A): runner release and the
 * backtest event notifier.
 *
 * Contract: release_backtest() stops/joins the replay first, then drains the
 * notifier and destroys its MQ server exactly once before returning; the
 * later runner/EventNotifier destruction does not destroy again. The MQ module
 * itself has no versioned callback-quiescence capability and stays resident.
 *
 * Uses the R160 fake WtMsgQue module (WT_R66_FAKE_MQ_DIR, same convention as
 * r66_btporter_notifier_test.cpp); no real MQ server or socket is created.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <new>
#include <string>
#include <thread>
#include <unistd.h>

#include "Share/DLLHelper.hpp"
#include "WtBtCore/WtHelper.h"
#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace
{
	namespace fs = std::filesystem;

	void reset_singleton()
	{
		WtBtRunner& runner = getRunner();
		runner.~WtBtRunner();
		new (&runner) WtBtRunner();
	}

	struct RunnerReset
	{
		~RunnerReset() { reset_singleton(); }
	};

	struct InstDirRestore
	{
		std::string previous = WtHelper::getInstDir();
		~InstDirRestore() { WtHelper::setInstDir(previous.c_str()); }
	};

	struct EmptySink : IDataSink
	{
		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override {}
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override {}
	};

	struct FakeMq
	{
		using Reset = void(*)();
		using Count = uint32_t(*)();
		using Id = uint32_t(*)();

		DllHandle handle = nullptr;
		Reset reset = nullptr;
		Count create_count = nullptr;
		Count destroy_count = nullptr;
		Count publish_count = nullptr;
		Id destroyed_id = nullptr;

		~FakeMq() { if (handle) DLLHelper::free_library(handle); }
	};

	bool load_fake_mq(FakeMq& fake, const std::string& modulePath)
	{
		fake.handle = DLLHelper::load_library(modulePath.c_str());
		if (!fake.handle)
			return false;
		fake.reset = reinterpret_cast<FakeMq::Reset>(DLLHelper::get_symbol(fake.handle, "r160_fake_reset"));
		fake.create_count = reinterpret_cast<FakeMq::Count>(DLLHelper::get_symbol(fake.handle, "r160_fake_create_count"));
		fake.destroy_count = reinterpret_cast<FakeMq::Count>(DLLHelper::get_symbol(fake.handle, "r160_fake_destroy_count"));
		fake.publish_count = reinterpret_cast<FakeMq::Count>(DLLHelper::get_symbol(fake.handle, "r160_fake_publish_count"));
		fake.destroyed_id = reinterpret_cast<FakeMq::Id>(DLLHelper::get_symbol(fake.handle, "r160_fake_destroyed_id"));
		return fake.reset && fake.create_count && fake.destroy_count && fake.publish_count && fake.destroyed_id;
	}

	std::string make_config(const std::string& directory, const std::string& url)
	{
		return "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + directory +
			"\",\"stime\":\"202401020000\",\"etime\":\"202401030000\",\"basefiles\":{}},"
			"\"env\":{\"mocker\":\"none\"},\"notifier\":{\"active\":true,\"url\":\"" + url + "\"}}";
	}

	class S5R20RunnerNotifierRelease : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			const char* fakeDirValue = std::getenv("WT_R66_FAKE_MQ_DIR");
			if (!fakeDirValue || fakeDirValue[0] == '\0')
				GTEST_SKIP() << "set WT_R66_FAKE_MQ_DIR to the isolated R160 fake WtMsgQue module directory";
			_fakeDir = fakeDirValue;
			const std::string moduleName = DLLHelper::wrap_module("WtMsgQue", "lib");
			_modulePath = (fs::path(_fakeDir) / moduleName).string();
			if (!fs::exists(_modulePath))
				GTEST_SKIP() << "R160 fake WtMsgQue module is not present at " << _modulePath;
			// EventNotifier searches the cached CWD first; never fall through to a
			// production WtMsgQue placed there.
			const std::string cwdModule = (fs::path(WtHelper::getCWD()) / moduleName).string();
			if (fs::exists(cwdModule) && !fs::equivalent(cwdModule, _modulePath))
				GTEST_SKIP() << "run this test with an isolated CWD so the fake module wins lookup";

			reset_singleton();
			WtHelper::setInstDir((fs::path(_fakeDir).string() + fs::path::preferred_separator).c_str());
			ASSERT_TRUE(load_fake_mq(_fake, _modulePath)) << _modulePath;
			_fake.reset();

			_tempDir = fs::temp_directory_path() /
				("s5_r20_runner_notifier_" + std::to_string(static_cast<long long>(::getpid())));
			std::error_code ec;
			fs::remove_all(_tempDir, ec);
			fs::create_directories(_tempDir, ec);
			ASSERT_FALSE(ec) << ec.message();
		}

		void TearDown() override
		{
			std::error_code ec;
			if (!_tempDir.empty())
				fs::remove_all(_tempDir, ec);
		}

		void configure()
		{
			char error[256] = {};
			ASSERT_EQ(0, config_backtest_v2(make_config(_tempDir.string(), "fake://s5_r20").c_str(), false,
				error, sizeof(error))) << error;
			ASSERT_EQ(1u, _fake.create_count());
			ASSERT_EQ(0u, _fake.destroy_count());
		}

		RunnerReset _resetRunner;
		InstDirRestore _restoreInstDir;
		FakeMq _fake;
		std::string _fakeDir;
		std::string _modulePath;
		fs::path _tempDir;
	};
}

// Configured but never run: release destroys the server once; destruction does not repeat it.
TEST_F(S5R20RunnerNotifierRelease, IdleReleaseDestroysServerOnceBeforeRunnerDestruction)
{
	configure();
	release_backtest();
	EXPECT_EQ(1u, _fake.destroy_count()) << "release_backtest must destroy the notifier server";
	EXPECT_EQ(0xF1234567u, _fake.destroyed_id());
	release_backtest();
	EXPECT_EQ(1u, _fake.destroy_count()) << "repeated release must not destroy again";
	reset_singleton();
	EXPECT_EQ(1u, _fake.destroy_count()) << "runner destruction must not destroy again";
	EXPECT_EQ(1u, _fake.create_count());
}

// After a synchronous replay published events, release destroys once and no
// later publication reaches the plugin.
TEST_F(S5R20RunnerNotifierRelease, SyncReplayThenReleaseDestroysOnceAndStopsPublishing)
{
	configure();
	EmptySink sink;
	getRunner().replayer().register_sink(&sink, "s5_r20_sink");
	run_backtest(false, false);
	const uint32_t published = _fake.publish_count();
	EXPECT_GT(published, 0u);
	release_backtest();
	EXPECT_EQ(1u, _fake.destroy_count());
	EXPECT_EQ(published, _fake.publish_count());
	reset_singleton();
	EXPECT_EQ(1u, _fake.destroy_count());
	EXPECT_EQ(published, _fake.publish_count());
}

// Release requested while an asynchronous replay may still be running: the
// runner joins the replay before destroying; nothing publishes afterwards.
TEST_F(S5R20RunnerNotifierRelease, AsyncReplayReleaseJoinsThenDestroysOnce)
{
	configure();
	EmptySink sink;
	getRunner().replayer().register_sink(&sink, "s5_r20_async_sink");
	run_backtest(false, true);
	release_backtest();
	EXPECT_EQ(1u, _fake.destroy_count());
	const uint32_t published = _fake.publish_count();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(published, _fake.publish_count()) << "publication reached the plugin after release";
	reset_singleton();
	EXPECT_EQ(1u, _fake.destroy_count());
}
