#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#endif

#include "WtBtPorter/WtBtRunner.h"
#include "WtBtCore/HisDataReplayer.h"

namespace
{
	namespace fs = std::filesystem;

	struct CallbackGate
	{
		std::mutex mutex;
		std::condition_variable changed;
		bool entered = false;
		bool opened = false;
		bool reenterRelease = false;
		bool reenterReleaseReturned = false;
		size_t callbackCalls = 0;
	};

	enum GateStage
	{
		GATE_ON_INIT,
		GATE_ON_REPLAY_DONE
	};

	class GateSink : public IDataSink
	{
	public:
		GateSink(WtBtRunner& runner, CallbackGate& gate, GateStage stage)
			: _runner(runner), _gate(gate), _stage(stage) {}

		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override
		{
			if (_stage == GATE_ON_INIT)
				block();
		}
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override
		{
			if (_stage == GATE_ON_REPLAY_DONE)
				block();
		}

	private:
		void block()
		{
			if (_gate.reenterRelease)
			{
				_runner.release();
				std::lock_guard<std::mutex> lock(_gate.mutex);
				_gate.reenterReleaseReturned = true;
				_gate.changed.notify_all();
			}
			std::unique_lock<std::mutex> lock(_gate.mutex);
			++_gate.callbackCalls;
			_gate.entered = true;
			_gate.changed.notify_all();
			_gate.changed.wait(lock, [this]() { return _gate.opened; });
		}

		WtBtRunner& _runner;
		CallbackGate& _gate;
		GateStage _stage;
	};

	class N04ReplaySentinel : public IDataSink
	{
	public:
		uint32_t init = 0;
		uint32_t done = 0;

		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override { ++init; }
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override { ++done; }
	};

	fs::path prepare_dir(const char* tag)
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		fs::path root = fs::temp_directory_path() /
			(std::string("r222_bt_runner_") + tag + "_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(stamp));
		std::error_code ec;
		fs::remove_all(root, ec);
		fs::create_directories(root, ec);
		return root;
	}

	void write_text(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << content;
	}

