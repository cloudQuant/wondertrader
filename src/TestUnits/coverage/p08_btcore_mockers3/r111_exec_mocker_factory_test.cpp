/**
 * r111_exec_mocker_factory_test.cpp  (lane R111)
 *
 * 靶子：src/WtBtCore/ExecMocker.cpp
 *   init 成功路径（L71-85）：真实构建产物 libWtExeFact.dylib + WtMinImpactExeUnit
 *   handle_init（L140-161）、handle_schedule 三种 volmode（L163-192）
 *   handle_entrust/handle_order/handle_trade（L285-335）与 handle_replay_done（L337-347）
 *
 * 此前 n04/r20/r92 只覆盖「模块缺失」分支，_exec_unit 非空路径整族留白。
 * 本文件通过 ExecMocker::init 的公开配置加载本机构建产物插件（不联网），
 * 执行单元只做纯内存撮合回报，不接真实柜台；并发回调测试使用可控本地线程。
 *
 * 不使用 #define private public；临时文件由 r62 fixture 写入系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <WtBtCore/ExecMocker.h>
#include <WtBtCore/WtHelper.h>

#include "r111_common.h"

namespace
{
	namespace fs = std::filesystem;
	const char* const kRb = "SHFE.rb.2405";

	WTSVariant* make_exec_cfg(const std::string& module, const char* id,
		double volunit, int32_t volmode, const char* unitName = "WtMinImpactExeUnit")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("module", module.c_str());
		cfg->append("code", kRb);
		cfg->append("period", "m1");
		cfg->append("volunit", volunit);
		cfg->append("volmode", volmode);

		WTSVariant* exec = WTSVariant::createObject();
		exec->append("name", unitName);
		exec->append("id", id);

		WTSVariant* params = WTSVariant::createObject();
		params->append("offset", static_cast<int32_t>(0));
		params->append("expire", static_cast<uint32_t>(0));
		params->append("pricemode", static_cast<int32_t>(0));
		params->append("span", static_cast<uint32_t>(0));
		params->append("byrate", false);
		params->append("lots", 1.0);
		params->append("rate", 1.0);
		params->append("minopenlots", 1.0);
		exec->append("params", params);
		params->release();

		cfg->append("executer", exec);
		exec->release();
		return cfg;
	}

	std::string read_text(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return std::string();
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	uint32_t line_count(const std::string& content)
	{
		if (content.empty())
			return 0;
		uint32_t lines = 0;
		for (char c : content)
		{
			if (c == '\n')
				lines++;
		}
		if (content.back() != '\n')
			lines++;
		return lines;
	}

	bool wait_for_text(const std::filesystem::path& p, const std::string& text,
		std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (read_text(p).find(text) != std::string::npos)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return read_text(p).find(text) != std::string::npos;
	}

	fs::path make_unique_test_temp_dir(const char* label)
	{
		static std::atomic<uint64_t> sequence(0);
		std::error_code ec;
		const fs::path tempRoot = fs::temp_directory_path(ec);
		if (ec)
			return fs::path();

		for (uint32_t attempt = 0; attempt < 100; ++attempt)
		{
			const auto now = std::chrono::steady_clock::now().time_since_epoch();
			const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
			const uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed);
			const fs::path candidate = tempRoot /
				(std::string("wt_r111_") + label + "_" + std::to_string(stamp) + "_" +
					std::to_string(serial));
			ec.clear();
			if (fs::create_directory(candidate, ec))
				return candidate;
			if (ec)
				return fs::path();
		}
		return fs::path();
	}

	class ScopedTestTempDir
	{
	public:
		explicit ScopedTestTempDir(const char* label)
			: _path(make_unique_test_temp_dir(label))
		{}

		~ScopedTestTempDir()
		{
			if (!_path.empty())
			{
				std::error_code ec;
				fs::remove_all(_path, ec);
			}
		}

		const fs::path& path() const { return _path; }

	private:
		fs::path _path;
	};

	std::string fake_exec_plugin_path(const char* name)
	{
#ifdef _WIN32
		const char* suffix = ".dll";
#elif defined(__APPLE__)
		const char* suffix = ".dylib";
#else
		const char* suffix = ".so";
#endif

		fs::path dir = r111::exe_dir();
		for (int depth = 0; depth != 9 && !dir.empty(); ++depth)
		{
			const fs::path direct = dir / "exec_v2_plugins" / (std::string(name) + suffix);
			if (fs::exists(direct))
				return direct.string();
			const fs::path coverage = dir / "TestUnits" / "coverage" /
				"exec_v2_plugins" / (std::string(name) + suffix);
			if (fs::exists(coverage))
				return coverage.string();
			dir = dir.parent_path();
		}
		return std::string();
	}

	class ScopedPluginCallbackLog
	{
	public:
		explicit ScopedPluginCallbackLog(const std::string& path)
		{
			const char* previous = std::getenv("WT_COV_EXEC_V2_LOG");
			if (previous)
				_previous = previous;
#ifdef _WIN32
			_putenv_s("WT_COV_EXEC_V2_LOG", path.c_str());
#else
			setenv("WT_COV_EXEC_V2_LOG", path.c_str(), 1);
#endif
		}

		~ScopedPluginCallbackLog()
		{
#ifdef _WIN32
			_putenv_s("WT_COV_EXEC_V2_LOG", _previous.c_str());
#else
			if (_previous.empty())
				unsetenv("WT_COV_EXEC_V2_LOG");
			else
				setenv("WT_COV_EXEC_V2_LOG", _previous.c_str(), 1);
#endif
		}

	private:
		std::string _previous;
	};

	class ScopedPluginTickGate
	{
	public:
		explicit ScopedPluginTickGate(const std::string& path)
		{
			const char* previous = std::getenv("WT_COV_EXEC_V2_TICK_GATE");
			if (previous)
				_previous = previous;
#ifdef _WIN32
			_putenv_s("WT_COV_EXEC_V2_TICK_GATE", path.c_str());
#else
			setenv("WT_COV_EXEC_V2_TICK_GATE", path.c_str(), 1);
#endif
		}

		~ScopedPluginTickGate()
		{
#ifdef _WIN32
			_putenv_s("WT_COV_EXEC_V2_TICK_GATE", _previous.c_str());
#else
			if (_previous.empty())
				unsetenv("WT_COV_EXEC_V2_TICK_GATE");
			else
				setenv("WT_COV_EXEC_V2_TICK_GATE", _previous.c_str(), 1);
#endif
		}

	private:
		std::string _previous;
	};
}

class R111ExecMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtExeFact");
	}
};

// Use the real plugin with a valid unit and destroy it without dispatching replay
// callbacks. This exercises unit deletion, factory deletion, and plugin unload.
TEST_F(R111ExecMocker, RealPluginLifecycleUsesExportedDeleterAbi)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_lifecycle", 0.0, 0);
		ASSERT_TRUE(m.init(cfg));
		cfg->release();
		EXPECT_TRUE(m.quiesce());
	}
}

// A second init must not replace the original unit/factory/module. Entrust is a
// no-op for this unknown successful order, but reaches the original real unit.
TEST_F(R111ExecMocker, SecondInitIsRejectedAndOriginalUnitRemainsUsable)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	ExecMocker m(&_rp);
	WTSVariant* firstCfg = make_exec_cfg(module(), "r111_exec_first_init", 0.0, 0);
	const bool firstInit = m.init(firstCfg);
	firstCfg->release();
	ASSERT_TRUE(firstInit);

	WTSVariant* secondCfg = make_exec_cfg(module(), "r111_exec_second_init", 0.0, 0, "NoSuchExecuter");
	const bool secondInit = m.init(secondCfg);
	secondCfg->release();
	EXPECT_FALSE(secondInit);

	// If the failed call replaced _exec_unit with NULL, this callback would
	// dereference it; the original unit stays reachable until scope destruction.
	m.handle_entrust(1u, kRb, true, "ok", 0u);
	EXPECT_TRUE(m.quiesce());
}

// A real factory rejects unknown names with NULL; ExecMocker must fail init
// without dereferencing the missing unit, and its factory must still be released.
TEST_F(R111ExecMocker, UnknownExecuterNameFailsAndReleasesFactory)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_unknown", 0.0, 0, "NoSuchExecuter");
	const bool firstInit = m.init(cfg);
	cfg->release();
	EXPECT_FALSE(firstInit);

	// Retrying after any failed attempt is intentionally unsupported: matcher
	// state may already have been initialized and no reset contract exists.
	WTSVariant* retryCfg = make_exec_cfg(module(), "r111_exec_retry", 0.0, 0);
	const bool retryInit = m.init(retryCfg);
	retryCfg->release();
	EXPECT_FALSE(retryInit);
}

// The real index plugin loads but has no createExecFact export.
TEST_F(R111ExecMocker, PluginWithoutExecuterFactoryExportFailsSafely)
{
	const std::string moduleWithoutExecFactory = r111::plugin_path("WtIndexFactory");
	ASSERT_TRUE(std::filesystem::exists(moduleWithoutExecFactory)) << moduleWithoutExecFactory;

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(moduleWithoutExecFactory, "r111_exec_missing_factory", 0.0, 0);
	EXPECT_FALSE(m.init(cfg));
	cfg->release();
}

TEST_F(R111ExecMocker, MissingExecuterConfigurationFailsWithDiagnostic)
{
	ExecMocker m(&_rp);
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", module().c_str());
	cfg->append("code", kRb);
	cfg->append("period", "m1");
	std::string error;
	EXPECT_FALSE(m.init(cfg, &error));
	EXPECT_NE(std::string::npos, error.find("execution unit configuration is missing"));
	cfg->release();
}

// init 成功（L71-85）→ handle_init（L140-161）→ 15:00 早退与正常调度（L163-192）
TEST_F(R111ExecMocker, InitWithRealExecuterAndInitSchedule)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_init", 0.0, 0);
	ASSERT_TRUE(m.init(cfg));
	cfg->release();

	// 真实插件装载后商品/时段查询仍透传回放器
	ASSERT_NE(nullptr, m.getCommodityInfo(kRb));
	ASSERT_NE(nullptr, m.getSessionInfo(kRb));
	OrderMap* orders = m.getOrders(kRb);
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(0u, orders->size());
	orders->release();

	// handle_init：K 线预取 + sub_tick + 表头 + on_channel_ready + set_position(0)
	m.handle_init();
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kRb));

	// 15:00 早退：没有 tick 也不能解引用 _last_tick
	m.handle_schedule(20240103u, 1500u);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));

	// 正常调度：volmode=0 且 position<=0 → target=volunit(0)，do_calc 在同值处返回
	WTSTickData* t1 = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
	m.handle_tick(kRb, t1, 0);
	t1->release();
	m.handle_schedule(20240103u, 931u);
	m.handle_schedule(20240103u, 932u);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	EXPECT_TRUE(m.quiesce());
}

// IMatchSink 三回调（L285-335）与落盘（L337-347）：撤销/成交各写一条 csv
TEST_F(R111ExecMocker, EntrustOrderTradeAndReplayDump)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_trade", 1.0, 0);
	ASSERT_TRUE(m.init(cfg));
	cfg->release();
	m.handle_init();

	WTSTickData* t1 = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0, 5, 7);
	m.handle_tick(kRb, t1, 0);
	t1->release();

	const uint64_t ordTime = (uint64_t)20240103 * 1000000000ull + 142500000ull;

	// entrust 成功/失败都只转发给执行单元，不影响账户
	m.handle_entrust(1u, kRb, true, "ok", ordTime);
	m.handle_entrust(2u, kRb, false, "rejected", ordTime);
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	// The real executor sent one buy order on the preceding tick; it is pending,
	// so position is unchanged while undone quantity starts at +1.
	EXPECT_DOUBLE_EQ(1.0, m.getUndoneQty(kRb));

	// 撤单剩余量 2 抵消该 +1 在途量后为 -1，并写 cancel=true 记录。
	m.handle_order(10u, kRb, true, 2.0, 3799.0, true, ordTime);
	EXPECT_DOUBLE_EQ(-1.0, m.getUndoneQty(kRb));

	// 手工成交买入 2 后，仓位为 2，未完成量为 -3。
	m.handle_trade(11u, kRb, true, 2.0, 3800.0, 3800.0, ordTime);
	EXPECT_DOUBLE_EQ(2.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(-3.0, m.getUndoneQty(kRb));
	m.handle_order(12u, kRb, false, 1.0, 3801.0, false, ordTime);
	EXPECT_DOUBLE_EQ(2.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(-3.0, m.getUndoneQty(kRb));
	m.handle_trade(13u, kRb, false, 1.0, 3801.0, 3801.0, ordTime);
	EXPECT_DOUBLE_EQ(1.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(-2.0, m.getUndoneQty(kRb));

	// 第二笔撤单回报
	m.handle_order(14u, kRb, true, 1.0, 3800.0, true, ordTime);
	EXPECT_DOUBLE_EQ(-3.0, m.getUndoneQty(kRb));

	m.handle_replay_done();

	const std::filesystem::path csv = _out / "exec" / "trades_r111_exec_trade.csv";
	ASSERT_TRUE(std::filesystem::exists(csv)) << csv;
	const std::string content = read_text(csv);
	// 表头 + 撤单(10) + 买(11) + 卖(13) + 撤单(14)
	EXPECT_EQ(5u, line_count(content));
	EXPECT_NE(std::string::npos, content.find(
		"localid,signaltime,ordertime,bs,sigprice,ordprice,lmtprice,tradetime,trdprice,qty,sigtimespan,exectime,cancel"));
	EXPECT_NE(std::string::npos, content.find(",true\n"));
	EXPECT_NE(std::string::npos, content.find(",false\n"));
	EXPECT_TRUE(m.quiesce());
}

// volmode=-1/+1 的 target 累计分支（L180-187）
TEST_F(R111ExecMocker, ScheduleNegativeAndPositiveVolumeModes)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_sell", 0.0, -1);
		ASSERT_TRUE(m.init(cfg));
		cfg->release();
		WTSTickData* t = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
		m.handle_tick(kRb, t, 0);
		t->release();
		m.handle_schedule(20240103u, 931u);
		m.handle_schedule(20240103u, 932u);
		EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
		EXPECT_TRUE(m.quiesce());
	}

	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(module(), "r111_exec_buy", 0.0, 1);
		ASSERT_TRUE(m.init(cfg));
		cfg->release();
		WTSTickData* t = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
		m.handle_tick(kRb, t, 0);
		t->release();
		m.handle_schedule(20240103u, 931u);
		m.handle_schedule(20240103u, 932u);
		EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
		EXPECT_TRUE(m.quiesce());
	}
}

// Short ABI tables and missing teardown capabilities must be rejected before
// the host calls any factory callback.
TEST_F(R111ExecMocker, RejectsShortAndUnverifiablePluginTables)
{
	ScopedTestTempDir testFiles("reject-plugin-tables");
	ASSERT_FALSE(testFiles.path().empty());
	const char* const fixtures[] = {
		"bad_size", "short_quiesce", "short_factory_preflight", "bad_callback",
		"no_quiesce_capability", "no_request_quiesce", "no_can_wait_quiesce",
		"no_wait_quiesce", "no_factory_preflight_capability", "no_factory_preflight",
		"abi20", "abi21"
	};
	for (const char* fixture : fixtures)
	{
		const std::string plugin = fake_exec_plugin_path(fixture);
		ASSERT_FALSE(plugin.empty()) << "missing fake plugin fixture: " << fixture;
		const std::filesystem::path logPath = testFiles.path() / (std::string(fixture) + ".log");
		ScopedPluginCallbackLog callbackLog(logPath.string());
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(plugin, fixture, 0.0, 0);
		std::string error;
		const bool initialized = m.init(cfg, &error);
		cfg->release();
		EXPECT_FALSE(initialized) << fixture;
		EXPECT_NE(std::string::npos, error.find("incompatible or incomplete"));
		EXPECT_EQ("get_api\n", read_text(logPath)) << fixture;
	}
}

// The fake dynamic module records the full shutdown order through the real
// ExecMocker ABI boundary. Real WtExeFact order/trade delivery is covered above.
TEST_F(R111ExecMocker, PluginQuiesceUsesFactoryPreflightRequestCanWaitWaitThenDestroy)
{
	ScopedTestTempDir testFiles("plugin-quiesce-order");
	ASSERT_FALSE(testFiles.path().empty());
	const std::string plugin = fake_exec_plugin_path("valid");
	ASSERT_FALSE(plugin.empty()) << "missing fake ABI 2.2 plugin fixture";
	const std::filesystem::path logPath = testFiles.path() / "quiesce-order.log";
	ScopedPluginCallbackLog callbackLog(logPath.string());
	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(plugin, "r111_fake_order", 0.0, 0, "FakeV2Factory.Unit");
		ASSERT_TRUE(m.init(cfg));
		cfg->release();

		// The runner can use this nonblocking preflight before it stops or joins
		// replay work. It must not itself request quiescence or alter the later
		// explicit lifecycle sequence.
		EXPECT_TRUE(m.canStopFromCurrentThread());
		EXPECT_TRUE(m.requestQuiesce());
		EXPECT_TRUE(m.canWaitQuiesce());
		EXPECT_TRUE(m.waitQuiesce());
	}
	EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
		"can_wait_factory_quiesce\ncan_wait_factory_quiesce\nrequest_quiesce\n"
		"can_wait_quiesce\nwait_quiesce\n"
		"destroy_unit\ndestroy_factory\n", read_text(logPath));
}

TEST_F(R111ExecMocker, OtherThreadPluginCallbackDoesNotBlockControlThreadPreflight)
{
	ScopedTestTempDir testFiles("active-plugin-callback");
	ASSERT_FALSE(testFiles.path().empty());
	const std::string plugin = fake_exec_plugin_path("valid");
	ASSERT_FALSE(plugin.empty()) << "missing fake ABI 2.2 plugin fixture";
	const std::filesystem::path logPath = testFiles.path() / "active-plugin-callback.log";
	const std::filesystem::path gatePath = testFiles.path() / "active-plugin-callback.gate";
	ScopedPluginCallbackLog callbackLog(logPath.string());
	ScopedPluginTickGate callbackGate(gatePath.string());

	ExecMocker m(&_rp);
	WTSVariant* cfg = make_exec_cfg(plugin, "r111_fake_blocking_tick", 0.0, 0, "BlockingUnit");
	const bool initialized = m.init(cfg);
	cfg->release();
	ASSERT_TRUE(initialized);

	WTSTickData* tick = r62::make_tick(kRb, 20240103u, 142500000u, 3800.0, 3799.0, 3801.0);
	std::thread callbackThread([&m, tick]() { m.handle_tick(kRb, tick, 0); });
	const bool callbackEntered = wait_for_text(logPath, "blocking_on_tick_enter",
		std::chrono::seconds(3));
	const bool preflightAllowed = callbackEntered && m.canStopFromCurrentThread();
	const bool requestWhileCallbackActive = preflightAllowed && m.requestQuiesce();
	const bool canWaitWhileCallbackActive = requestWhileCallbackActive && m.canWaitQuiesce();

	// Always release and join the controlled callback before assertions or local
	// owner destruction, including when a regression makes preflight fail.
	bool gateSignaled = false;
	{
		std::ofstream gate(gatePath, std::ios::binary);
		gate << "release";
		gate.close();
		gateSignaled = gate.good();
	}
	callbackThread.join();
	tick->release();

	const bool requestAfterCallback = requestWhileCallbackActive || m.requestQuiesce();
	const bool canWaitAfterCallback = requestAfterCallback && m.canWaitQuiesce();
	const bool waitedAfterCallback = canWaitAfterCallback && m.waitQuiesce();

	EXPECT_TRUE(callbackEntered);
	EXPECT_TRUE(preflightAllowed);
	EXPECT_TRUE(requestWhileCallbackActive);
	EXPECT_FALSE(canWaitWhileCallbackActive);
	EXPECT_TRUE(gateSignaled);
	EXPECT_TRUE(requestAfterCallback);
	EXPECT_TRUE(canWaitAfterCallback);
	EXPECT_TRUE(waitedAfterCallback);
	EXPECT_NE(std::string::npos, read_text(logPath).find("blocking_on_tick_released"));
}

// A failed request leaves the plugin unit/factory/library alive and retries on
// the same control thread before destruction is allowed.
TEST_F(R111ExecMocker, FailedQuiesceRequestRetainsAllPluginOwnersUntilRetrySucceeds)
{
	ScopedTestTempDir testFiles("quiesce-request-retry");
	ASSERT_FALSE(testFiles.path().empty());
	const std::string plugin = fake_exec_plugin_path("request_fail");
	ASSERT_FALSE(plugin.empty()) << "missing fake request-failure fixture";
	const std::filesystem::path logPath = testFiles.path() / "quiesce-retry.log";
	ScopedPluginCallbackLog callbackLog(logPath.string());
	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(plugin, "r111_fake_retry", 0.0, 0, "FakeV2Factory.Unit");
		ASSERT_TRUE(m.init(cfg));
		cfg->release();

		std::string error;
		EXPECT_FALSE(m.requestQuiesce(&error));
		EXPECT_FALSE(error.empty());
		EXPECT_FALSE(m.waitQuiesce());
		EXPECT_EQ(std::string::npos, read_text(logPath).find("destroy_unit"));
		EXPECT_EQ(std::string::npos, read_text(logPath).find("destroy_factory"));

		EXPECT_TRUE(m.requestQuiesce());
		EXPECT_TRUE(m.canWaitQuiesce());
		EXPECT_TRUE(m.waitQuiesce());
	}
	const std::string events = read_text(logPath);
	EXPECT_LT(events.find("request_quiesce"), events.find("destroy_unit"));
	EXPECT_LT(events.find("wait_quiesce"), events.find("destroy_unit"));
	EXPECT_LT(events.find("destroy_unit"), events.find("destroy_factory"));
}

TEST_F(R111ExecMocker, FailedWaitRetainsPluginOwnersAndRequiresFreshCanWaitBeforeRetry)
{
	ScopedTestTempDir testFiles("quiesce-wait-retry");
	ASSERT_FALSE(testFiles.path().empty());
	const std::string plugin = fake_exec_plugin_path("wait_fail");
	ASSERT_FALSE(plugin.empty()) << "missing fake wait-failure fixture";
	const std::filesystem::path logPath = testFiles.path() / "wait-retry.log";
	ScopedPluginCallbackLog callbackLog(logPath.string());
	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(plugin, "r111_fake_wait_retry", 0.0, 0, "FakeV2Factory.Unit");
		ASSERT_TRUE(m.init(cfg));
		cfg->release();

		EXPECT_TRUE(m.requestQuiesce());
		EXPECT_TRUE(m.canWaitQuiesce());
		EXPECT_FALSE(m.waitQuiesce());
		EXPECT_EQ(std::string::npos, read_text(logPath).find("destroy_unit"));
		EXPECT_EQ(std::string::npos, read_text(logPath).find("destroy_factory"));

		EXPECT_TRUE(m.canWaitQuiesce());
		EXPECT_TRUE(m.waitQuiesce());
	}
	const std::string events = read_text(logPath);
	EXPECT_LT(events.find("wait_quiesce"), events.find("destroy_unit"));
	EXPECT_LT(events.find("destroy_unit"), events.find("destroy_factory"));
}

TEST_F(R111ExecMocker, FailedFactoryPreflightSendsNoRequestAndCanBeRetried)
{
	ScopedTestTempDir testFiles("factory-preflight-retry");
	ASSERT_FALSE(testFiles.path().empty());
	const std::string plugin = fake_exec_plugin_path("factory_preflight_false");
	ASSERT_FALSE(plugin.empty()) << "missing fake factory-preflight fixture";
	const std::filesystem::path logPath = testFiles.path() / "factory-preflight-retry.log";
	ScopedPluginCallbackLog callbackLog(logPath.string());
	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(plugin, "r111_factory_preflight_retry", 0.0, 0,
			"FakeV2Factory.Unit");
		ASSERT_TRUE(m.init(cfg));
		cfg->release();

		EXPECT_FALSE(m.requestQuiesce());
		EXPECT_EQ(std::string::npos, read_text(logPath).find("request_quiesce"));
		EXPECT_EQ(std::string::npos, read_text(logPath).find("destroy_unit"));
		EXPECT_EQ(std::string::npos, read_text(logPath).find("destroy_factory"));

		EXPECT_TRUE(m.requestQuiesce());
		EXPECT_TRUE(m.canWaitQuiesce());
		EXPECT_TRUE(m.waitQuiesce());
	}
	const std::string events = read_text(logPath);
	const std::size_t firstPreflight = events.find("can_wait_factory_quiesce");
	const std::size_t secondPreflight = events.find("can_wait_factory_quiesce", firstPreflight + 1);
	EXPECT_NE(std::string::npos, firstPreflight);
	EXPECT_NE(std::string::npos, secondPreflight);
	EXPECT_EQ(std::string::npos,
		events.find("can_wait_factory_quiesce", secondPreflight + 1));
	EXPECT_LT(secondPreflight, events.find("request_quiesce"));
	EXPECT_LT(events.find("request_quiesce"), events.find("destroy_unit"));
}

TEST_F(R111ExecMocker, CanStopPreflightFailureDefersWithoutChangingRequestState)
{
	ScopedTestTempDir testFiles("can-stop-preflight-false");
	ASSERT_FALSE(testFiles.path().empty());
	const std::string plugin = fake_exec_plugin_path("factory_preflight_false");
	ASSERT_FALSE(plugin.empty()) << "missing fake factory-preflight fixture";
	const std::filesystem::path logPath = testFiles.path() / "can-stop-preflight-false.log";
	ScopedPluginCallbackLog callbackLog(logPath.string());
	{
		ExecMocker m(&_rp);
		WTSVariant* cfg = make_exec_cfg(plugin, "r111_can_stop_preflight_false", 0.0, 0,
			"FakeV2Factory.Unit");
		ASSERT_TRUE(m.init(cfg));
		cfg->release();

		// A false factory preflight means the runner must defer before stopping
		// or joining. It must not record a unit request; the normal lifecycle can
		// retry later from a safe control thread.
		EXPECT_FALSE(m.canStopFromCurrentThread());
		EXPECT_EQ("get_api\ncreate_factory\ncreate_unit\ninitialize_unit\n"
			"can_wait_factory_quiesce\n", read_text(logPath));
		EXPECT_TRUE(m.requestQuiesce());
		EXPECT_TRUE(m.canWaitQuiesce());
		EXPECT_TRUE(m.waitQuiesce());
	}
	const std::string events = read_text(logPath);
	const std::size_t firstPreflight = events.find("can_wait_factory_quiesce");
	const std::size_t secondPreflight = events.find("can_wait_factory_quiesce", firstPreflight + 1);
	EXPECT_NE(std::string::npos, secondPreflight);
	EXPECT_LT(secondPreflight, events.find("request_quiesce"));
	EXPECT_LT(events.find("wait_quiesce"), events.find("destroy_unit"));
}
