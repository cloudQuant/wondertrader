/*
 * BUG-R66-001: backtest config retry and notifier ownership.
 *
 * This test uses the existing R160 fake WtMsgQue module, selected with
 * WT_R66_FAKE_MQ_DIR. It exercises the public config entry points and counts
 * the actual create/publish/destroy calls made by WtBtCore::EventNotifier.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <new>
#include <string>
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
		using Text = const char*(*)();

		DllHandle handle = nullptr;
		Reset reset = nullptr;
		Count create_count = nullptr;
		Count destroy_count = nullptr;
		Count publish_count = nullptr;
		Id destroyed_id = nullptr;
		Id published_id = nullptr;
		Text url = nullptr;
		Text topic = nullptr;
	};

	bool load_fake_mq(FakeMq& fake, const std::string& directory, std::string& modulePath)
	{
		modulePath = (fs::path(directory) / DLLHelper::wrap_module("WtMsgQue", "lib")).string();
		fake.handle = DLLHelper::load_library(modulePath.c_str());
		if (!fake.handle)
			return false;
		fake.reset = reinterpret_cast<FakeMq::Reset>(DLLHelper::get_symbol(fake.handle, "r160_fake_reset"));
		fake.create_count = reinterpret_cast<FakeMq::Count>(DLLHelper::get_symbol(fake.handle, "r160_fake_create_count"));
		fake.destroy_count = reinterpret_cast<FakeMq::Count>(DLLHelper::get_symbol(fake.handle, "r160_fake_destroy_count"));
		fake.publish_count = reinterpret_cast<FakeMq::Count>(DLLHelper::get_symbol(fake.handle, "r160_fake_publish_count"));
		fake.destroyed_id = reinterpret_cast<FakeMq::Id>(DLLHelper::get_symbol(fake.handle, "r160_fake_destroyed_id"));
		fake.published_id = reinterpret_cast<FakeMq::Id>(DLLHelper::get_symbol(fake.handle, "r160_fake_published_id"));
		fake.url = reinterpret_cast<FakeMq::Text>(DLLHelper::get_symbol(fake.handle, "r160_fake_url"));
		fake.topic = reinterpret_cast<FakeMq::Text>(DLLHelper::get_symbol(fake.handle, "r160_fake_topic"));
		return fake.reset && fake.create_count && fake.destroy_count && fake.publish_count &&
			fake.destroyed_id && fake.published_id && fake.url && fake.topic;
	}

	std::string make_config(const std::string& directory, const std::string& url, bool failReplayer,
		const std::string& mode = "none")
	{
		const std::string session = failReplayer
			? "\"session\":\"" + (fs::path(directory) / "missing-sessions.json").string() + "\""
			: std::string();
		return "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + directory +
			"\",\"stime\":\"202401020000\",\"etime\":\"202401030000\",\"basefiles\":{" +
			session + "}},\"env\":{\"mocker\":\"" + mode + "\"}," +
			(mode == "hft" ? "\"hft\":{\"name\":\"r66_hft\",\"module\":\"\"}," : "") +
			"\"notifier\":{\"active\":true,\"url\":\"" +
			url + "\"}}";
	}
}

TEST(R66BtPorterNotifier, FailedConfigRetryAndDuplicateConfigOwnOneServer)
{
	const char* fakeDirValue = std::getenv("WT_R66_FAKE_MQ_DIR");
	if (!fakeDirValue || fakeDirValue[0] == '\0')
		GTEST_SKIP() << "set WT_R66_FAKE_MQ_DIR to the isolated R160 fake WtMsgQue module directory";

	const std::string fakeDir = fakeDirValue;
	const std::string moduleName = DLLHelper::wrap_module("WtMsgQue", "lib");
	const std::string modulePath = (fs::path(fakeDir) / moduleName).string();
	if (!fs::exists(modulePath))
		GTEST_SKIP() << "R160 fake WtMsgQue module is not present at " << modulePath;

	// EventNotifier searches the cached CWD before WtHelper::getInstDir(). Never
	// allow this test to fall through to a real WtMsgQue library in the CWD.
	const std::string cwdModule = (fs::path(WtHelper::getCWD()) / moduleName).string();
	if (fs::exists(cwdModule) && !fs::equivalent(cwdModule, modulePath))
		GTEST_SKIP() << "run this test with an isolated CWD so the fake module wins lookup";

	RunnerReset resetRunner;
	reset_singleton();
	InstDirRestore restoreInstDir;
	WtHelper::setInstDir((fs::path(fakeDir).string() + fs::path::preferred_separator).c_str());

	FakeMq fake;
	std::string loadedModule;
	ASSERT_TRUE(load_fake_mq(fake, fakeDir, loadedModule)) << loadedModule;
	auto closeInspector = [&fake]() {
		if (fake.handle)
			DLLHelper::free_library(fake.handle);
	};
	struct InspectorClose
	{
		std::function<void()> close;
		~InspectorClose() { close(); }
	} inspectorClose{ closeInspector };
	fake.reset();

	const fs::path tempDir = fs::temp_directory_path() /
		("r66_bt_notifier_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code ec;
	fs::remove_all(tempDir, ec);
	fs::create_directories(tempDir, ec);
	ASSERT_FALSE(ec) << ec.message();

	const std::string firstUrl = "fake://r66_first";
	const std::string failedCfg = make_config(tempDir.string(), firstUrl, true);
	char error[256] = {};
	EXPECT_NE(0, config_backtest_v2(failedCfg.c_str(), false, error, sizeof(error)));
	EXPECT_EQ(0u, fake.create_count()) << "a replayer failure must happen before notifier server creation";
	EXPECT_EQ(0u, fake.destroy_count());

	const std::string correctedCfg = make_config(tempDir.string(), firstUrl, false);
	EXPECT_EQ(0, config_backtest_v2(correctedCfg.c_str(), false, error, sizeof(error))) << error;
	ASSERT_EQ(1u, fake.create_count());
	EXPECT_STREQ(firstUrl.c_str(), fake.url());

	// The legacy public wrapper shares the successful runner state and must not
	// reinitialize the non-reentrant notifier on a duplicate config call.
	const std::string duplicateCfg = make_config(tempDir.string(), "fake://r66_duplicate", false);
	config_backtest(duplicateCfg.c_str(), false);
	EXPECT_EQ(1u, fake.create_count());
	EXPECT_STREQ(firstUrl.c_str(), fake.url());

	// A successful public configuration leaves the notifier connected to the
	// replayer. A synchronous empty replay emits BT_START through the fake module.
	EmptySink sink;
	getRunner().replayer().register_sink(&sink, "r66_sink");
	run_backtest(false, false);
	EXPECT_GT(fake.publish_count(), 0u);
	EXPECT_EQ(0xF1234567u, fake.published_id());
	EXPECT_STREQ("BT_EVENT", fake.topic());

	reset_singleton();
	EXPECT_EQ(1u, fake.destroy_count());
	EXPECT_EQ(0xF1234567u, fake.destroyed_id());

	// A failure after successful replayer initialization remains permanently
	// non-retryable by contract; it must not initialize the notifier twice.
	fake.reset();
	const std::string failedModeUrl = "fake://r66_mode_failure";
	const std::string failedModeCfg = make_config(tempDir.string(), failedModeUrl, false, "hft");
	EXPECT_NE(0, config_backtest_v2(failedModeCfg.c_str(), false, error, sizeof(error)));
	EXPECT_EQ(1u, fake.create_count());
	const std::string unsupportedRetryCfg = make_config(tempDir.string(), failedModeUrl, false);
	EXPECT_NE(0, config_backtest_v2(unsupportedRetryCfg.c_str(), false, error, sizeof(error)));
	EXPECT_NE(std::string(error).find("retry is unsupported"), std::string::npos) << error;
	EXPECT_EQ(1u, fake.create_count());
	reset_singleton();
	EXPECT_EQ(1u, fake.destroy_count());
	EXPECT_EQ(0xF1234567u, fake.destroyed_id());

	fs::remove_all(tempDir, ec);
}