	std::string json_escape(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size());
		for (char ch : value)
		{
			if (ch == '\\' || ch == '"')
				escaped.push_back('\\');
			escaped.push_back(ch);
		}
		return escaped;
	}

	void write_basefiles(const fs::path& root)
	{
		write_text(root / "sessions.json",
			R"({"TRADING":{"name":"Trading","offset":0,"sections":[{"from":0,"to":2359}]}})");
		write_text(root / "commodities.json",
			R"({"SHFE":{"rb":{"name":"Rebar","session":"TRADING","holiday":"CHINA","pricetick":1,"volscale":10,"covermode":0,"pricemode":0,"trademode":0,"category":1}}})");
		write_text(root / "contracts.json",
			R"({"SHFE":{"rb2510":{"name":"rb2510","product":"rb","exchg":"SHFE"}}})");
		// prepare() maps the initial trading date with checked mapping (card 1
		// A+C), which requires the default calendar; same fixture fix as I03-NEW-018.
		write_text(root / "holidays.json", R"({"CHINA":[]})");
	}

	std::string no_mocker_config(const fs::path& root)
	{
		const std::string rootPath = json_escape(root.string());
		const std::string sessionsPath = json_escape((root / "sessions.json").string());
		const std::string commoditiesPath = json_escape((root / "commodities.json").string());
		const std::string contractsPath = json_escape((root / "contracts.json").string());
		std::ostringstream cfg;
		cfg << "{\"replayer\":{"
			<< "\"mode\":\"none\",\"path\":\"" << rootPath << "\","
			<< "\"stime\":20240102000000,\"etime\":20240103000000,\"tick\":false,"
			<< "\"basefiles\":{\"session\":\"" << sessionsPath
			<< "\",\"commodity\":\"" << commoditiesPath
			<< "\",\"contract\":\"" << contractsPath
			<< "\",\"holiday\":\"" << json_escape((root / "holidays.json").string()) << "\"}},"
			<< "\"env\":{\"mocker\":\"none\"}}";
		return cfg.str();
	}

	std::string exec_config(const fs::path& root, const std::string& module,
		const std::string& execUnit = "WtMinImpactExeUnit", bool csvReplay = false)
	{
		const std::string rootPath = json_escape(root.string());
		const std::string sessionsPath = json_escape((root / "sessions.json").string());
		const std::string commoditiesPath = json_escape((root / "commodities.json").string());
		const std::string contractsPath = json_escape((root / "contracts.json").string());
		std::ostringstream cfg;
		cfg << "{\"replayer\":{"
			<< "\"mode\":\"" << (csvReplay ? "csv" : "none") << "\",\"path\":\"" << rootPath << "\","
			<< "\"stime\":" << (csvReplay ? "20240102093000" : "20240102000000")
			<< ",\"etime\":" << (csvReplay ? "20240102093200" : "20240103000000")
			<< ",\"tick\":" << (csvReplay ? "true" : "false") << ","
			<< "\"basefiles\":{\"session\":\"" << sessionsPath
			<< "\",\"commodity\":\"" << commoditiesPath
			<< "\",\"contract\":\"" << contractsPath
			<< "\",\"holiday\":\"" << json_escape((root / "holidays.json").string()) << "\"}},"
			<< "\"env\":{\"mocker\":\"exec\"},"
			<< "\"exec\":{\"name\":\"r222_exec\",\"module\":\"" << json_escape(module)
			<< "\",\"code\":\"SHFE.rb.2510\",\"period\":\"m1\","
			<< "\"volunit\":0,\"volmode\":0,"
			<< "\"executer\":{\"name\":\"" << json_escape(execUnit) << "\",\"id\":\"r222_exec\","
			<< "\"params\":{\"offset\":0,\"expire\":0,\"pricemode\":0,\"span\":0,"
			<< "\"byrate\":false,\"lots\":1,\"rate\":1,\"minopenlots\":1}},"
			<< "\"matcher\":{\"cancelrate\":0}}}";
		return cfg.str();
	}

	fs::path executable_dir()
	{
#ifdef _WIN32
		char path[MAX_PATH] = { 0 };
		const DWORD size = GetModuleFileNameA(NULL, path, MAX_PATH);
		return size == 0 ? fs::current_path() : fs::path(path).parent_path();
#elif defined(__APPLE__)
		uint32_t size = 0;
		_NSGetExecutablePath(NULL, &size);
		std::vector<char> path(size + 1, 0);
		if (_NSGetExecutablePath(path.data(), &size) != 0)
			return fs::current_path();
		return fs::path(path.data()).parent_path();
#else
		char path[4096] = { 0 };
		const ssize_t size = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
		return size <= 0 ? fs::current_path() : fs::path(std::string(path, static_cast<size_t>(size))).parent_path();
#endif
	}

	std::string plugin_path(const char* basename)
	{
#ifdef _WIN32
		const char* suffix = ".dll";
#elif defined(__APPLE__)
		const char* suffix = ".dylib";
#else
		const char* suffix = ".so";
#endif
		fs::path dir = executable_dir();
		for (int depth = 0; depth != 8 && !dir.empty(); ++depth)
		{
			const fs::path candidates[] = {
				dir / "exec_v2_plugins" / (std::string(basename) + suffix),
				dir / "coverage" / "exec_v2_plugins" / (std::string(basename) + suffix),
				dir / "TestUnits" / "coverage" / "exec_v2_plugins" /
					(std::string(basename) + suffix),
				dir / "src" / "TestUnits" / "coverage" / "exec_v2_plugins" /
					(std::string(basename) + suffix)
			};
			for (const fs::path& candidate : candidates)
			{
				if (fs::exists(candidate))
					return candidate.string();
			}
			dir = dir.parent_path();
		}
		return std::string();
	}

	class ScopedEnvironment
	{
	public:
		ScopedEnvironment(const char* name, const std::string& value) : _name(name)
		{
			const char* prior = std::getenv(name);
			if (prior)
				_previous = prior;
#ifdef _WIN32
			_putenv_s(name, value.c_str());
#else
			setenv(name, value.c_str(), 1);
#endif
		}

		~ScopedEnvironment()
		{
#ifdef _WIN32
			_putenv_s(_name.c_str(), _previous.c_str());
#else
			if (_previous.empty())
				unsetenv(_name.c_str());
			else
				setenv(_name.c_str(), _previous.c_str(), 1);
#endif
		}

	private:
		std::string _name;
		std::string _previous;
	};

	std::string read_text(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream content;
		content << in.rdbuf();
		return content.str();
	}

	void wait_for_gate(CallbackGate& gate)
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		gate.changed.wait(lock, [&gate]() { return gate.entered; });
	}

	void open_gate(CallbackGate& gate)
	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		gate.opened = true;
		gate.changed.notify_all();
	}
}

