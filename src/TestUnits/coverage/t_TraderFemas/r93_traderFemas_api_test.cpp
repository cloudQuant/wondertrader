/*
 * lane R93（第十七波）— TraderFemas 有 API 指针半边
 *
 * 目标：src/TraderFemas/TraderFemas.cpp。用 r93_femas_api_stub.h 的机械空实现
 * 替代真实 CUstpFtdcTraderApi，覆盖 connect/release、认证→登录→查询前置→就绪、
 * orderInsert/orderAction/logout、查询请求组装和独立调度回归。不连真实柜台。
 *
 * 既有 T4 已覆盖：init/dll 失败留白、回调解析、枚举换算、无 API 拒绝面。
 * 查询请求组装用例中，queryPositions/Orders/Trades 的字段断言会由测试侧手工
 * 出队执行；这不验证自动调度。独立调度用例通过可控 strand/io_context 观察
 * 三种查询是否真正发送，不手工执行队列；现有缺陷登记在 bugs/R93.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/io_context_strand.hpp>

#include "TraderFemas/TraderFemas.h"
#include "r93_femas_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Share/TimeUtils.hpp"

USING_NS_WTP;

namespace
{
	namespace fs = std::filesystem;

	int g_r93_femas_tmp_seq = 0;

	class R93FemasTempDir
	{
	public:
		R93FemasTempDir()
		{
			_dir = fs::temp_directory_path() /
				("wt_r93_femas_" + std::to_string((long)::getpid()) + "_" + std::to_string(++g_r93_femas_tmp_seq));
			std::error_code ec;
			fs::create_directories(_dir, ec);
		}
		~R93FemasTempDir()
		{
			std::error_code ec;
			fs::remove_all(_dir, ec);
		}
		std::string path(const char* leaf) const { return (_dir / leaf).string(); }

	private:
		fs::path _dir;
	};

	class R93FemasBdMgr : public IBaseDataMgr
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

	class R93FemasSink : public ITraderSpi
	{
	public:
		IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			events.push_back({ (int)e, ec });
			if (on_event)
				on_event(e);
		}
		void handleTraderLog(WTSLogLevel, const char* message) override
		{
			logs.push_back(message ? message : "");
		}
		void onLoginResult(bool succ, const char* msg, uint32_t date) override
		{
			logins.push_back({ succ, msg ? msg : "", date });
			if (on_login_result)
				on_login_result(succ);
		}
		void onLogout() override { ++n_logout_; }

		struct LoginRec { bool succ; std::string msg; uint32_t date; };

		R93FemasBdMgr bd_;
		std::vector<std::pair<int, int32_t>> events;
		std::vector<std::string> logs;
		std::vector<LoginRec> logins;
		std::function<void(WTSTraderEvent)> on_event;
		std::function<void(bool)> on_login_result;
		int n_logout_ = 0;
	};

	class R93FemasProbe : public TraderFemas
	{
	public:
		void setApi(CUstpFtdcTraderApi* api) { m_pUserAPI = api; }
		CUstpFtdcTraderApi* api() const { return m_pUserAPI; }
		CUstpFtdcTraderApi* createFromFactory() { return m_funcCreator == NULL ? NULL : m_funcCreator(""); }
		bool hasModule() const { return m_hInstCTP != NULL; }
		bool hasCreator() const { return m_funcCreator != NULL; }
		bool workerNull() const { return m_thrdWorker == NULL; }
		void setState(WrapperState state) { m_wrapperState = state; }
		void setQryOnline(bool value) { m_bQryOnline = value; }
		void setStrand(boost::asio::io_context::strand* strand) { m_strandIO = strand; }
		void setLastQueryTime(uint64_t value) { m_lastQryTime = value; }
		void setFactory(CUstpFtdcTraderApi* (*factory)(const char*)) { m_funcCreator = factory; }
		void setSessionID(const char* sid) { m_strSessionID = sid; }
		WrapperState state() const { return m_wrapperState; }
		bool queryInFlight() const { return m_bInQuery; }
		bool strandNull() const { return m_strandIO == NULL; }
		size_t runOneIo() { return m_asyncIO.poll_one(); }
		void presetWorker()
		{
			// connect() 只在 m_thrdWorker==NULL 时起常驻线程；预置未启动的
			// std::thread 占位（非 joinable），让连接用例完全不创建线程。
			m_thrdWorker.reset(new StdThread());
		}
		bool runNextQuery()
		{
			StdUniqueLock lock(m_mtxQuery);
			if (m_queQuery.empty())
				return false;
			CommonExecuter handler = m_queQuery.front();
			handler();
			m_queQuery.pop();
			return true;
		}
		size_t queued()
		{
			StdUniqueLock lock(m_mtxQuery);
			return m_queQuery.size();
		}
	};

	R93FemasApiStub* g_r93_femas_stub = NULL;
	CUstpFtdcTraderApi* r93_femas_create(const char*) { return g_r93_femas_stub; }
	CUstpFtdcRspInfoField r93_femas_rsp(int errId, const char* msg);

	class R93FemasInitCallbackApiStub : public R93FemasApiStub
	{
	public:
		enum CallbackMode { NO_CALLBACK, LOGIN_SUCCESS, LOGIN_FAILURE, THROW_INIT };
		CallbackMode callback_mode_ = NO_CALLBACK;

		void Init() override
		{
			++n_init_;
			if (callback_mode_ == THROW_INIT)
				throw std::runtime_error("fake Init failure");
			if (spi_ == NULL || callback_mode_ == NO_CALLBACK)
				return;

			// Connect and authenticate synchronously through the same callback chain
			// that can reach onLoginResult() while Init() is still on the stack.
			spi_->OnFrontConnected();
		}

		int ReqDSUserCertification(CUstpFtdcDSUserInfoField* request, int requestId) override
		{
			R93FemasApiStub::ReqDSUserCertification(request, requestId);
			CUstpFtdcDSUserCertRspDataField cert = {};
			CUstpFtdcRspInfoField response = r93_femas_rsp(0, "");
			if (spi_ != NULL)
				spi_->OnRspDSUserCertification(&cert, &response, requestId, true);
			return stub_ret_;
		}

		int ReqUserLogin(CUstpFtdcReqUserLoginField* request, int requestId) override
		{
			R93FemasApiStub::ReqUserLogin(request, requestId);
			CUstpFtdcRspUserLoginField login = {};
			strncpy(login.MaxOrderLocalID, "00000042", sizeof(login.MaxOrderLocalID) - 1);
			CUstpFtdcRspInfoField response = r93_femas_rsp(
				callback_mode_ == LOGIN_FAILURE ? 1 : 0,
				callback_mode_ == LOGIN_FAILURE ? "fake synchronous login failure" : "");
			if (spi_ != NULL)
				spi_->OnRspUserLogin(&login, &response, requestId, true);
			if (spi_ != NULL && callback_mode_ != LOGIN_FAILURE && callback_mode_ != THROW_INIT)
			{
				CUstpFtdcRspUserLoginField queryLogin = {};
				CUstpFtdcRspInfoField queryResponse = r93_femas_rsp(0, "");
				spi_->OnRspQueryUserLogin(&queryLogin, &queryResponse, requestId, true);
			}
			return stub_ret_;
		}
	};

	CUstpFtdcRspInfoField r93_femas_rsp(int errId, const char* msg)
	{
		CUstpFtdcRspInfoField rsp;
		memset(&rsp, 0, sizeof(rsp));
		rsp.ErrorID = errId;
		strncpy(rsp.ErrorMsg, msg ? msg : "", sizeof(rsp.ErrorMsg) - 1);
		return rsp;
	}

	WTSVariant* r93_femas_config(const std::string& flowDir, const char* module)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("front", "tcp://127.0.0.1:17001");
		cfg->append("query", "tcp://127.0.0.1:17002");
		cfg->append("broker", "9999");
		cfg->append("user", "femasuser");
		cfg->append("pass", "femaspass");
		cfg->append("appid", "APP-93");
		cfg->append("authcode", "AUTH-93");
		cfg->append("flowdir", flowDir.c_str());
		cfg->append("ctpmodule", module);
		return cfg;
	}

	class R93FemasTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			comm_ = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "TRADING", "CHINA");
			comm_->setCategory(CC_Future);
			comm_->setCoverMode(CM_CoverToday);
			comm_->setTradingMode(TM_Both);
			comm_->setVolScale(10);
			ct_ = WTSContractInfo::create("rb2401", "螺纹钢2401", "SHFE", "rb");
			ct_->setCommInfo(comm_);

			probe_.registerSpi(&sink_);

			WTSVariant* cfg = r93_femas_config(temp_.path("flow_base"), WT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE);
			ASSERT_TRUE(probe_.init(cfg));
			cfg->release();
		}

		void TearDown() override
		{
			probe_.release();
			// 本文件只创建了合约/品种，不注册进 bdMgr，因此这里显式释放一次
			ct_->release();
			comm_->release();
		}

		void make_ready()
		{
			probe_.setApi(&stub_);
			probe_.setState(TraderFemas::WS_ALLREADY);
			probe_.setSessionID("S1");
		}

	bool logs_contain(const R93FemasSink& sink, const char* fragment) const
	{
		for (const std::string& log : sink.logs)
		{
			if (log.find(fragment) != std::string::npos)
				return true;
		}
		return false;
	}

		R93FemasTempDir temp_;
		WTSCommodityInfo* comm_ = NULL;
		WTSContractInfo* ct_ = NULL;
		R93FemasSink sink_;
		R93FemasApiStub stub_;
		R93FemasProbe probe_;
	};

	/* ------------------------------------------------------------------ */
	/* 连接 / 释放                                                          */
	/* ------------------------------------------------------------------ */

	/// TraderFemas.cpp L140-212：release 的 RegisterSpi(NULL)+Release；connect 的
	/// flow 目录、creator、RegisterSpi/Front/QryFront、RESUME 订阅、心跳、日志文件、Init。
	TEST_F(R93FemasTest, connect_and_release_drive_stub_lifecycle)
	{
		g_r93_femas_stub = &stub_;
		probe_.setFactory(&r93_femas_create);
		probe_.presetWorker();

		probe_.connect();
		ASSERT_NE(nullptr, stub_.spi_);
		EXPECT_EQ(static_cast<CUstpFtdcTraderSpi*>(&probe_), stub_.spi_);
		EXPECT_EQ(USTP_TERT_RESUME, stub_.private_topic_);
		EXPECT_EQ(USTP_TERT_RESUME, stub_.public_topic_);
		EXPECT_EQ("tcp://127.0.0.1:17001", stub_.front_);
		EXPECT_EQ("tcp://127.0.0.1:17002", stub_.qry_front_);
		EXPECT_EQ(60u, stub_.heartbeat_);
		EXPECT_EQ(1, stub_.n_init_);
		EXPECT_NE(std::string::npos, stub_.req_log_.find("femas_req.log"));
		EXPECT_NE(std::string::npos, stub_.rsp_log_.find("femas_res.log"));

		probe_.release();
		EXPECT_EQ(1, stub_.n_release_);
		EXPECT_EQ(nullptr, stub_.spi_);
	}

	TEST_F(R93FemasTest, connect_init_sync_success_can_reenter_query_account)
	{
		R93FemasInitCallbackApiStub initStub;
		initStub.callback_mode_ = R93FemasInitCallbackApiStub::LOGIN_SUCCESS;
		g_r93_femas_stub = &initStub;
		probe_.setFactory(&r93_femas_create);
		probe_.presetWorker();
		int queryResult = -99;
		sink_.on_event = [this](WTSTraderEvent event) {
			if (event == WTE_Connect)
				probe_.login("femasuser", "femaspass", "r93-init-sync");
		};
		sink_.on_login_result = [this, &queryResult](bool success) {
			if (success)
				queryResult = probe_.queryAccount();
		};

		probe_.connect();
		ASSERT_EQ(1, initStub.n_init_);
		ASSERT_EQ(1, initStub.n_auth_);
		ASSERT_EQ(1, initStub.n_login_);
		ASSERT_EQ(1u, sink_.logins.size());
		ASSERT_TRUE(sink_.logins.front().succ);
		EXPECT_EQ(0, queryResult);
		EXPECT_FALSE(probe_.strandNull());
		ASSERT_EQ(1u, probe_.queued());

		// The fake executor drains the work after Init has returned, matching the
		// production worker's first run while avoiding an unbounded test thread.
		EXPECT_EQ(1u, probe_.runOneIo());
		EXPECT_EQ(1, initStub.n_qry_account_);
		EXPECT_EQ(0u, probe_.queued());

		sink_.on_login_result = nullptr;
		sink_.on_event = nullptr;
		probe_.release();
		g_r93_femas_stub = NULL;
	}

	TEST_F(R93FemasTest, connect_init_sync_failed_login_does_not_reenter_query)
	{
		R93FemasInitCallbackApiStub initStub;
		initStub.callback_mode_ = R93FemasInitCallbackApiStub::LOGIN_FAILURE;
		g_r93_femas_stub = &initStub;
		probe_.setFactory(&r93_femas_create);
		probe_.presetWorker();
		int reentrantQueries = 0;
		sink_.on_event = [this](WTSTraderEvent event) {
			if (event == WTE_Connect)
				probe_.login("femasuser", "femaspass", "r93-init-sync");
		};
		sink_.on_login_result = [&reentrantQueries](bool success) {
			if (success)
				++reentrantQueries;
		};

		probe_.connect();
		ASSERT_EQ(1, initStub.n_init_);
		ASSERT_EQ(1, initStub.n_auth_);
		ASSERT_EQ(1, initStub.n_login_);
		ASSERT_EQ(1u, sink_.logins.size());
		EXPECT_FALSE(sink_.logins.front().succ);
		EXPECT_EQ(TraderFemas::WS_LOGINFAILED, probe_.state());
		EXPECT_EQ(0, reentrantQueries);
		EXPECT_FALSE(probe_.strandNull());
		EXPECT_EQ(0u, probe_.queued());
		EXPECT_EQ(0, initStub.n_qry_account_);

		sink_.on_login_result = nullptr;
		sink_.on_event = nullptr;
		probe_.release();
		g_r93_femas_stub = NULL;
	}

	TEST_F(R93FemasTest, connect_init_without_callback_allows_post_init_async_query)
	{
		R93FemasInitCallbackApiStub initStub;
		g_r93_femas_stub = &initStub;
		probe_.setFactory(&r93_femas_create);
		probe_.presetWorker();
		int queryResult = -99;
		sink_.on_event = [this](WTSTraderEvent event) {
			if (event == WTE_Connect)
				probe_.login("femasuser", "femaspass", "r93-init-async");
		};
		sink_.on_login_result = [this, &queryResult](bool success) {
			if (success)
				queryResult = probe_.queryAccount();
		};

		probe_.connect();
		EXPECT_EQ(1, initStub.n_init_);
		EXPECT_TRUE(sink_.logins.empty());
		EXPECT_FALSE(probe_.strandNull());
		EXPECT_EQ(0u, probe_.queued());

		ASSERT_NE(nullptr, initStub.spi_);
		initStub.spi_->OnFrontConnected();
		ASSERT_EQ(1u, sink_.logins.size());
		EXPECT_TRUE(sink_.logins.front().succ);
		EXPECT_EQ(0, queryResult);
		ASSERT_EQ(1u, probe_.queued());
		EXPECT_EQ(1u, probe_.runOneIo());
		EXPECT_EQ(1, initStub.n_qry_account_);
		EXPECT_EQ(0u, probe_.queued());

		sink_.on_login_result = nullptr;
		sink_.on_event = nullptr;
		probe_.release();
		g_r93_femas_stub = NULL;
	}

	TEST_F(R93FemasTest, connect_init_exception_does_not_start_a_waiting_worker)
	{
		R93FemasInitCallbackApiStub initStub;
		initStub.callback_mode_ = R93FemasInitCallbackApiStub::THROW_INIT;
		g_r93_femas_stub = &initStub;
		probe_.setFactory(&r93_femas_create);

		EXPECT_THROW(probe_.connect(), std::runtime_error);
		EXPECT_EQ(1, initStub.n_init_);
		EXPECT_TRUE(probe_.workerNull());
		EXPECT_FALSE(probe_.strandNull());
		EXPECT_EQ(static_cast<CUstpFtdcTraderApi*>(&initStub), probe_.api());

		probe_.release();
		EXPECT_EQ(1, initStub.n_release_);
		EXPECT_EQ(nullptr, initStub.spi_);
		EXPECT_EQ(nullptr, probe_.api());
		g_r93_femas_stub = NULL;
	}

	TEST_F(R93FemasTest, init_missing_module_fails_and_void_connect_stops_with_diagnostic)
	{
		R93FemasProbe probe;
		R93FemasSink sink;
		probe.registerSpi(&sink);
		WTSVariant* cfg = r93_femas_config(temp_.path("missing_module"), WT_T4_FEMAS_MISSING_LIBRARY_MODULE);

		EXPECT_FALSE(probe.init(cfg));
		cfg->release();
		EXPECT_FALSE(probe.hasModule());
		EXPECT_FALSE(probe.hasCreator());
		EXPECT_TRUE(logs_contain(sink, "Failed to load trader API module"));

		EXPECT_NO_THROW(probe.connect());
		EXPECT_EQ(nullptr, probe.api());
		EXPECT_TRUE(probe.workerNull());
		EXPECT_TRUE(logs_contain(sink, "creator is unavailable"));
		probe.release();
	}

	TEST_F(R93FemasTest, init_missing_creator_symbol_fails_and_connect_does_not_call_api)
	{
		R93FemasProbe probe;
		R93FemasSink sink;
		probe.registerSpi(&sink);
		WTSVariant* cfg = r93_femas_config(temp_.path("missing_symbol"), WT_T4_FEMAS_MISSING_SYMBOL_MODULE);

		EXPECT_FALSE(probe.init(cfg));
		cfg->release();
		EXPECT_FALSE(probe.hasModule());
		EXPECT_FALSE(probe.hasCreator());
		EXPECT_TRUE(logs_contain(sink, "creator symbol not found"));

		EXPECT_NO_THROW(probe.connect());
		EXPECT_EQ(nullptr, probe.api());
		EXPECT_TRUE(probe.workerNull());
		EXPECT_TRUE(logs_contain(sink, "creator is unavailable"));
		probe.release();
	}

	TEST_F(R93FemasTest, connect_creator_returning_null_fails_closed_with_diagnostic)
	{
		R93FemasProbe probe;
		R93FemasSink sink;
		probe.registerSpi(&sink);
		WTSVariant* cfg = r93_femas_config(temp_.path("null_creator"), WT_T4_FEMAS_NULL_CREATOR_MODULE);

		ASSERT_TRUE(probe.init(cfg));
		cfg->release();
		ASSERT_TRUE(probe.hasModule());
		ASSERT_TRUE(probe.hasCreator());
		EXPECT_NO_THROW(probe.connect());
		EXPECT_EQ(nullptr, probe.api());
		EXPECT_TRUE(probe.workerNull());
		EXPECT_TRUE(logs_contain(sink, "creator returned null"));
		probe.release();
	}

	TEST_F(R93FemasTest, repeated_init_is_rejected_without_replacing_loaded_creator)
	{
		R93FemasProbe probe;
		R93FemasSink sink;
		probe.registerSpi(&sink);
		probe.presetWorker();
		WTSVariant* cfg = r93_femas_config(temp_.path("reinit_first"), WT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE);
		ASSERT_TRUE(probe.init(cfg));
		cfg->release();
		EXPECT_FALSE(probe.init(nullptr));
		EXPECT_TRUE(probe.hasModule());
		EXPECT_TRUE(probe.hasCreator());

		WTSVariant* rejected = r93_femas_config(temp_.path("reinit_rejected"), WT_T4_FEMAS_MISSING_LIBRARY_MODULE);
		EXPECT_FALSE(probe.init(rejected));
		rejected->release();
		EXPECT_TRUE(probe.hasModule());
		EXPECT_TRUE(probe.hasCreator());
		EXPECT_TRUE(logs_contain(sink, "Cannot initialize an API instance more than once"));

		probe.connect();
		ASSERT_NE(nullptr, probe.api());
		probe.release();
	}

	TEST_F(R93FemasTest, failed_init_can_be_retried_with_valid_module)
	{
		R93FemasProbe probe;
		R93FemasSink sink;
		probe.registerSpi(&sink);
		probe.presetWorker();
		WTSVariant* missing = r93_femas_config(temp_.path("retry_missing"), WT_T4_FEMAS_MISSING_LIBRARY_MODULE);
		EXPECT_FALSE(probe.init(missing));
		missing->release();
		EXPECT_FALSE(probe.hasModule());
		EXPECT_FALSE(probe.hasCreator());

		WTSVariant* valid = r93_femas_config(temp_.path("retry_valid"), WT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE);
		ASSERT_TRUE(probe.init(valid));
		valid->release();
		EXPECT_TRUE(probe.hasModule());
		EXPECT_TRUE(probe.hasCreator());
		probe.connect();
		ASSERT_NE(nullptr, probe.api());
		probe.release();
	}

	TEST_F(R93FemasTest, dynamic_creator_success_connects_fake_api_as_positive_control)
	{
		R93FemasProbe probe;
		R93FemasSink sink;
		probe.registerSpi(&sink);
		probe.presetWorker();
		WTSVariant* cfg = r93_femas_config(temp_.path("successful_creator"), WT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE);

		ASSERT_TRUE(probe.init(cfg));
		cfg->release();
		ASSERT_TRUE(probe.hasModule());
		ASSERT_TRUE(probe.hasCreator());
		R93FemasApiStub* api = static_cast<R93FemasApiStub*>(probe.createFromFactory());
		ASSERT_NE(nullptr, api);
		const int initBefore = api->n_init_;
		const int releaseBefore = api->n_release_;
		probe.connect();

		ASSERT_NE(nullptr, probe.api());
		EXPECT_EQ(initBefore + 1, api->n_init_);
		EXPECT_EQ(static_cast<CUstpFtdcTraderSpi*>(&probe), api->spi_);
		EXPECT_EQ(USTP_TERT_RESUME, api->public_topic_);
		EXPECT_EQ(USTP_TERT_RESUME, api->private_topic_);

		probe.release();
		EXPECT_EQ(releaseBefore + 1, api->n_release_);
		EXPECT_EQ(nullptr, api->spi_);
	}

	/// TraderFemas.cpp L169-212 的 quick 分支：SubscribePublic/Private 使用 USTP_TERT_QUICK。
	TEST_F(R93FemasTest, quick_connect_subscribes_quick_topics)
	{
		R93FemasApiStub stub;
		R93FemasProbe probe;
		g_r93_femas_stub = &stub;
		probe.presetWorker();

		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("front", "tcp://127.0.0.1:17001");
		cfg->append("broker", "9999");
		cfg->append("user", "femasuser");
		cfg->append("pass", "femaspass");
		cfg->append("quick", true);
		cfg->append("flowdir", temp_.path("flow_quick").c_str());
		cfg->append("ctpmodule", WT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE);
		ASSERT_TRUE(probe.init(cfg));
		cfg->release();
		probe.setFactory(&r93_femas_create);

		probe.connect();
		EXPECT_EQ(USTP_TERT_QUICK, stub.private_topic_);
		EXPECT_EQ(USTP_TERT_QUICK, stub.public_topic_);
		probe.release();
		EXPECT_EQ(1, stub.n_release_);
	}

	/* ------------------------------------------------------------------ */
	/* 认证 → 登录 → 就绪                                                   */
	/* ------------------------------------------------------------------ */

	/// TraderFemas.cpp L260-303（authenticate/doLogin/login）、L551-595（认证与登录应答）、
	/// L543-549 + L804-810（查询前置登录后 onInitialized → ALLREADY）。
	TEST_F(R93FemasTest, authenticate_login_and_query_front_reach_ready)
	{
		probe_.setApi(&stub_);

		ASSERT_EQ(0, probe_.login("femasuser", "femaspass", "prod-93"));
		ASSERT_EQ(1, stub_.n_auth_);
		EXPECT_STREQ("APP-93", stub_.auth_req_.AppID);
		EXPECT_STREQ("AUTH-93", stub_.auth_req_.AuthCode);
		EXPECT_EQ('1', stub_.auth_req_.EncryptType);
		EXPECT_EQ(TraderFemas::WS_LOGINING, probe_.state());

		CUstpFtdcRspInfoField ok = r93_femas_rsp(0, "");
		probe_.OnRspDSUserCertification(NULL, &ok, 1, true);
		ASSERT_EQ(1, stub_.n_login_);
		EXPECT_STREQ("9999", stub_.login_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.login_req_.UserID);
		EXPECT_STREQ("femaspass", stub_.login_req_.Password);
		EXPECT_STREQ("prod-93", stub_.login_req_.UserProductInfo);

		CUstpFtdcRspUserLoginField loginData;
		memset(&loginData, 0, sizeof(loginData));
		strncpy(loginData.MaxOrderLocalID, "A000123", sizeof(loginData.MaxOrderLocalID) - 1);
		probe_.OnRspUserLogin(&loginData, &ok, 2, true);
		EXPECT_EQ(TraderFemas::WS_LOGINED, probe_.state());
		EXPECT_FALSE(probe_.isConnected());
		EXPECT_TRUE(sink_.logins.empty());

		probe_.OnRspQueryUserLogin(&loginData, &ok, 3, true);
		EXPECT_EQ(TraderFemas::WS_ALLREADY, probe_.state());
		EXPECT_TRUE(probe_.isConnected());
		ASSERT_EQ(1u, sink_.logins.size());
		EXPECT_TRUE(sink_.logins[0].succ);
		EXPECT_EQ(20260105u, sink_.logins[0].date);

		// 登录成功后 makeEntrustID 以 MaxOrderLocalID（去掉尾零）为前缀
		char eid[64] = { 0 };
		ASSERT_TRUE(probe_.makeEntrustID(eid, sizeof(eid)));
		EXPECT_EQ(0, std::string(eid).find("A000123"));
	}

	/* ------------------------------------------------------------------ */
	/* 下单 / 撤单 / 登出                                                    */
	/* ------------------------------------------------------------------ */

	/// TraderFemas.cpp L325-392：三种 OrderFlag 的 TimeCondition/VolumeCondition、
	/// 方向/开平、userTag→UserOrderLocalID、失败日志。
	TEST_F(R93FemasTest, order_insert_encodes_price_and_time_conditions)
	{
		make_ready();

		WTSEntrust* nor = WTSEntrust::create("rb2401", 2, 3500.0, "SHFE");
		nor->setContractInfo(ct_);
		nor->setDirection(WDT_LONG);
		nor->setOffsetType(WOT_OPEN);
		nor->setPriceType(WPT_ANYPRICE);
		nor->setOrderFlag(WOF_NOR);
		ASSERT_EQ(0, probe_.orderInsert(nor));
		EXPECT_EQ(1, stub_.n_order_insert_);
		EXPECT_STREQ("9999", stub_.order_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.order_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.order_req_.InvestorID);
		EXPECT_STREQ("rb2401", stub_.order_req_.InstrumentID);
		EXPECT_STREQ("SHFE", stub_.order_req_.ExchangeID);
		EXPECT_STREQ("S1000000000002", stub_.order_req_.UserOrderLocalID);
		EXPECT_EQ(USTP_FTDC_OPT_AnyPrice, stub_.order_req_.OrderPriceType);
		EXPECT_EQ(USTP_FTDC_D_Buy, stub_.order_req_.Direction);
		EXPECT_EQ(USTP_FTDC_OF_Open, stub_.order_req_.OffsetFlag);
		EXPECT_EQ(USTP_FTDC_CHF_Speculation, stub_.order_req_.HedgeFlag);
		EXPECT_DOUBLE_EQ(3500.0, stub_.order_req_.LimitPrice);
		EXPECT_EQ(2, stub_.order_req_.Volume);
		EXPECT_EQ(USTP_FTDC_TC_GFD, stub_.order_req_.TimeCondition);
		EXPECT_EQ(USTP_FTDC_VC_AV, stub_.order_req_.VolumeCondition);
		EXPECT_EQ(USTP_FTDC_FCR_NotForceClose, stub_.order_req_.ForceCloseReason);
		nor->release();

		WTSEntrust* fak = WTSEntrust::create("rb2401", 3, 3510.0, "SHFE");
		fak->setContractInfo(ct_);
		fak->setDirection(WDT_SHORT);
		fak->setOffsetType(WOT_CLOSE);
		fak->setPriceType(WPT_LIMITPRICE);
		fak->setOrderFlag(WOF_FAK);
		fak->setUserTag("TAG-77");
		ASSERT_EQ(0, probe_.orderInsert(fak));
		EXPECT_EQ(2, stub_.n_order_insert_);
		EXPECT_STREQ("TAG-77", stub_.order_req_.UserOrderLocalID);
		EXPECT_EQ(USTP_FTDC_OPT_LimitPrice, stub_.order_req_.OrderPriceType);
		EXPECT_EQ(USTP_FTDC_D_Buy, stub_.order_req_.Direction);	// 空平 = 买
		EXPECT_EQ(USTP_FTDC_OF_Close, stub_.order_req_.OffsetFlag);
		EXPECT_EQ(USTP_FTDC_TC_IOC, stub_.order_req_.TimeCondition);
		EXPECT_EQ(USTP_FTDC_VC_AV, stub_.order_req_.VolumeCondition);
		fak->release();

		WTSEntrust* fok = WTSEntrust::create("rb2401", 1, 3520.0, "SHFE");
		fok->setContractInfo(ct_);
		fok->setDirection(WDT_LONG);
		fok->setOffsetType(WOT_CLOSE);
		fok->setPriceType(WPT_LIMITPRICE);
		fok->setOrderFlag(WOF_FOK);
		ASSERT_EQ(0, probe_.orderInsert(fok));
		EXPECT_EQ(3, stub_.n_order_insert_);
		EXPECT_EQ(USTP_FTDC_TC_IOC, stub_.order_req_.TimeCondition);
		EXPECT_EQ(USTP_FTDC_VC_CV, stub_.order_req_.VolumeCondition);
		fok->release();

		stub_.stub_ret_ = -3;
		WTSEntrust* fail = WTSEntrust::create("rb2401", 1, 3530.0, "SHFE");
		fail->setContractInfo(ct_);
		ASSERT_EQ(0, probe_.orderInsert(fail));
		ASSERT_FALSE(sink_.logs.empty());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Order inserting failed: -3"));
		fail->release();
	}

	/// TraderFemas.cpp L305-323（logout 请求字段与失败日志）、L394-422（撤单字段：
	/// 本地委托号/系统号/CZCE→ZCE/ActionFlag 与失败日志）。
	TEST_F(R93FemasTest, order_action_logout_and_failure_logs)
	{
		make_ready();

		WTSEntrustAction* cancel = WTSEntrustAction::create("rb2401", "CZCE");
		cancel->setEntrustID("A0001230000000007");
		cancel->setOrderID("OS-9");
		cancel->setActionFlag(WAF_CANCEL);
		ASSERT_EQ(0, probe_.orderAction(cancel));
		EXPECT_EQ(1, stub_.n_order_action_);
		EXPECT_STREQ("9999", stub_.action_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.action_req_.InvestorID);
		EXPECT_STREQ("femasuser", stub_.action_req_.UserID);
		EXPECT_STREQ("A0001230000000007", stub_.action_req_.UserOrderLocalID);
		EXPECT_STREQ("S1000000000002", stub_.action_req_.UserOrderActionLocalID);
		EXPECT_EQ(USTP_FTDC_AF_Delete, stub_.action_req_.ActionFlag);
		EXPECT_STREQ("OS-9", stub_.action_req_.OrderSysID);
		EXPECT_STREQ("ZCE", stub_.action_req_.ExchangeID);
		cancel->release();

		WTSEntrustAction* modify = WTSEntrustAction::create("rb2401", "SHFE");
		modify->setEntrustID("A0001230000000008");
		modify->setOrderID("OS-10");
		modify->setActionFlag(WAF_MODIFY);
		ASSERT_EQ(0, probe_.orderAction(modify));
		EXPECT_EQ(2, stub_.n_order_action_);
		EXPECT_EQ(USTP_FTDC_AF_Modify, stub_.action_req_.ActionFlag);
		EXPECT_STREQ("SHFE", stub_.action_req_.ExchangeID);
		modify->release();

		ASSERT_EQ(0, probe_.logout());
		EXPECT_EQ(1, stub_.n_logout_);
		EXPECT_STREQ("9999", stub_.logout_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.logout_req_.UserID);

		stub_.stub_ret_ = -4;
		WTSEntrustAction* fail = WTSEntrustAction::create("rb2401", "SHFE");
		fail->setEntrustID("A1");
		fail->setOrderID("OS-11");
		ASSERT_EQ(0, probe_.orderAction(fail));
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Sending cancel request failed: -4"));
		fail->release();

		ASSERT_EQ(0, probe_.logout());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Sending logout request failed: -4"));
	}

	TEST_F(R93FemasTest, order_insert_accepts_maximum_sdk_tag_and_rejects_overlong_without_api_call)
	{
		make_ready();
		const std::size_t maxTagLength = sizeof(stub_.order_req_.UserOrderLocalID) - 1;
		const std::string maxTag(maxTagLength, 'T');
		WTSEntrust* valid = WTSEntrust::create("rb2401", 1, 3500.0, "SHFE");
		valid->setContractInfo(ct_);
		valid->setUserTag(maxTag.c_str());
		ASSERT_EQ(0, probe_.orderInsert(valid));
		EXPECT_EQ(1, stub_.n_order_insert_);
		EXPECT_STREQ(maxTag.c_str(), stub_.order_req_.UserOrderLocalID);
		valid->release();

		const std::string overlongTag(maxTagLength + 1, 'T');
		WTSEntrust* invalid = WTSEntrust::create("rb2401", 1, 3500.0, "SHFE");
		invalid->setContractInfo(ct_);
		invalid->setUserTag(overlongTag.c_str());
		EXPECT_EQ(-1, probe_.orderInsert(invalid));
		EXPECT_EQ(1, stub_.n_order_insert_);
		invalid->release();
	}

	TEST_F(R93FemasTest, order_insert_checks_generated_session_and_local_id_capacity)
	{
		make_ready();
		probe_.setSessionID("12345678");
		WTSEntrust* valid = WTSEntrust::create("rb2401", 1, 3500.0, "SHFE");
		valid->setContractInfo(ct_);
		ASSERT_EQ(0, probe_.orderInsert(valid));
		EXPECT_EQ(1, stub_.n_order_insert_);
		EXPECT_EQ(20u, std::strlen(stub_.order_req_.UserOrderLocalID));
		const std::string firstOrderLocalID = stub_.order_req_.UserOrderLocalID;
		EXPECT_EQ("12345678", firstOrderLocalID.substr(0, 8));
		const std::string firstOrderSequence = firstOrderLocalID.substr(8);
		ASSERT_EQ(12u, firstOrderSequence.size());
		const unsigned long firstOrderSequenceNumber = std::stoul(firstOrderSequence);
		valid->release();

		probe_.setSessionID("123456789");
		WTSEntrust* invalid = WTSEntrust::create("rb2401", 1, 3500.0, "SHFE");
		invalid->setContractInfo(ct_);
		EXPECT_EQ(-1, probe_.orderInsert(invalid));
		EXPECT_EQ(1, stub_.n_order_insert_);
		invalid->release();

		probe_.setSessionID("12345678");
		WTSEntrust* nextValid = WTSEntrust::create("rb2401", 1, 3500.0, "SHFE");
		nextValid->setContractInfo(ct_);
		ASSERT_EQ(0, probe_.orderInsert(nextValid));
		EXPECT_EQ(2, stub_.n_order_insert_);
		const std::string nextOrderLocalID = stub_.order_req_.UserOrderLocalID;
		EXPECT_EQ("12345678", nextOrderLocalID.substr(0, 8));
		const std::string nextOrderSequence = nextOrderLocalID.substr(8);
		ASSERT_EQ(12u, nextOrderSequence.size());
		EXPECT_EQ(firstOrderSequenceNumber + 1, std::stoul(nextOrderSequence));
		nextValid->release();
	}

	TEST_F(R93FemasTest, order_action_accepts_maximum_identifiers_and_rejects_overlong_without_api_call)
	{
		make_ready();
		probe_.setSessionID("12345678");
		const std::string maxEntrustID(sizeof(stub_.action_req_.UserOrderLocalID) - 1, 'E');
		const std::string maxOrderID(sizeof(stub_.action_req_.OrderSysID) - 1, 'O');
		WTSEntrustAction* valid = WTSEntrustAction::create("rb2401", "SHFE");
		valid->setEntrustID(maxEntrustID.c_str());
		valid->setOrderID(maxOrderID.c_str());
		ASSERT_EQ(0, probe_.orderAction(valid));
		EXPECT_EQ(1, stub_.n_order_action_);
		EXPECT_STREQ(maxEntrustID.c_str(), stub_.action_req_.UserOrderLocalID);
		EXPECT_STREQ(maxOrderID.c_str(), stub_.action_req_.OrderSysID);
		const std::string firstActionLocalID = stub_.action_req_.UserOrderActionLocalID;
		EXPECT_EQ("12345678", firstActionLocalID.substr(0, 8));
		const std::string firstActionSequence = firstActionLocalID.substr(8);
		ASSERT_EQ(12u, firstActionSequence.size());
		const unsigned long firstActionSequenceNumber = std::stoul(firstActionSequence);
		valid->release();

		const int callsBeforeRejects = stub_.n_order_action_;
		WTSEntrustAction* overlongEntrustID = WTSEntrustAction::create("rb2401", "SHFE");
		overlongEntrustID->setEntrustID(std::string(maxEntrustID.size() + 1, 'E').c_str());
		overlongEntrustID->setOrderID("ORDER-1");
		EXPECT_EQ(-1, probe_.orderAction(overlongEntrustID));
		EXPECT_EQ(callsBeforeRejects, stub_.n_order_action_);
		overlongEntrustID->release();

		WTSEntrustAction* overlongOrderID = WTSEntrustAction::create("rb2401", "SHFE");
		overlongOrderID->setEntrustID("EID-1");
		overlongOrderID->setOrderID(std::string(maxOrderID.size() + 1, 'O').c_str());
		EXPECT_EQ(-1, probe_.orderAction(overlongOrderID));
		EXPECT_EQ(callsBeforeRejects, stub_.n_order_action_);
		overlongOrderID->release();

		probe_.setSessionID("123456789");
		WTSEntrustAction* overlongGeneratedID = WTSEntrustAction::create("rb2401", "SHFE");
		overlongGeneratedID->setEntrustID("EID-2");
		overlongGeneratedID->setOrderID("ORDER-2");
		EXPECT_EQ(-1, probe_.orderAction(overlongGeneratedID));
		EXPECT_EQ(callsBeforeRejects, stub_.n_order_action_);
		overlongGeneratedID->release();

		probe_.setSessionID("12345678");
		WTSEntrustAction* nextValid = WTSEntrustAction::create("rb2401", "SHFE");
		nextValid->setEntrustID("EID-3");
		nextValid->setOrderID("ORDER-3");
		ASSERT_EQ(0, probe_.orderAction(nextValid));
		EXPECT_EQ(callsBeforeRejects + 1, stub_.n_order_action_);
		const std::string nextActionLocalID = stub_.action_req_.UserOrderActionLocalID;
		EXPECT_EQ("12345678", nextActionLocalID.substr(0, 8));
		const std::string nextActionSequence = nextActionLocalID.substr(8);
		ASSERT_EQ(12u, nextActionSequence.size());
		EXPECT_EQ(firstActionSequenceNumber + 1, std::stoul(nextActionSequence));
		nextValid->release();
	}

	/* ------------------------------------------------------------------ */
	/* 查询请求                                                              */
	/* ------------------------------------------------------------------ */

	/// queryAccount 经 strand 自动触发，验证账户请求组装及调度。
	TEST_F(R93FemasTest, query_account_is_scheduled_and_request_is_assembled)
	{
		make_ready();
		boost::asio::io_context ioc;
		boost::asio::io_context::strand strand(ioc);
		probe_.setStrand(&strand);

		ASSERT_EQ(0, probe_.queryAccount());
		ASSERT_EQ(1u, probe_.queued());
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_account_);
		EXPECT_STREQ("9999", stub_.qry_account_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_account_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_account_req_.InvestorID);
		EXPECT_EQ(0u, probe_.queued());
	}

	/// 只验证三个入队闭包的请求字段。runNextQuery() 是测试侧手工出队执行，
	/// 不代表 queryPositions/Orders/Trades 的产品调度已经成功。
	TEST_F(R93FemasTest, position_order_trade_requests_assemble_after_manual_test_drain)
	{
		make_ready();
		boost::asio::io_context ioc;
		boost::asio::io_context::strand strand(ioc);
		probe_.setStrand(&strand);

		ASSERT_EQ(0, probe_.queryPositions());
		ASSERT_EQ(1u, probe_.queued());
		// 人工执行队首闭包，仅用于观察其组装的请求字段。
		ASSERT_TRUE(probe_.runNextQuery());
		EXPECT_EQ(1, stub_.n_qry_position_);
		EXPECT_STREQ("9999", stub_.qry_position_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_position_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_position_req_.InvestorID);

		ASSERT_EQ(0, probe_.queryOrders());
		ASSERT_EQ(1u, probe_.queued());
		// 人工执行队首闭包，仅用于观察其组装的请求字段。
		ASSERT_TRUE(probe_.runNextQuery());
		EXPECT_EQ(1, stub_.n_qry_order_);
		EXPECT_STREQ("9999", stub_.qry_order_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_order_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_order_req_.InvestorID);

		ASSERT_EQ(0, probe_.queryTrades());
		ASSERT_EQ(1u, probe_.queued());
		// 人工执行队首闭包，仅用于观察其组装的请求字段。
		ASSERT_TRUE(probe_.runNextQuery());
		EXPECT_EQ(1, stub_.n_qry_trade_);
		EXPECT_STREQ("9999", stub_.qry_trade_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_trade_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_trade_req_.InvestorID);
	}

	/// 这些回归只调用公开查询入口并运行注入的单线程 io_context，不手工出队。
	/// 当前实现未在三个入口调用 triggerQuery()，因此它们会揭示 BUG-R93-003。
	TEST_F(R93FemasTest, standalone_position_query_is_scheduled_and_sent)
	{
		make_ready();
		boost::asio::io_context ioc;
		boost::asio::io_context::strand strand(ioc);
		probe_.setStrand(&strand);

		ASSERT_EQ(0, probe_.queryPositions());
		EXPECT_EQ(1u, probe_.queued());
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_position_);
		EXPECT_EQ(0u, probe_.queued());
		EXPECT_STREQ("9999", stub_.qry_position_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_position_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_position_req_.InvestorID);
	}

	TEST_F(R93FemasTest, standalone_order_query_is_scheduled_and_sent)
	{
		make_ready();
		boost::asio::io_context ioc;
		boost::asio::io_context::strand strand(ioc);
		probe_.setStrand(&strand);

		ASSERT_EQ(0, probe_.queryOrders());
		EXPECT_EQ(1u, probe_.queued());
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_order_);
		EXPECT_EQ(0u, probe_.queued());
		EXPECT_STREQ("9999", stub_.qry_order_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_order_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_order_req_.InvestorID);
	}

	TEST_F(R93FemasTest, standalone_trade_query_is_scheduled_and_sent)
	{
		make_ready();
		boost::asio::io_context ioc;
		boost::asio::io_context::strand strand(ioc);
		probe_.setStrand(&strand);

		ASSERT_EQ(0, probe_.queryTrades());
		EXPECT_EQ(1u, probe_.queued());
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_trade_);
		EXPECT_EQ(0u, probe_.queued());
		EXPECT_STREQ("9999", stub_.qry_trade_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_trade_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_trade_req_.InvestorID);
	}

	TEST_F(R93FemasTest, queued_queries_are_serial_and_final_error_callbacks_advance)
	{
		make_ready();
		boost::asio::io_context ioc;
		boost::asio::io_context::strand strand(ioc);
		probe_.setStrand(&strand);

		ASSERT_EQ(0, probe_.queryPositions());
		ASSERT_EQ(0, probe_.queryOrders());
		ASSERT_EQ(0, probe_.queryTrades());
		EXPECT_EQ(3u, probe_.queued());

		// All entry points schedule the same strand. Only the first request starts
		// while it is in flight; the two remaining notifications must not duplicate it.
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_position_);
		EXPECT_EQ(0, stub_.n_qry_order_);
		EXPECT_EQ(0, stub_.n_qry_trade_);
		EXPECT_EQ(2u, probe_.queued());
		EXPECT_TRUE(probe_.queryInFlight());

		CUstpFtdcRspInfoField ok = r93_femas_rsp(0, "");
		CUstpFtdcRspInfoField failed = r93_femas_rsp(8, "query rejected");
		probe_.OnRspQryInvestorPosition(NULL, &ok, 1, false);
		EXPECT_EQ(0, stub_.n_qry_order_);	// Non-final response does not advance the queue.
		EXPECT_TRUE(probe_.queryInFlight());

		// A terminal error response still releases the in-flight slot and schedules
		// the next request. Reset only the test clock anchor to avoid the 1s throttle.
		probe_.setLastQueryTime(0);
		probe_.OnRspQryInvestorPosition(NULL, &failed, 1, true);
		ioc.restart();
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_order_);
		EXPECT_EQ(0, stub_.n_qry_trade_);
		EXPECT_EQ(1u, probe_.queued());
		EXPECT_TRUE(probe_.queryInFlight());

		probe_.setLastQueryTime(0);
		probe_.OnRspQryOrder(NULL, &failed, 2, true);
		ioc.restart();
		ioc.run();
		EXPECT_EQ(1, stub_.n_qry_trade_);
		EXPECT_EQ(0u, probe_.queued());
		EXPECT_TRUE(probe_.queryInFlight());

		probe_.setLastQueryTime(0);
		probe_.OnRspQryTrade(NULL, &failed, 3, true);
		ioc.restart();
		ioc.run();
		EXPECT_EQ(0u, probe_.queued());
		EXPECT_FALSE(probe_.queryInFlight());
	}
} // namespace
