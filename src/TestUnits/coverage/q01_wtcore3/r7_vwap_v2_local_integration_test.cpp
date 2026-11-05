/**
 * Integration coverage for the built-in VWAP units through the V2 plugin ABI
 * and WtLocalExecuter. This deliberately uses the real libWtExeFact artifact,
 * actual WTSSessionInfo instances, and profile documents written for those
 * sessions; only the execution-engine stub and market interfaces are fakes.
 */
#include "../n01_wtcore_tradepaths/r71_wtcore_env.h"

#include <WtCore/WtExecMgr.h>
#include <WtCore/WtLocalExecuter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wtp {

/** Narrow read-only access to the private unit registry for this integration test. */
struct WtLocalExecuterTestAccess
{
	static size_t groupCount(const WtLocalExecuter& executer) { return executer._group_rules.size(); }
	static bool hasConfig(const WtLocalExecuter& executer) { return executer._config != nullptr; }
	static bool hasPool(const WtLocalExecuter& executer) { return executer._pool != nullptr; }
	static bool hasUnit(const WtLocalExecuter& executer, const char* code)
	{
		if (code == nullptr || code[0] == '\0')
			return false;
		SpinLock lock(executer._mtx_units);
		return executer._unit_map.find(std::string(code)) != executer._unit_map.end();
	}

	static void registerUnit(WtLocalExecuter& executer, const std::string& code,
		ExecuteUnit* rawUnit, IExecuterFact* fact)
	{
		ExecuteUnitPtr unit(new ExeUnitWrapper(rawUnit, fact));
		std::shared_ptr<WtLocalExecuter::UnitLifecycleState> state(
			new WtLocalExecuter::UnitLifecycleState());
		state->creation_complete = true;
		state->result = unit;
		state->init_status = ExecUnitInitState::READY;
		state->init_generation = 1;
		SpinLock lock(executer._mtx_units);
		executer._unit_map[code] = unit;
		executer._unit_states[code] = state;
	}

	static void makeInitializing(WtLocalExecuter& executer, const std::string& code)
	{
		SpinLock lock(executer._mtx_units);
		auto state = executer._unit_states.at(code);
		std::lock_guard<std::mutex> stateLock(state->mutex);
		state->creation_complete = false;
		state->creation_started = true;
		state->init_status = ExecUnitInitState::INITIALIZING;
	}

	static void finishInitializing(WtLocalExecuter& executer, const std::string& code)
	{
		std::shared_ptr<WtLocalExecuter::UnitLifecycleState> state;
		{
			SpinLock lock(executer._mtx_units);
			state = executer._unit_states.at(code);
			std::lock_guard<std::mutex> stateLock(state->mutex);
			state->creation_complete = true;
			state->init_status = ExecUnitInitState::READY;
		}
		executer.drainUnitCallbacks(code, state);
	}

	static double target(const WtLocalExecuter& executer, const std::string& code)
	{
		std::lock_guard<std::mutex> lock(executer._position_state_mutex);
		auto it = executer._target_pos.find(code);
		return it == executer._target_pos.end() ? 0.0 : it->second;
	}

	static void unregisterUnits(WtLocalExecuter& executer,
		const std::vector<std::string>& codes)
	{
		SpinLock lock(executer._mtx_units);
		for (const std::string& code : codes)
		{
			executer._unit_map.erase(code);
			executer._unit_states.erase(code);
		}
	}
};

} // namespace wtp

namespace {

namespace fs = std::filesystem;
using namespace wtp;

class ScopedWorkingDirectory
{
public:
	~ScopedWorkingDirectory()
	{
		if (_changed)
		{
			std::error_code restoreError;
			fs::current_path(_original, restoreError);
			if (!restoreError)
			{
				std::error_code cleanupError;
				fs::remove_all(_temporary, cleanupError);
			}
		}
	}

	bool enter(const std::string& label)
	{
		std::error_code ec;
		_original = fs::current_path(ec);
		if (ec)
			return false;
		_temporary = fs::temp_directory_path(ec) /
			("q01_vwap_v2_" + label + "_" + std::to_string(::getpid()));
		if (ec)
			return false;
		fs::remove_all(_temporary, ec);
		ec.clear();
		fs::create_directories(_temporary, ec);
		if (ec)
			return false;
		fs::current_path(_temporary, ec);
		_changed = !ec;
		return _changed;
	}

private:
	fs::path _original;
	fs::path _temporary;
	bool _changed = false;
};

class VwapIntegrationStub : public IExecuterStub
{
public:
	struct Instrument
	{
		WTSCommodityInfo* commodity = nullptr;
		WTSSessionInfo* session = nullptr;
	};