TEST(WtBtRunnerShutdown, ImmediateStopWaitsForPrepareThenJoinsTheWorker)
{
	const fs::path root = prepare_dir("immediate_stop");
	write_basefiles(root);
	CallbackGate gate;
	WtBtRunner runner;
	GateSink sink(runner, gate, GATE_ON_INIT);
	runner.init((root / "missing_log.json").string().c_str(), true, root.string().c_str());
	std::string error;
	ASSERT_TRUE(runner.config_v2(no_mocker_config(root).c_str(), false, &error)) << error;
	runner.replayer().register_sink(&sink, "r222_init_gate");
	std::atomic<bool> runReturned(false);
	std::atomic<bool> stopReturned(false);
	std::thread runThread([&]() {
		runner.run(false, true);
		runReturned.store(true, std::memory_order_release);
	});
	wait_for_gate(gate);
	std::thread stopThread([&]() {
		runner.stop();
		stopReturned.store(true, std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(stopReturned.load(std::memory_order_acquire));
	open_gate(gate);
	runThread.join();
	stopThread.join();
	EXPECT_TRUE(runReturned.load(std::memory_order_acquire));
	EXPECT_TRUE(stopReturned.load(std::memory_order_acquire));
	// 决策 K46=B（2026-11-05 用户裁定）：isAsync() 表示是否启动过后台 worker；stop 在 prepare 期间生效时
	// 回放不启动 worker，isAsync() 为 false（改前期望 true，即“请求的运行模式”，见 I03-NEW-081）。
	EXPECT_FALSE(runner.isAsync());
	runner.release();
	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST(WtBtRunnerShutdown, ActiveAsyncRunRejectsDuplicateWithoutBlockingOrRestarting)
{
	const fs::path root = prepare_dir("duplicate_async_run");
	write_basefiles(root);
	CallbackGate gate;
	WtBtRunner runner;
	GateSink sink(runner, gate, GATE_ON_REPLAY_DONE);
	runner.init((root / "missing_log.json").string().c_str(), true, root.string().c_str());
	std::string error;
	ASSERT_TRUE(runner.config_v2(no_mocker_config(root).c_str(), false, &error)) << error;
	runner.replayer().register_sink(&sink, "r222_duplicate_run_gate");

	runner.run(false, true);
	wait_for_gate(gate);

	std::mutex resultMutex;
	std::condition_variable resultChanged;
	bool duplicateReturned = false;
	std::thread duplicateThread([&]() {
		runner.run(false, true);
		{
			std::lock_guard<std::mutex> lock(resultMutex);
			duplicateReturned = true;
		}
		resultChanged.notify_all();
	});
	bool returnedWhileReplayBlocked = false;
	{
		std::unique_lock<std::mutex> lock(resultMutex);
		returnedWhileReplayBlocked = resultChanged.wait_for(
			lock, std::chrono::seconds(1), [&duplicateReturned]() { return duplicateReturned; });
	}

	open_gate(gate);
	duplicateThread.join();
	runner.stop();

	size_t callbackCalls = 0;
	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		callbackCalls = gate.callbackCalls;
	}
	EXPECT_TRUE(returnedWhileReplayBlocked);
	EXPECT_EQ(1u, callbackCalls);
	runner.release();
	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST(WtBtRunnerShutdown, AsyncCallbackReleaseDefersUntilControlThreadCanJoin)
{
	const fs::path root = prepare_dir("callback_release");
	write_basefiles(root);
	CallbackGate gate;
	gate.reenterRelease = true;
	WtBtRunner runner;
	GateSink sink(runner, gate, GATE_ON_REPLAY_DONE);
	runner.init((root / "missing_log.json").string().c_str(), true, root.string().c_str());
	std::string error;
	ASSERT_TRUE(runner.config_v2(no_mocker_config(root).c_str(), false, &error)) << error;
	runner.replayer().register_sink(&sink, "r222_replay_done_gate");
	runner.run(false, true);
	wait_for_gate(gate);
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		gate.changed.wait(lock, [&gate]() { return gate.reenterReleaseReturned; });
	}

	std::atomic<bool> releaseReturned(false);
	std::atomic<bool> releaseStarted(false);
	std::thread releaseThread([&]() {
		releaseStarted.store(true, std::memory_order_release);
		runner.release();
		releaseReturned.store(true, std::memory_order_release);
	});
	while (!releaseStarted.load(std::memory_order_acquire))
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_FALSE(releaseReturned.load(std::memory_order_acquire));
	open_gate(gate);
	releaseThread.join();
	EXPECT_TRUE(releaseReturned.load(std::memory_order_acquire));
	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST(WtBtRunnerShutdown, FailedFactoryPreflightRetainsExecOwnersUntilRetry)
{
	const std::string module = plugin_path("factory_preflight_false");
	ASSERT_FALSE(module.empty()) << "missing coverage execution-plugin fixture factory_preflight_false";

	const fs::path root = prepare_dir("failed_preflight");
	write_basefiles(root);
	const fs::path callbackLog = root / "plugin_callbacks.log";
	ScopedEnvironment callbackLogEnv("WT_COV_EXEC_V2_LOG", callbackLog.string());
	WtBtRunner runner;
	runner.init((root / "missing_log.json").string().c_str(), true, root.string().c_str());
	std::string error;
	ASSERT_TRUE(runner.config_v2(exec_config(root, module).c_str(), false, &error)) << error;

	runner.release();
	const std::string firstAttempt = read_text(callbackLog);
	EXPECT_NE(std::string::npos, firstAttempt.find("can_wait_factory_quiesce"));
	EXPECT_EQ(std::string::npos, firstAttempt.find("destroy_unit"));
	EXPECT_EQ(std::string::npos, firstAttempt.find("destroy_factory"));

	runner.release();
	const std::string retry = read_text(callbackLog);
	EXPECT_NE(std::string::npos, retry.find("destroy_unit"));
	EXPECT_NE(std::string::npos, retry.find("destroy_factory"));
	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST(N04ExecMocker, InitFailureIsNotRegisteredWithReplayer)
{
	const std::string module = plugin_path("init_fail");
	ASSERT_FALSE(module.empty()) << "missing coverage execution-plugin fixture init_fail";

	const fs::path root = prepare_dir("init_failure");
	write_basefiles(root);
	const fs::path callbackLog = root / "plugin_callbacks.log";
	ScopedEnvironment callbackLogEnv("WT_COV_EXEC_V2_LOG", callbackLog.string());
	N04ReplaySentinel sentinel;
	WtBtRunner runner;
	runner.replayer().register_sink(&sentinel, "n04_exec_failure_sentinel");
	runner.init((root / "missing_log.json").string().c_str(), true, root.string().c_str());
	std::string error;
	EXPECT_FALSE(runner.config_v2(exec_config(root, module).c_str(), false, &error));
	EXPECT_NE(std::string::npos, error.find("controlled initialization failure"));

	// The public Runner failure path must leave the preinstalled replay sink in place.
	ASSERT_TRUE(runner.replayer().prepare());
	runner.replayer().run(false);
	EXPECT_EQ(1U, sentinel.init);
	EXPECT_EQ(1U, sentinel.done);

	const std::string events = read_text(callbackLog);
	EXPECT_LT(events.find("create_unit"), events.find("initialize_unit"));
	EXPECT_LT(events.find("initialize_unit"), events.find("destroy_unit"));
	EXPECT_LT(events.find("destroy_unit"), events.find("destroy_factory"));
	EXPECT_EQ(std::string::npos, events.find("request_quiesce"));
	EXPECT_EQ(std::string::npos, events.find("wait_quiesce"));
	std::error_code ec;
	fs::remove_all(root, ec);
}

TEST(N04ExecMocker, SuccessfulRunnerReplayCallsPluginAndClosesSafely)
{
	const std::string module = plugin_path("valid");
	ASSERT_FALSE(module.empty()) << "missing coverage execution-plugin fixture valid";

	const fs::path root = prepare_dir("successful_replay");
	write_basefiles(root);
	fs::create_directories(root / "csv");
	write_text(root / "csv" / "SHFE.rb.2510_m1.csv",
		"date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n"
		"20240102,931,100,102,99,101,1,101,0,0,101\n");
	const fs::path callbackLog = root / "plugin_callbacks.log";
	ScopedEnvironment callbackLogEnv("WT_COV_EXEC_V2_LOG", callbackLog.string());
	ScopedEnvironment tickGateEnv("WT_COV_EXEC_V2_TICK_GATE", "");
	WtBtRunner runner;
	runner.init((root / "missing_log.json").string().c_str(), true, root.string().c_str());
	std::string error;
	ASSERT_TRUE(runner.config_v2(exec_config(root, module, "BlockingUnit", true).c_str(), false, &error)) << error;

	// Synchronous run returns only after the replay and plugin tick callbacks finish.
	runner.run(false, false);
	runner.release();

	const std::string events = read_text(callbackLog);
	const size_t replayCallback = events.find("blocking_on_tick_enter");
	const size_t factoryPreflight = events.find("can_wait_factory_quiesce");
	const size_t requestQuiesce = events.find("request_quiesce");
	const size_t canWaitQuiesce = events.find("can_wait_quiesce");
	const size_t waitQuiesce = events.find("wait_quiesce");
	const size_t destroyUnit = events.find("destroy_unit");
	const size_t destroyFactory = events.find("destroy_factory");
	ASSERT_NE(std::string::npos, replayCallback);
	ASSERT_NE(std::string::npos, factoryPreflight);
	ASSERT_NE(std::string::npos, requestQuiesce);
	ASSERT_NE(std::string::npos, canWaitQuiesce);
	ASSERT_NE(std::string::npos, waitQuiesce);
	ASSERT_NE(std::string::npos, destroyUnit);
	ASSERT_NE(std::string::npos, destroyFactory);
	EXPECT_LT(replayCallback, factoryPreflight);
	EXPECT_LT(factoryPreflight, requestQuiesce);
	EXPECT_LT(requestQuiesce, canWaitQuiesce);
	EXPECT_LT(canWaitQuiesce, waitQuiesce);
	EXPECT_LT(waitQuiesce, destroyUnit);
	EXPECT_LT(destroyUnit, destroyFactory);
	std::error_code ec;
	fs::remove_all(root, ec);
}
