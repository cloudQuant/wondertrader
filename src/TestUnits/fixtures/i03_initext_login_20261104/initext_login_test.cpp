/**
 * Iteration 03, BUG-I02-NEW-016, decision card 10 = A (user decision 2026-11-03):
 * an ITraderApi passed to TraderAdapter::initExt() authenticates and logs in by
 * itself. The adapter holds no credentials for it; WTE_Connect only reports the
 * channel state and the login outcome flows from the external API's
 * onLoginResult() callback. The config init() path keeps logging in with the
 * configured user/pass.
 *
 * Built against two TraderAdapter variants selected by WT_I03_VARIANT:
 *   1 = working tree before this change (baseline/ snapshot)
 *   2 = current change
 *
 * The config path loads the repository's test-only R59CredentialsTrader plugin
 * (src/TestUnits/fixtures/r59_credentials_trader.cpp) from ./traders/ under the
 * scratch working directory. Every API here is a local fake: no vendor SDK, no
 * network, no real login or order.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

#include <Includes/IBaseDataMgr.h>
#include <Includes/ITraderApi.h>
#include <Includes/WTSVariant.hpp>

#include <WtCore/TraderAdapter.h>
#include <WtCore/WtHelper.h>

#ifndef WT_I03_VARIANT
#error "WT_I03_VARIANT must be 1 (pre-change tree) or 2 (fix)"
#endif

USING_NS_WTP;

namespace {

namespace fs = std::filesystem;

class EmptyBaseData : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return NULL; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
	WTSContractInfo* getContract(const char*, const char* = "", uint32_t = 0) override { return NULL; }
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
};

/**
 * Self-authenticating external API, modelled on HftLatencyTool's TestTrader:
 * init(NULL) succeeds, connect() reports WTE_Connect and the API then reports
 * its own login result. login() must never be reached from the adapter.
 */
class SelfAuthApi : public ITraderApi
{
public:
	bool init_ret = true;
	bool auto_login = false;		// connect() also reports the API's own login result
	bool login_ok = true;
	int connect_ec = 0;

	ITraderSpi* spi = NULL;
	std::atomic<uint32_t> n_init{ 0 }, n_release{ 0 }, n_connect{ 0 }, n_login{ 0 };
	std::atomic<uint32_t> n_qpos{ 0 };
	std::atomic<bool> init_got_null{ false };
	std::string last_user, last_pass;

	bool init(WTSVariant* params) override
	{
		++n_init;
		init_got_null = (params == NULL);
		return init_ret;
	}
	void release() override { ++n_release; }
	void registerSpi(ITraderSpi* listener) override { spi = listener; }
	void connect() override
	{
		++n_connect;
		if (spi == NULL)
			return;
		spi->handleEvent(WTE_Connect, connect_ec);
		if (auto_login && connect_ec == 0)
			spi->onLoginResult(login_ok, login_ok ? "" : "external auth rejected", 20261104);
	}
	void disconnect() override {}
	bool isConnected() override { return true; }
	int login(const char* user, const char* pass, const char*) override
	{
		++n_login;
		last_user = user ? user : "<null>";
		last_pass = pass ? pass : "<null>";
		return 0;
	}
	bool makeEntrustID(char* buffer, int length) override
	{
		std::snprintf(buffer, (size_t)length, "I03X");
		return true;
	}
	int queryAccount() override { return 0; }
	int queryPositions() override { ++n_qpos; return 0; }
	int queryOrders() override { return 0; }
	int queryTrades() override { return 0; }
};

struct VariantRelease
{
	void operator()(WTSVariant* v) const { if (v) v->release(); }
};