	void bind(const char* code, WTSCommodityInfo* commodity, WTSSessionInfo* session)
	{
		_instruments[code] = Instrument{ commodity, session };
	}

	uint64_t get_real_time() override { return 20250110093100000ull; }
	WTSCommodityInfo* get_comm_info(const char* code) override
	{
		auto it = _instruments.find(code ? code : "");
		return it == _instruments.end() ? nullptr : it->second.commodity;
	}
	WTSSessionInfo* get_sess_info(const char* code) override
	{
		auto it = _instruments.find(code ? code : "");
		return it == _instruments.end() ? nullptr : it->second.session;
	}
	IHotMgr* get_hot_mon() override { return _hot_mgr; }
	uint32_t get_trading_day() override { return 20250110; }
	void setHotMgr(IHotMgr* hotMgr) { _hot_mgr = hotMgr; }

private:
	std::map<std::string, Instrument> _instruments;
	IHotMgr* _hot_mgr = nullptr;
};

class CapturingLocalExecuter : public WtLocalExecuter
{
public:
	using WtLocalExecuter::WtLocalExecuter;

	void writeLog(const char* message) override
	{
		if (message != nullptr)
			logs.emplace_back(message);
	}

	std::vector<std::string> logs;
};

class LocalGroupPositionProbe
{
public:
	void record(const char* code, double target)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_positions.emplace_back(code == nullptr ? "" : code, target);
	}

	std::vector<std::pair<std::string, double>> snapshot() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _positions;
	}

private:
	mutable std::mutex _mutex;
	std::vector<std::pair<std::string, double>> _positions;
};

class LocalGroupPositionUnit : public ExecuteUnit
{
public:
	explicit LocalGroupPositionUnit(std::shared_ptr<LocalGroupPositionProbe> probe)
		: _probe(std::move(probe)) {}
	const char* getName() override { return "LocalGroupPositionUnit"; }
	const char* getFactName() override { return "LocalGroupPositionFact"; }
	void set_position(const char* code, double target) override { _probe->record(code, target); }
	void on_tick(WTSTickData*) override {}
	void on_trade(uint32_t, const char*, bool, double, double) override {}
	void on_order(uint32_t, const char*, bool, double, double, bool) override {}
	void on_entrust(uint32_t, const char*, bool, const char*) override {}
	void on_channel_ready() override {}
	void on_channel_lost() override {}

private:
	std::shared_ptr<LocalGroupPositionProbe> _probe;
};

class LocalGroupPositionFact : public IExecuterFact
{
public:
	const char* getName() override { return "LocalGroupPositionFact"; }
	void enumExeUnit(FuncEnumUnitCallback callback) override
	{
		if (callback != nullptr)
			callback(getName(), "LocalGroupPositionUnit", true);
	}
	ExecuteUnit* createExeUnit(const char*) override { return nullptr; }
	ExecuteUnit* createDiffExeUnit(const char*) override { return nullptr; }
	ExecuteUnit* createArbiExeUnit(const char*) override { return nullptr; }
	bool deleteExeUnit(ExecuteUnit* unit) override
	{
		delete unit;
		return true;
	}
};

