/*
 * lane R93（第十七波）— TraderFemas 有 API 指针半边
 *
 * 目标：src/TraderFemas/TraderFemas.cpp。用 r93_femas_api_stub.h 的机械空实现
 * 替代真实 CUstpFtdcTraderApi，覆盖 connect/release、认证→登录→查询前置→就绪、
 * orderInsert/orderAction/logout、四个查询入口的请求组装。不连真实柜台。
 *
 * 既有 T4 已覆盖：init/dll 失败留白、回调解析、枚举换算、无 API 拒绝面。
 * 本文件不重复这些断言；queryPositions/Orders/Trades 只入队不触发发送的现状
 * 以「测试侧手工执行队列」驱动，缺陷登记在 bugs/R93.md。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
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
		void handleEvent(WTSTraderEvent e, int32_t ec) override { events.push_back({ (int)e, ec }); }
		void handleTraderLog(WTSLogLevel, const char* message) override
		{
			logs.push_back(message ? message : "");
		}
		void onLoginResult(bool succ, const char* msg, uint32_t date) override
		{
			logins.push_back({ succ, msg ? msg : "", date });
		}
		void onLogout() override { ++n_logout_; }

		struct LoginRec { bool succ; std::string msg; uint32_t date; };

		R93FemasBdMgr bd_;
		std::vector<std::pair<int, int32_t>> events;
		std::vector<std::string> logs;
		std::vector<LoginRec> logins;
		int n_logout_ = 0;
	};

	class R93FemasProbe : public TraderFemas
	{
	public:
		void setApi(CUstpFtdcTraderApi* api) { m_pUserAPI = api; }
		void setState(WrapperState state) { m_wrapperState = state; }
		void setStrand(boost::asio::io_context::strand* strand) { m_strandIO = strand; }
		void setFactory(CUstpFtdcTraderApi* (*factory)(const char*)) { m_funcCreator = factory; }
		void setSessionID(const char* sid) { m_strSessionID = sid; }
		WrapperState state() const { return m_wrapperState; }
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

	CUstpFtdcRspInfoField r93_femas_rsp(int errId, const char* msg)
	{
		CUstpFtdcRspInfoField rsp;
		memset(&rsp, 0, sizeof(rsp));
		rsp.ErrorID = errId;
		strncpy(rsp.ErrorMsg, msg ? msg : "", sizeof(rsp.ErrorMsg) - 1);
		return rsp;
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

			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("front", "tcp://127.0.0.1:17001");
			cfg->append("query", "tcp://127.0.0.1:17002");
			cfg->append("broker", "9999");
			cfg->append("user", "femasuser");
			cfg->append("pass", "femaspass");
			cfg->append("appid", "APP-93");
			cfg->append("authcode", "AUTH-93");
			cfg->append("flowdir", temp_.path("flow_base").c_str());
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

	/* ------------------------------------------------------------------ */
	/* 查询请求                                                              */
	/* ------------------------------------------------------------------ */

	/// TraderFemas.cpp L424-506：queryAccount 经 strand 触发；其余三个只入队，
	/// 由测试探针取出执行，逐字段校验 BrokerID/UserID/InvestorID。
	TEST_F(R93FemasTest, query_entries_assemble_requests)
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

		ASSERT_EQ(0, probe_.queryPositions());
		ASSERT_EQ(1u, probe_.queued());
		ASSERT_TRUE(probe_.runNextQuery());
		EXPECT_EQ(1, stub_.n_qry_position_);
		EXPECT_STREQ("9999", stub_.qry_position_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_position_req_.UserID);
		EXPECT_STREQ("femasuser", stub_.qry_position_req_.InvestorID);

		ASSERT_EQ(0, probe_.queryOrders());
		ASSERT_EQ(1u, probe_.queued());
		ASSERT_TRUE(probe_.runNextQuery());
		EXPECT_EQ(1, stub_.n_qry_order_);
		EXPECT_STREQ("9999", stub_.qry_order_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_order_req_.InvestorID);

		ASSERT_EQ(0, probe_.queryTrades());
		ASSERT_EQ(1u, probe_.queued());
		ASSERT_TRUE(probe_.runNextQuery());
		EXPECT_EQ(1, stub_.n_qry_trade_);
		EXPECT_STREQ("9999", stub_.qry_trade_req_.BrokerID);
		EXPECT_STREQ("femasuser", stub_.qry_trade_req_.InvestorID);
	}
} // namespace