std::string readAll(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

class I03InitExtLogin : public ::testing::Test
{
protected:
	EmptyBaseData bd_;
	SelfAuthApi api_;
	std::string id_;
	fs::path tmp_;

	void SetUp() override
	{
		static std::atomic<uint32_t> seq{ 0 };
		id_ = "i03x" + std::to_string(++seq);
		tmp_ = fs::temp_directory_path() / ("i03_initext_" + std::to_string((long)::getpid()) + "_" + id_);
		fs::create_directories(tmp_);
		WtHelper::setTime(20261104, 931, 0);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(fs::path(WtHelper::getBaseDir()) / "traders" / id_, ec);
		fs::remove_all(tmp_, ec);
	}
};

// ---- config init(): unchanged contract, adapter logs in with cfg credentials ----

TEST_F(I03InitExtLogin, config_init_connect_logs_in_with_cfg_credentials)
{
	// run.sh installs the test-only plugin under ./traders/ of the scratch cwd.
	ASSERT_TRUE(fs::is_regular_file(fs::path(WtHelper::getCWD()) / "traders" / I03_CRED_PLUGIN_FILE))
		<< WtHelper::getCWD();
	const fs::path record = tmp_ / "login_record.txt";

	std::unique_ptr<WTSVariant, VariantRelease> cfg(WTSVariant::createObject());
	ASSERT_TRUE(cfg->append("module", "R59CredentialsTrader"));
	ASSERT_TRUE(cfg->append("savedata", false));
	ASSERT_TRUE(cfg->append("user", "cfg_user"));
	ASSERT_TRUE(cfg->append("pass", "cfg_pass"));
	ASSERT_TRUE(cfg->append("r59_login_record", record.string().c_str()));

	TraderAdapter trader;
	ASSERT_TRUE(trader.init(id_.c_str(), cfg.get(), &bd_, NULL));
	ASSERT_TRUE(trader.run());	// plugin connect() -> WTE_Connect(0) -> adapter login(cfg)
	EXPECT_EQ(TraderAdapter::AS_LOGINING, trader.state());
	EXPECT_EQ("cfg_user\ncfg_pass\n", readAll(record));

	// A failed connect event does not log in again.
	trader.handleEvent(WTE_Connect, -2);
	EXPECT_EQ("cfg_user\ncfg_pass\n", readAll(record));

	// A second successful connect (reconnect) logs in again with the same account.
	trader.handleEvent(WTE_Connect, 0);
	EXPECT_EQ("cfg_user\ncfg_pass\ncfg_user\ncfg_pass\n", readAll(record));

	trader.onLoginResult(true, "", 20261104);
	EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
	trader.release();
}

// ---- successful initExt(): external API owns authentication ----

TEST_F(I03InitExtLogin, init_ext_connect_reports_state_without_adapter_login)
{
	TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	EXPECT_TRUE(api_.init_got_null.load());	// adapter passes no configuration/credentials
	ASSERT_TRUE(trader.run());				// connect() synchronously reports WTE_Connect(0)
	EXPECT_EQ(1u, api_.n_connect.load());
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_LOGINING, trader.state());

	// Repeated connect notifications (e.g. reconnect) also never trigger login.
	trader.handleEvent(WTE_Connect, 0);
	trader.handleEvent(WTE_Connect, 0);
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_LOGINING, trader.state());
	EXPECT_EQ(0u, api_.n_qpos.load());

	// The login result comes from the external API itself.
	trader.onLoginResult(true, "", 20261104);
	EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
	EXPECT_EQ(1u, api_.n_qpos.load());
	EXPECT_EQ(0u, api_.n_login.load());
	trader.release();
}

TEST_F(I03InitExtLogin, init_ext_self_login_success_flows_from_api_callback)
{
	api_.auto_login = true;
	TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	ASSERT_TRUE(trader.run());
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_LOGINED, trader.state());
	EXPECT_EQ(1u, api_.n_qpos.load());
	trader.release();
}

TEST_F(I03InitExtLogin, init_ext_self_login_failure_flows_from_api_callback)
{
	api_.auto_login = true;
	api_.login_ok = false;
	TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	ASSERT_TRUE(trader.run());
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_LOGINFAILED, trader.state());
	EXPECT_EQ(0u, api_.n_qpos.load());
	trader.release();
}

TEST_F(I03InitExtLogin, init_ext_connect_failure_event_does_not_login)
{
	api_.connect_ec = -7;
	api_.auto_login = true;
	TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	ASSERT_TRUE(trader.run());
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_LOGINING, trader.state());
	trader.release();
}

// ---- initExt failure paths ----

TEST_F(I03InitExtLogin, init_ext_rejects_null_api_and_second_api)
{
	TraderAdapter none;
	EXPECT_FALSE(none.initExt(id_.c_str(), NULL, &bd_, NULL));
	EXPECT_FALSE(none.run());

	TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	SelfAuthApi second;
	EXPECT_FALSE(trader.initExt(id_.c_str(), &second, &bd_, NULL));
	EXPECT_EQ(0u, second.n_init.load());
	trader.release();
}

TEST_F(I03InitExtLogin, init_ext_api_init_failure_releases_and_never_logs_in)
{
	api_.init_ret = false;
	TraderAdapter trader;
	EXPECT_FALSE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	EXPECT_EQ(1u, api_.n_init.load());
	EXPECT_EQ(1u, api_.n_release.load());
	EXPECT_FALSE(trader.run());
	EXPECT_EQ(0u, api_.n_connect.load());
	// Defensive only: the SPI is never registered after a failed initExt, so a
	// provider cannot normally deliver this; the adapter still must not log in.
	trader.handleEvent(WTE_Connect, 0);
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, trader.state());
}

TEST_F(I03InitExtLogin, late_connect_after_init_ext_release_does_not_login)
{
	TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, NULL));
	ASSERT_TRUE(trader.run());
	trader.release();
	EXPECT_EQ(1u, api_.n_release.load());
	trader.handleEvent(WTE_Connect, 0);
	EXPECT_EQ(0u, api_.n_login.load());
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, trader.state());
}

} // namespace