class R7VwapV2LocalIntegration : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
		const std::string label = std::string(test->test_case_name()) + "_" + test->name();
		ASSERT_TRUE(_working_directory.enter(label));

		// This coverage target requires its real plugin artifact. A missing binary
		// is a failed prerequisite, not a skipped integration result.
		ASSERT_TRUE(_plugin_dir.ok()) << "real libWtExeFact artifact is required";
		ASSERT_TRUE(_factory.loadFactories(_plugin_dir.dir().c_str()))
			<< "real libWtExeFact could not be loaded through WtExecuterFactory V2";

		_stub.setHotMgr(&_hot_mgr);
	}

	void TearDown() override
	{
		WtExecQuiesceCoordinator coordinator({ &_manager });
		const bool safe = coordinator.quiesce_all() && coordinator.is_complete()
			&& _manager.wait_quiesced();
		EXPECT_TRUE(safe) << "VWAP integration executors did not complete staged quiescence";
		if (!safe)
			std::abort();
	}

	WTSSessionInfo* makeSession(const char* id,
		const std::vector<std::pair<uint32_t, uint32_t>>& sections)
	{
		_bd.regisSession(id);
		WTSSessionInfo* session = _bd.session(id);
		if (session == nullptr)
			return nullptr;
		for (const auto& section : sections)
			session->addTradingSection(section.first, section.second);
		return session;
	}

	void addInstrument(const char* exchange, const char* product, const char* rawCode,
		const char* standardCode, const char* sessionId, TradingMode tradingMode,
		ContractCategory category, WTSSessionInfo* session,
		bool exposeCommodity = true, bool exposeSession = true)
	{
		WTSCommodityInfo* commodity = r71::makeComm(exchange, product, sessionId, tradingMode, category);
		commodity->setSessionInfo(session);
		WTSContractInfo* contract = r71::makeCt(exchange, product, rawCode);
		_bd.regis(commodity, contract);
		_stub.bind(standardCode, exposeCommodity ? commodity : nullptr,
			exposeSession ? session : nullptr);
	}

	static std::string profileForSession(WTSSessionInfo& session)
	{
		std::ostringstream out;
		out << "{\"version\":2,\"profiles\":[{\"session_id\":\"" << session.id()
			<< "\",\"offset_minutes\":" << session.getOffsetMins() << ",\"sections\":[";
		const WTSSessionInfo::TradingTimes& sections = session.getTradingSections();
		for (size_t i = 0; i < sections.size(); ++i)
		{
			if (i != 0)
				out << ',';
			out << "{\"from\":" << sections[i].first_raw
				<< ",\"to\":" << sections[i].second_raw << '}';
		}
		out << "],\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
			<< "\"close_inclusive\":true,\"values\":[";
		for (uint32_t i = 0; i <= session.getTradingMins(); ++i)
		{
			if (i != 0)
				out << ',';
			out << "1.0";
		}
		out << "]}]}";
		return out.str();
	}

	static bool writeProfile(const char* filename, const std::string& contents)
	{
		std::ofstream output(filename, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
			return false;
		output << contents;
		output.close();
		return !output.fail();
	}

	std::shared_ptr<CapturingLocalExecuter> makeLocal(const char* executorId,
		const char* unitName, uint32_t beginTime, uint32_t endTime, uint32_t totalTimes = 1)
	{
		std::shared_ptr<CapturingLocalExecuter> local(new CapturingLocalExecuter(
			&_factory, executorId, &_data_manager));
		_manager.add_executer(std::static_pointer_cast<IExecCommand>(local));
		_executors.push_back(local);
		local->setStub(&_stub);

		std::ostringstream json;
		json << "{\"scale\":1.0,\"poolsize\":0,\"policy\":{\"default\":{";
		json << "\"name\":\"" << unitName << "\",\"begin_time\":" << beginTime
			<< ",\"end_time\":" << endTime << ",\"ord_sticky\":0,\"tail_secs\":0"
			<< ",\"total_times\":" << totalTimes << ",\"price_mode\":0,\"price_offset\":0"
			<< ",\"lots\":1.0,\"minopenlots\":1.0,\"offset\":0}}}";
		WTSVariant* params = WTSCfgLoader::load_from_content(json.str());
		if (params == nullptr)
			return nullptr;
		const bool initialized = local->init(params);
		params->release();
		return initialized ? local : nullptr;
	}

	ExecUnitInitStatus query(const char* executorId, const char* code) const
	{
		return _manager.query_exec_unit_init_status(executorId, code);
	}

	ScopedWorkingDirectory _working_directory;
	r71::R71GenEnv _generate_env;
	r71::R71PluginDir _plugin_dir;
	r71::R71BaseDataMgr _bd;
	r71::R71HotMgr _hot_mgr;
	VwapIntegrationStub _stub;
	r71::R71DataMgr _data_manager;
	WtExecuterFactory _factory;
	WtExecuterMgr _manager;
	std::vector<std::shared_ptr<CapturingLocalExecuter>> _executors;
};

TEST_F(R7VwapV2LocalIntegration, MatchingDynamicProfilesRegisterFutureAndStockUnits)
{
	WTSSessionInfo* futureSession = makeSession("FUTURE_TEST",
		{ { 900, 1100 }, { 1300, 1500 } });
	WTSSessionInfo* stockSession = makeSession("STOCK_TEST",
		{ { 930, 1030 }, { 1300, 1500 } });
	ASSERT_NE(futureSession, nullptr);
	ASSERT_NE(stockSession, nullptr);
	addInstrument("SHFE", "ag", "ag2512", "SHFE.ag.2512", "FUTURE_TEST",
		TM_Both, CC_Future, futureSession);
	addInstrument("SSE", "STK", "600000", "SSE.STK.600000", "STOCK_TEST",
		TM_LongT1, CC_Stock, stockSession);
	ASSERT_TRUE(writeProfile("Vwap_ag.txt", profileForSession(*futureSession)));
	ASSERT_TRUE(writeProfile("Vwap_STK.txt", profileForSession(*stockSession)));

	std::shared_ptr<CapturingLocalExecuter> future = makeLocal("r7_v2_future_match",
		"WtExeFact.WtVWapExeUnit", 900, 1500);
	std::shared_ptr<CapturingLocalExecuter> stock = makeLocal("r7_v2_stock_match",
		"WtExeFact.WtStockVWapExeUnit", 930, 1500);
	ASSERT_NE(future, nullptr);
	ASSERT_NE(stock, nullptr);
	future->on_position_changed("SHFE.ag.2512", 1.0);
	stock->on_position_changed("SSE.STK.600000", 100.0);

	const ExecUnitInitStatus futureStatus = query("r7_v2_future_match", "SHFE.ag.2512");
	const ExecUnitInitStatus stockStatus = query("r7_v2_stock_match", "SSE.STK.600000");
	EXPECT_EQ(futureStatus.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(futureStatus.state, ExecUnitInitState::READY);
	EXPECT_TRUE(futureStatus.error.empty());
	EXPECT_TRUE(WtLocalExecuterTestAccess::hasUnit(*future, "SHFE.ag.2512"));
	EXPECT_EQ(stockStatus.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(stockStatus.state, ExecUnitInitState::READY);
	EXPECT_TRUE(stockStatus.error.empty());
	EXPECT_TRUE(WtLocalExecuterTestAccess::hasUnit(*stock, "SSE.STK.600000"));
}

TEST_F(R7VwapV2LocalIntegration, SessionMismatchedProfileFailsAndDoesNotRegisterUnit)
{
	WTSSessionInfo* actualSession = makeSession("FUTURE_TEST",
		{ { 900, 1100 }, { 1300, 1500 } });
	ASSERT_NE(actualSession, nullptr);
	WTSSessionInfo* profileSession = WTSSessionInfo::create("FUTURE_TEST", "FUTURE_TEST", 0);
	ASSERT_NE(profileSession, nullptr);
	profileSession->addTradingSection(900, 1030);
	profileSession->addTradingSection(1330, 1500);
	addInstrument("SHFE", "ag", "ag2512", "SHFE.ag.2512", "FUTURE_TEST",
		TM_Both, CC_Future, actualSession);
	const std::string mismatchedProfile = profileForSession(*profileSession);
	profileSession->release();
	ASSERT_TRUE(writeProfile("Vwap_ag.txt", mismatchedProfile));

	std::shared_ptr<CapturingLocalExecuter> local = makeLocal("r7_v2_future_mismatch",
		"WtExeFact.WtVWapExeUnit", 900, 1500);
	ASSERT_NE(local, nullptr);
	local->on_position_changed("SHFE.ag.2512", 1.0);

	const ExecUnitInitStatus status = query("r7_v2_future_mismatch", "SHFE.ag.2512");
	EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(status.error, "execution-unit configuration was rejected");
	EXPECT_FALSE(WtLocalExecuterTestAccess::hasUnit(*local, "SHFE.ag.2512"));
	bool sawSessionMismatch = false;
	for (const std::string& log : local->logs)
		sawSessionMismatch = sawSessionMismatch || log.find("no profile matching this session") != std::string::npos;
	EXPECT_TRUE(sawSessionMismatch) << "the built-in unit should report the session/profile mismatch";
}

TEST_F(R7VwapV2LocalIntegration, StockSessionMismatchedProfileFailsAndDoesNotRegisterUnit)
{
	WTSSessionInfo* actualSession = makeSession("STOCK_TEST",
		{ { 930, 1030 }, { 1300, 1500 } });
	ASSERT_NE(actualSession, nullptr);
	WTSSessionInfo* profileSession = WTSSessionInfo::create("STOCK_TEST", "STOCK_TEST", 0);
	ASSERT_NE(profileSession, nullptr);
	profileSession->addTradingSection(930, 1100);
	profileSession->addTradingSection(1300, 1500);
	addInstrument("SSE", "STK", "600000", "SSE.STK.600000", "STOCK_TEST",
		TM_LongT1, CC_Stock, actualSession);
	const std::string mismatchedProfile = profileForSession(*profileSession);
	profileSession->release();
	ASSERT_TRUE(writeProfile("Vwap_STK.txt", mismatchedProfile));

	std::shared_ptr<CapturingLocalExecuter> local = makeLocal("r7_v2_stock_mismatch",
		"WtExeFact.WtStockVWapExeUnit", 930, 1500);
	ASSERT_NE(local, nullptr);
	local->on_position_changed("SSE.STK.600000", 100.0);

	const ExecUnitInitStatus status = query("r7_v2_stock_mismatch", "SSE.STK.600000");
	EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(status.error, "execution-unit configuration was rejected");
	EXPECT_FALSE(WtLocalExecuterTestAccess::hasUnit(*local, "SSE.STK.600000"));
	bool sawSessionMismatch = false;
	for (const std::string& log : local->logs)
		sawSessionMismatch = sawSessionMismatch || log.find("no profile matching this session") != std::string::npos;
	EXPECT_TRUE(sawSessionMismatch) << "the built-in stock unit should report the session/profile mismatch";
}

TEST_F(R7VwapV2LocalIntegration, InvalidStockConfigurationFailsAndDoesNotRegisterUnit)
{
	WTSSessionInfo* session = makeSession("STOCK_TEST",
		{ { 930, 1030 }, { 1300, 1500 } });
	ASSERT_NE(session, nullptr);
	addInstrument("SSE", "STK", "600000", "SSE.STK.600000", "STOCK_TEST",
		TM_LongT1, CC_Stock, session);
	ASSERT_TRUE(writeProfile("Vwap_STK.txt", profileForSession(*session)));

	std::shared_ptr<CapturingLocalExecuter> local = makeLocal("r7_v2_stock_bad_config",
		"WtExeFact.WtStockVWapExeUnit", 930, 1500, 0);
	ASSERT_NE(local, nullptr);
	local->on_position_changed("SSE.STK.600000", 100.0);

	const ExecUnitInitStatus status = query("r7_v2_stock_bad_config", "SSE.STK.600000");
	EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(status.state, ExecUnitInitState::FAILED);
	EXPECT_EQ(status.error, "execution-unit configuration was rejected");
	EXPECT_FALSE(WtLocalExecuterTestAccess::hasUnit(*local, "SSE.STK.600000"));
	bool sawInvalidConfiguration = false;
	for (const std::string& log : local->logs)
		sawInvalidConfiguration = sawInvalidConfiguration ||
			log.find("total_times must be greater than zero") != std::string::npos;
	EXPECT_TRUE(sawInvalidConfiguration) << "the built-in stock unit should report total_times validation";
}

TEST_F(R7VwapV2LocalIntegration, MissingCommodityOrSessionFailsClosedForAllV2Units)
{
	WTSSessionInfo* futureSession = makeSession("FUTURE_TEST",
		{ { 900, 1100 }, { 1300, 1500 } });
	WTSSessionInfo* stockSession = makeSession("STOCK_TEST",
		{ { 930, 1030 }, { 1300, 1500 } });
	ASSERT_NE(futureSession, nullptr);
	ASSERT_NE(stockSession, nullptr);

	// Keep the registered contract/commodity realistic, but make each missing
	// metadata value observable through the same ExecuteContext getters used by
	// the built-in V2 units.
	addInstrument("SHFE", "ag", "ag2513", "SHFE.ag.2513", "FUTURE_TEST",
		TM_Both, CC_Future, futureSession, false, true);
	addInstrument("SHFE", "ag", "ag2514", "SHFE.ag.2514", "FUTURE_TEST",
		TM_Both, CC_Future, futureSession, true, false);
	addInstrument("SSE", "STK", "600001", "SSE.STK.600001", "STOCK_TEST",
		TM_LongT1, CC_Stock, stockSession, false, true);
	addInstrument("SSE", "STK", "600002", "SSE.STK.600002", "STOCK_TEST",
		TM_LongT1, CC_Stock, stockSession, true, false);

	auto expectFailedAndUnregistered = [this](const char* executorId, const char* unitName,
		uint32_t beginTime, const char* code, double target, const char* diagnostic) {
		std::shared_ptr<CapturingLocalExecuter> local = makeLocal(
			executorId, unitName, beginTime, 1500);
		if (local == nullptr)
		{
			ADD_FAILURE() << executorId << " local executor failed to initialize";
			return;
		}
		local->on_position_changed(code, target);
		const ExecUnitInitStatus status = query(executorId, code);
		EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK) << executorId;
		EXPECT_EQ(status.state, ExecUnitInitState::FAILED) << executorId;
		EXPECT_FALSE(WtLocalExecuterTestAccess::hasUnit(*local, code)) << executorId;
		bool sawDiagnostic = false;
		for (const std::string& log : local->logs)
			sawDiagnostic = sawDiagnostic || log.find(diagnostic) != std::string::npos;
		EXPECT_TRUE(sawDiagnostic) << executorId << " did not report: " << diagnostic;
	};

	const char* const twap = "WtExeFact.WtTWapExeUnit";
	const char* const vwap = "WtExeFact.WtVWapExeUnit";
	const char* const stockVwap = "WtExeFact.WtStockVWapExeUnit";
	const char* const futureMissingCommodity = "commodity or trading session is unavailable";
	const char* const stockMissingCommodity = "commodity information is unavailable";
	const char* const stockMissingSession = "trading session is unavailable";

	expectFailedAndUnregistered("r7_v2_twap_missing_commodity", twap, 900,
		"SHFE.ag.2513", 1.0, futureMissingCommodity);
	expectFailedAndUnregistered("r7_v2_twap_missing_session", twap, 900,
		"SHFE.ag.2514", 1.0, futureMissingCommodity);
	expectFailedAndUnregistered("r7_v2_vwap_missing_commodity", vwap, 900,
		"SHFE.ag.2513", 1.0, futureMissingCommodity);
	expectFailedAndUnregistered("r7_v2_vwap_missing_session", vwap, 900,
		"SHFE.ag.2514", 1.0, futureMissingCommodity);
	expectFailedAndUnregistered("r7_v2_stock_missing_commodity", stockVwap, 930,
		"SSE.STK.600001", 100.0, stockMissingCommodity);
	expectFailedAndUnregistered("r7_v2_stock_missing_session", stockVwap, 930,
		"SSE.STK.600002", 100.0, stockMissingSession);
}

TEST_F(R7VwapV2LocalIntegration, MatchingTwapCommodityAndSessionRegisterUnit)
{
	WTSSessionInfo* session = makeSession("FUTURE_TEST",
		{ { 900, 1100 }, { 1300, 1500 } });
	ASSERT_NE(session, nullptr);
	addInstrument("SHFE", "ag", "ag2512", "SHFE.ag.2512", "FUTURE_TEST",
		TM_Both, CC_Future, session);

	std::shared_ptr<CapturingLocalExecuter> local = makeLocal("r7_v2_twap_match",
		"WtExeFact.WtTWapExeUnit", 900, 1500);
	ASSERT_NE(local, nullptr);
	local->on_position_changed("SHFE.ag.2512", 1.0);

	const ExecUnitInitStatus status = query("r7_v2_twap_match", "SHFE.ag.2512");
	EXPECT_EQ(status.query_result, ExecUnitInitQueryResult::OK);
	EXPECT_EQ(status.state, ExecUnitInitState::READY);
	EXPECT_TRUE(status.error.empty());
	EXPECT_TRUE(WtLocalExecuterTestAccess::hasUnit(*local, "SHFE.ag.2512"));
}

TEST_F(R7VwapV2LocalIntegration, LocalGroupTargetsDispatchStablyAndClearOnEmptyInput)
{
	WTSSessionInfo* groupSession = makeSession("I03_LEGACY", {{900, 1500}});
	ASSERT_NE(groupSession, nullptr);
	addInstrument("SHFE", "rb", "rb2510", "SHFE.rb.2510", "I03_LEGACY", TM_Both, CC_Future, groupSession);
	addInstrument("SHFE", "au", "au2212", "SHFE.au.2212", "I03_LEGACY", TM_Both, CC_Future, groupSession);
	LocalGroupPositionFact fact;
	std::shared_ptr<CapturingLocalExecuter> local(new CapturingLocalExecuter(
		&_factory, "r7_local_group_targets", &_data_manager));
	_manager.add_executer(std::static_pointer_cast<IExecCommand>(local));
	_executors.push_back(local);
	local->setStub(&_stub);

	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_content(
		R"json({"scale":1.0,"poolsize":0,"strict_sync":false,
		"groups":{"SHFE.ags":{"SHFE.rb.2510":2.0,"SHFE.au.2212":2.0}},
		"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})json"),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);
	ASSERT_TRUE(local->init(config.get()));

	const std::string groupCode = "SHFE.ags";
	const std::string rbCode = "SHFE.rb.2510";
	const std::string auCode = "SHFE.au.2212";
	auto groupProbe = std::make_shared<LocalGroupPositionProbe>();
	auto rbProbe = std::make_shared<LocalGroupPositionProbe>();
	auto auProbe = std::make_shared<LocalGroupPositionProbe>();
	WtLocalExecuterTestAccess::registerUnit(*local, groupCode,
		new LocalGroupPositionUnit(groupProbe), &fact);
	WtLocalExecuterTestAccess::registerUnit(*local, rbCode,
		new LocalGroupPositionUnit(rbProbe), &fact);
	WtLocalExecuterTestAccess::registerUnit(*local, auCode,
		new LocalGroupPositionUnit(auProbe), &fact);

	auto expectLastTarget = [](const std::shared_ptr<LocalGroupPositionProbe>& probe,
		const std::string& code, double expected, size_t expectedCount) {
		const auto positions = probe->snapshot();
		ASSERT_EQ(positions.size(), expectedCount);
		EXPECT_EQ(positions.back().first, code);
		EXPECT_DOUBLE_EQ(positions.back().second, expected);
	};

	wt_hashmap<std::string, double> negativeTargets;
	negativeTargets[rbCode] = -6.0;
	negativeTargets[auCode] = -8.0;
	local->set_position(negativeTargets);
	expectLastTarget(groupProbe, groupCode, -3.0, 1);
	expectLastTarget(rbProbe, rbCode, 0.0, 1);
	expectLastTarget(auProbe, auCode, -2.0, 1);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), -2.0);

	// Fractional member targets produce an exact fractional group and residual.
	wt_hashmap<std::string, double> fractionalGroupTargets;
	fractionalGroupTargets[rbCode] = 7.5;
	fractionalGroupTargets[auCode] = 8.0;
	local->set_position(fractionalGroupTargets);
	expectLastTarget(groupProbe, groupCode, 3.75, 2);
	expectLastTarget(rbProbe, rbCode, 0.0, 2);
	expectLastTarget(auProbe, auCode, 0.5, 2);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), 0.5);

	// Integer member targets may also imply a fractional group count.
	wt_hashmap<std::string, double> fractionalCountTargets;
	fractionalCountTargets[rbCode] = 7.0;
	fractionalCountTargets[auCode] = 8.0;
	local->set_position(fractionalCountTargets);
	expectLastTarget(groupProbe, groupCode, 3.5, 3);
	expectLastTarget(rbProbe, rbCode, 0.0, 3);
	expectLastTarget(auProbe, auCode, 1.0, 3);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), 1.0);

	wt_hashmap<std::string, double> targets;
	targets[rbCode] = 6.0;
	targets[auCode] = 8.0;
	local->set_position(targets);

	expectLastTarget(groupProbe, groupCode, 3.0, 4);
	expectLastTarget(rbProbe, rbCode, 0.0, 4);
	expectLastTarget(auProbe, auCode, 2.0, 4);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, groupCode), 3.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), 2.0);

	local->set_position(targets);
	expectLastTarget(groupProbe, groupCode, 3.0, 5);
	expectLastTarget(rbProbe, rbCode, 0.0, 5);
	expectLastTarget(auProbe, auCode, 2.0, 5);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, groupCode), 3.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), 2.0);

	wt_hashmap<std::string, double> empty;
	local->set_position(empty);
	expectLastTarget(groupProbe, groupCode, 0.0, 6);
	expectLastTarget(rbProbe, rbCode, 0.0, 6);
	expectLastTarget(auProbe, auCode, 0.0, 6);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, groupCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), 0.0);
	WtLocalExecuterTestAccess::unregisterUnits(*local, { groupCode, rbCode, auCode });
}

TEST_F(R7VwapV2LocalIntegration, OverlappingGroupsUseConfiguredOrder)
{
	LocalGroupPositionFact fact;
	std::shared_ptr<CapturingLocalExecuter> local(new CapturingLocalExecuter(
		&_factory, "r7_local_overlapping_group_targets", &_data_manager));
	_manager.add_executer(std::static_pointer_cast<IExecCommand>(local));
	_executors.push_back(local);
	local->setStub(&_stub);

	std::shared_ptr<WTSVariant> config(WTSCfgLoader::load_from_content(
		R"json({"scale":1.0,"poolsize":0,"strict_sync":false,
		"groups":{"SHFE.ags":{"SHFE.rb.2510":2.0,"SHFE.au.2212":2.0},
		"SHFE.agc":{"SHFE.rb.2510":1.0,"SHFE.cu.2212":1.0}},
		"group_order":["SHFE.ags","SHFE.agc"],
		"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})json"),
		[](WTSVariant* value) { if (value) value->release(); });
	ASSERT_NE(config.get(), nullptr);
	ASSERT_TRUE(local->init(config.get()));

	const std::string firstGroupCode = "SHFE.ags";
	const std::string secondGroupCode = "SHFE.agc";
	const std::string rbCode = "SHFE.rb.2510";
	const std::string auCode = "SHFE.au.2212";
	const std::string cuCode = "SHFE.cu.2212";
	auto firstGroupProbe = std::make_shared<LocalGroupPositionProbe>();
	auto secondGroupProbe = std::make_shared<LocalGroupPositionProbe>();
	auto rbProbe = std::make_shared<LocalGroupPositionProbe>();
	auto auProbe = std::make_shared<LocalGroupPositionProbe>();
	auto cuProbe = std::make_shared<LocalGroupPositionProbe>();
	WtLocalExecuterTestAccess::registerUnit(*local, firstGroupCode,
		new LocalGroupPositionUnit(firstGroupProbe), &fact);
	WtLocalExecuterTestAccess::registerUnit(*local, secondGroupCode,
		new LocalGroupPositionUnit(secondGroupProbe), &fact);
	WtLocalExecuterTestAccess::registerUnit(*local, rbCode,
		new LocalGroupPositionUnit(rbProbe), &fact);
	WtLocalExecuterTestAccess::registerUnit(*local, auCode,
		new LocalGroupPositionUnit(auProbe), &fact);
	WtLocalExecuterTestAccess::registerUnit(*local, cuCode,
		new LocalGroupPositionUnit(cuProbe), &fact);

	wt_hashmap<std::string, double> targets;
	targets[rbCode] = 6.0;
	targets[auCode] = 8.0;
	targets[cuCode] = 5.0;
	local->set_position(targets);

	ASSERT_EQ(firstGroupProbe->snapshot().size(), 1u);
	EXPECT_DOUBLE_EQ(firstGroupProbe->snapshot().back().second, 3.0);
	EXPECT_TRUE(secondGroupProbe->snapshot().empty());
	const auto rbPositions = rbProbe->snapshot();
	const auto auPositions = auProbe->snapshot();
	const auto cuPositions = cuProbe->snapshot();
	ASSERT_EQ(rbPositions.size(), 1u);
	ASSERT_EQ(auPositions.size(), 1u);
	ASSERT_EQ(cuPositions.size(), 1u);
	EXPECT_DOUBLE_EQ(rbPositions.back().second, 0.0);
	EXPECT_DOUBLE_EQ(auPositions.back().second, 2.0);
	EXPECT_DOUBLE_EQ(cuPositions.back().second, 5.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, rbCode), 0.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, auCode), 2.0);
	EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local, cuCode), 5.0);
	WtLocalExecuterTestAccess::unregisterUnits(*local,
		{ firstGroupCode, secondGroupCode, rbCode, auCode, cuCode });
}

} // namespace
