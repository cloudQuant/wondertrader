/*
 * lane R26 — CTPLoader 登录/查询状态机用例
 *
 * 目标：src/CTPLoader/TraderSpi.cpp 的 ReqAuth/ReqUserLogin/OnRsp* 全链路 +
 *       LoadFromJson + ReqQryCommission/ReqQryMargin + 费率回报主体。
 *
 * 接缝：`pUserApi` 是 CTPLoader.cpp L27 的全局裸指针，测试用 T26CtpApiStub（107 个纯虚的
 * 机械替身，不起线程不连前置）替身它；回报全部由用例自己造的 CThostFtdc*Field 直接喂给
 * spi 的 OnRsp*。前置顺序按 FAIL裁定 A-07：OnFrontConnected → ReqAuthenticate →
 * OnRspAuthenticate → ReqUserLogin → OnRspUserLogin（FRONT_ID/SESSION_ID/ORDER_REF 这一组
 * 「会话已写入」断言必须由 OnRspUserLogin 置位，前面任何一步都不写）。
 *
 * 队列 worker 由条件变量唤醒，析构会停止并 join；队列并发与空闲停止回归由本文件的
 * 子进程测试覆盖。其他历史用例仍沿用堆上 spi，避免扩展本次测试修改范围。
 */
#include "t26_ctploader_common.hpp"
#include "t26_ctp_api_stub.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <sys/wait.h>
#include <signal.h>

#ifdef WT_T26_LOADJSON_TEST_HOOKS
namespace t26_test::load_json_test_hook
{
	void reset();
	size_t loads();
	size_t releases();
	bool every_loaded_variant_released_once();
}
#endif

struct CTraderSpiQueueTestAccess
{
	static void load_from_json(CTraderSpi& spi)
	{
		spi.LoadFromJson();
	}

	static bool wait_until_stopped(CTraderSpi& spi, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(spi._mtx);
		return spi._query_ready.wait_for(lock, timeout, [&spi]() { return spi._stopped; });
	}
};

namespace
{
	// 替身必须活得比 worker 线程久：堆上长命对象，进程退出即回收
	T26CtpApiStub& fresh_stub()
	{
		return *(new T26CtpApiStub());
	}

	CThostFtdcRspUserLoginField make_login_rsp(int front, int session, const char* max_ref)
	{
		CThostFtdcRspUserLoginField f;
		std::memset(&f, 0, sizeof(f));
		f.FrontID = front;
		f.SessionID = session;
		snprintf(f.MaxOrderRef, sizeof(f.MaxOrderRef), "%s", max_ref);
		return f;
	}

	// 触发过队列的 spi 不析构（见文件头注释），用例统一走这个工厂
	CTraderSpi* leaked_spi()
	{
		return new CTraderSpi();
	}

	// 用一条可控的 SHFE 期货合约把 _contracts 填上（bIsLast=false，不触 exit、不落队列）
	void seed_shfe_future(CTraderSpi& spi, const std::string& inst, const std::string& product)
	{
		auto f = t26::make_instrument("SHFE", inst, product, THOST_FTDC_PC_Futures);
		t26::set_fixed(f.InstrumentName, sizeof(f.InstrumentName), "seed");
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrument(&f, &ok, 20, false);
	}

	class ReentrantQueueApiStub : public T26CtpApiStub
	{
	public:
		void set_spi(CTraderSpi* spi) { _spi = spi; }

		int ReqQryInstrument(CThostFtdcQryInstrumentField* field, int requestID) override
		{
			int result = T26CtpApiStub::ReqQryInstrument(field, requestID);
			if (_spi != NULL)
			{
				// Exercise a synchronous callback reentering AppendQuery while the
				// worker is still inside the API call.
				emit_instrument_last();

				// Exercise multiple callback producers adding to the same queue.
				std::thread first([this]() { emit_instrument_last(); });
				std::thread second([this]() { emit_instrument_last(); });
				first.join();
				second.join();
			}
			return result;
		}

		int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField* field,
			int requestID) override
		{
			int result = T26CtpApiStub::ReqQryInstrumentCommissionRate(field, requestID);
			{
				std::lock_guard<std::mutex> lock(_mutex);
				++_commission_count;
			}
			_ready.notify_all();
			return result;
		}

		int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField* field,
			int requestID) override
		{
			int result = T26CtpApiStub::ReqQryInstrumentMarginRate(field, requestID);
			{
				std::lock_guard<std::mutex> lock(_mutex);
				++_margin_count;
			}
			_ready.notify_all();
			return result;
		}

		bool wait_for_all_rate_requests(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _ready.wait_for(lock, timeout, [this]() {
				return _commission_count == 3 && _margin_count == 3;
			});
		}

	private:
		void emit_instrument_last()
		{
			auto ok = t26::ok_rsp();
			_spi->OnRspQryInstrument(NULL, &ok, ++_callback_id, true);
		}

		CTraderSpi* _spi = NULL;
		std::atomic<int> _callback_id{ 100 };
		std::mutex _mutex;
		std::condition_variable _ready;
		unsigned int _commission_count = 0;
		unsigned int _margin_count = 0;
	};

	class BlockingQueueApiStub : public T26CtpApiStub
	{
	public:
		int ReqQryInstrument(CThostFtdcQryInstrumentField* field, int requestID) override
		{
			const int result = T26CtpApiStub::ReqQryInstrument(field, requestID);
			std::unique_lock<std::mutex> lock(_mutex);
			_instrument_entered = true;
			_ready.notify_all();
			_ready.wait(lock, [this]() { return _release_instrument; });
			return result;
		}

		int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField* field,
			int requestID) override
		{
			++_rate_request_count;
			return T26CtpApiStub::ReqQryInstrumentCommissionRate(field, requestID);
		}

		int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField* field,
			int requestID) override
		{
			++_rate_request_count;
			return T26CtpApiStub::ReqQryInstrumentMarginRate(field, requestID);
		}

		bool wait_until_instrument_entered(std::chrono::milliseconds timeout)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _ready.wait_for(lock, timeout, [this]() { return _instrument_entered; });
		}

		void release_instrument()
		{
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_release_instrument = true;
			}
			_ready.notify_all();
		}

		unsigned int rate_request_count() const { return _rate_request_count.load(); }

	private:
		std::mutex _mutex;
		std::condition_variable _ready;
		bool _instrument_entered = false;
		bool _release_instrument = false;
		std::atomic<unsigned int> _rate_request_count{ 0 };
	};
}

/* ------------------------------------------------------------------ */
/* 认证/登录链                                                          */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_flow, front_connected_builds_auth_request_from_config_globals)
{
	t26::setup_defaults("f1");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;

	BROKER_ID = "9999";
	INVESTOR_ID = "88888";
	APPID = "client_t26";
	AUTHCODE = "authcode_t26";

	{
		// ReqAuth 不落队列（TraderSpi.cpp L125-136 直发），worker 未起，spi 可安全析构
		CTraderSpi spi;
		t26::CoutCapture cap;
		spi.OnFrontConnected();
		const std::string out = cap.text();
		EXPECT_EQ(1u, stub.n_auth_);
		EXPECT_EQ(1, iRequestID);							// ++iRequestID 从 0 起步
		ASSERT_EQ(1u, stub.auth_reqids_.size());
		EXPECT_EQ(1, stub.auth_reqids_[0]);
		EXPECT_EQ("9999", std::string(stub.last_auth_.BrokerID));
		EXPECT_EQ("88888", std::string(stub.last_auth_.UserID));
		EXPECT_EQ("client_t26", std::string(stub.last_auth_.AppID));
		EXPECT_EQ("authcode_t26", std::string(stub.last_auth_.AuthCode));
		EXPECT_NE(std::string::npos, out.find("Requesting authentication: succeed")) << out;
	}
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, auth_success_last_triggers_user_login_with_password)
{
	t26::setup_defaults("f2");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;
	PASSWORD = "pwd123";

	CTraderSpi spi;
	t26::CoutCapture cap;
	spi.OnFrontConnected();								// 先走到 ReqAuthenticate
	auto ok = t26::ok_rsp();
	spi.OnRspAuthenticate(NULL, &ok, 1, true);			// TraderSpi.cpp L138-145

	EXPECT_EQ(1u, stub.n_auth_);
	EXPECT_EQ(1u, stub.n_login_);
	EXPECT_EQ(2, iRequestID);
	ASSERT_EQ(1u, stub.login_reqids_.size());
	EXPECT_EQ(2, stub.login_reqids_[0]);
	EXPECT_EQ("9999", std::string(stub.last_login_.BrokerID));
	EXPECT_EQ("88888", std::string(stub.last_login_.UserID));
	EXPECT_EQ("pwd123", std::string(stub.last_login_.Password));
	const std::string out = cap.text();
	EXPECT_NE(std::string::npos, out.find("Requesting user login: succeed")) << out;

	pUserApi = NULL;
}

TEST(t26_ctploader_flow, auth_non_last_or_error_never_reaches_login_request)
{
	t26::setup_defaults("f3");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;

	CTraderSpi spi;
	{
		auto ok = t26::ok_rsp();
		// bIsLast=false：整条守卫短路，连 ReqUserLogin 都不发（L141）
		spi.OnRspAuthenticate(NULL, &ok, 1, false);
		EXPECT_EQ(0u, stub.n_login_);
	}
	{
		auto err = t26::err_rsp(502, "bad auth");
		spi.OnRspAuthenticate(NULL, &err, 2, true);
		EXPECT_EQ(0u, stub.n_login_);
		EXPECT_EQ(0, iRequestID);							// 两条都被挡住 → 没有请求编号消耗
	}
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, login_success_writes_session_refs_and_queues_instrument_query)
{
	t26::setup_defaults("f4");
	T26CtpApiStub& stub = fresh_stub();
	stub.trading_day_ = "20251027";
	pUserApi = &stub;

	auto rsp = make_login_rsp(321, 654, "99");
	auto ok = t26::ok_rsp();

	CTraderSpi* spi = leaked_spi();						// 队列起了 worker：故意不析构
	// worker 的 stderr 必须走 fd 重定向（并发 rdbuf 换入换出会踩坏堆，见 common.hpp 注释）
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
	spi->OnRspUserLogin(&rsp, &ok, 3, true);

	// TraderSpi.cpp L162-171：会话三元组 + MaxOrderRef+1
	EXPECT_EQ(321, FRONT_ID);
	EXPECT_EQ(654, SESSION_ID);
	EXPECT_EQ("100", std::string(ORDER_REF));
	// INCREMENTAL=false → LoadFromJson 早退（L597-598），ReqQryInstrument 落队列（L175→L179-188）
	// worker 消费有 1010ms 限速窗口，需要轮询等待
	ASSERT_TRUE(t26::wait_for([&stub]() { return stub.n_qry_inst_ >= 1u; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(250));	// 等「queue left」噪声落进捕获缓冲再退场
	EXPECT_EQ(1, stub.qry_inst_reqids_[0]);				// 本用例没走 auth/login，请求编号从 1 起
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, login_non_last_response_keeps_session_untouched)
{
	t26::setup_defaults("f5");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;
	FRONT_ID = 777;
	SESSION_ID = 888;
	snprintf(ORDER_REF, sizeof(ORDER_REF), "%s", "untouched");

	auto rsp = make_login_rsp(321, 654, "99");
	auto ok = t26::ok_rsp();
	CTraderSpi spi;
	spi.OnRspUserLogin(&rsp, &ok, 4, false);				// L162：bIsLast=false 整段跳过

	EXPECT_EQ(777, FRONT_ID);
	EXPECT_EQ(888, SESSION_ID);
	EXPECT_EQ("untouched", std::string(ORDER_REF));
	EXPECT_EQ(0u, stub.n_qry_inst_);
	pUserApi = NULL;
}

/* ------------------------------------------------------------------ */
/* LoadFromJson（增量模式，OnRspUserLogin 里同步执行）                    */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_flow, incremental_login_reloads_commodities_and_contracts)
{
	t26::setup_defaults("f6");
	T26CtpApiStub& stub = fresh_stub();
	stub.trading_day_ = "20251027";
	pUserApi = &stub;
	INCREMENTAL = true;
	MAP_SESSION["SHFE.rb"] = "cfgsession";

	const std::string dir = SAVEPATH;
	{
		std::ofstream ofs(dir + "commodities.json");
		ofs << "{\"SHFE\":{\"rb\":{\"covermode\":1,\"pricemode\":1,\"category\":1,\"trademode\":0,"
			"\"precision\":0,\"pricetick\":1,\"volscale\":10,\"name\":\"螺纹钢\",\"exchg\":\"SHFE\","
			"\"session\":\"filesession\",\"holiday\":\"CHINA\"}}}";
	}
	{
		std::ofstream ofs(dir + "contracts.json");
		ofs << "{\"CZCE\":{\"SR2510\":{\"name\":\"白糖2510\",\"code\":\"SR2510\",\"altcode\":\"SR510\","
			"\"exchg\":\"CZCE\",\"product\":\"SR\",\"maxlimitqty\":500,\"maxmarketqty\":300,"
			"\"minlimitqty\":2,\"minmarketqty\":1,\"opendate\":20240513,\"expiredate\":20250930,"
			"\"longmarginratio\":0.08,\"shortmarginratio\":0.09,"
			"\"option\":{\"optiontype\":49,\"underlying\":\"SR510\",\"strikeprice\":5700,\"underlyingscale\":1}},"
			"\"rb2510\":{\"name\":\"螺纹2510\",\"code\":\"rb2510\",\"altcode\":\"rb2510\","
			"\"exchg\":\"CZCE\",\"product\":\"rb\",\"maxlimitqty\":100,\"maxmarketqty\":80,"
			"\"minlimitqty\":1,\"minmarketqty\":1,\"opendate\":20240513,\"expiredate\":20250930,"
			"\"longmarginratio\":0.08,\"shortmarginratio\":0.09}}}";
	}

	auto rsp = make_login_rsp(1, 2, "0");
	auto ok = t26::ok_rsp();
	CTraderSpi* spi = leaked_spi();
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");	// 同上：worker 噪声走 fd 重定向
#ifdef WT_T26_LOADJSON_TEST_HOOKS
	t26_test::load_json_test_hook::reset();
#endif
	spi->OnRspUserLogin(&rsp, &ok, 1, true);				// L173 → LoadFromJson（L595-682）
#ifdef WT_T26_LOADJSON_TEST_HOOKS
	EXPECT_EQ(2u, t26_test::load_json_test_hook::loads());		// commodities/contracts 各解析一次
	EXPECT_EQ(2u, t26_test::load_json_test_hook::releases());	// 两个解析结果各释放一次
	EXPECT_TRUE(t26_test::load_json_test_hook::every_loaded_variant_released_once());
#endif

	// —— 品种侧：字段逐项读回；session 只认 MAP_SESSION，文件里的 "session" 被忽略（L619）
	ASSERT_EQ(1u, _commodities.count("SHFE.rb"));
	const loader::Commodity& comm = _commodities["SHFE.rb"];
	EXPECT_EQ("rb", comm.m_strProduct);
	EXPECT_EQ("螺纹钢", comm.m_strName);
	EXPECT_EQ("SHFE", comm.m_strExchg);
	EXPECT_EQ("CNY", comm.m_strCurrency);
	EXPECT_EQ("cfgsession", comm.m_strSession);
	EXPECT_EQ(CM_CoverToday, comm.m_coverMode);
	EXPECT_EQ(PM_Limit, comm.m_priceMode);
	EXPECT_EQ(TM_Both, comm.m_tradeMode);
	EXPECT_EQ(CC_Future, comm.m_ccCategory);
	EXPECT_EQ(10u, comm.m_uVolScale);
	EXPECT_NEAR(1.0, comm.m_fPriceTick, 1e-9);
	EXPECT_EQ(0u, comm.m_uPrecision);

	// —— 合约侧：主体字段读回（key=文件里的合约代码，L643-651）
	ASSERT_EQ(1u, _contracts.count("SR2510"));
	const loader::Contract& cont = _contracts["SR2510"];
	EXPECT_EQ("SR2510", cont.m_strCode);
	EXPECT_EQ("CZCE", cont.m_strExchg);
	EXPECT_EQ("白糖2510", cont.m_strName);
	EXPECT_EQ("SR", cont.m_strProduct);
	EXPECT_EQ(300u, cont.m_maxMktQty);
	EXPECT_EQ(500u, cont.m_maxLmtQty);
	EXPECT_EQ(1u, cont.m_minMktQty);
	EXPECT_EQ(2u, cont.m_minLmtQty);
	EXPECT_EQ(20240513u, cont.m_uOpenDate);
	EXPECT_EQ(20250930u, cont.m_uExpireDate);
	EXPECT_NEAR(0.08, cont.m_dLongMarginRatio, 1e-9);
	EXPECT_NEAR(0.09, cont.m_dShortMarginRatio, 1e-9);

	// —— DumpToJson 的合同格式将期权信息嵌在 option 对象中；增量回读应保留每个字段。
	// fixture 的 optiontype=49 对应 WTSTypes.h 中 OT_Call='1'。
	EXPECT_EQ(OT_Call, cont.m_optType);
	EXPECT_EQ("SR510", cont.m_strUnderlying);
	EXPECT_NEAR(5700.0, cont.m_strikePrice, 1e-9);
	EXPECT_NEAR(1.0, cont.m_dUnderlyingScale, 1e-9);
	EXPECT_EQ("SR510", cont.m_strAltCode);

	// 普通合约没有 option 块时保留确定的非期权默认值，同时增量读取 altcode。
	ASSERT_EQ(1u, _contracts.count("rb2510"));
	const loader::Contract& future = _contracts["rb2510"];
	EXPECT_EQ("rb2510", future.m_strAltCode);
	EXPECT_EQ(OT_None, future.m_optType);
	EXPECT_TRUE(future.m_strUnderlying.empty());
	EXPECT_DOUBLE_EQ(0.0, future.m_strikePrice);
	EXPECT_DOUBLE_EQ(0.0, future.m_dUnderlyingScale);
	ASSERT_TRUE(t26::wait_for([&stub]() { return stub.n_qry_inst_ >= 1u; }));	// 让队列排空、worker 闭嘴
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, incremental_load_missing_contract_file_releases_only_loaded_commodity_root)
{
	t26::setup_defaults("f6_missing_contracts");
	INCREMENTAL = true;
	{
		std::ofstream ofs(SAVEPATH + COMM_FILE);
		ofs << "{\"SHFE\":{\"rb\":{\"name\":\"steel\",\"category\":1,\"volscale\":10,"
			"\"pricetick\":1.0,\"covermode\":1,\"pricemode\":1,\"trademode\":0,\"precision\":0}}}";
	}

#ifdef WT_T26_LOADJSON_TEST_HOOKS
	t26_test::load_json_test_hook::reset();
#endif
	CTraderSpi spi;
	CTraderSpiQueueTestAccess::load_from_json(spi);

	ASSERT_EQ(1u, _commodities.count("SHFE.rb"));
	EXPECT_TRUE(_contracts.empty());
#ifdef WT_T26_LOADJSON_TEST_HOOKS
	EXPECT_EQ(2u, t26_test::load_json_test_hook::loads());		// 第二次真实 load 返回 NULL
	EXPECT_EQ(1u, t26_test::load_json_test_hook::releases());	// 只释放成功解析的 commodity root
	EXPECT_TRUE(t26_test::load_json_test_hook::every_loaded_variant_released_once());
#endif
}

TEST(t26_ctploader_flow, incremental_load_invalid_contract_json_releases_only_loaded_commodity_root)
{
	t26::setup_defaults("f6_invalid_contracts");
	INCREMENTAL = true;
	{
		std::ofstream ofs(SAVEPATH + COMM_FILE);
		ofs << "{\"SHFE\":{\"rb\":{\"name\":\"steel\",\"category\":1,\"volscale\":10,"
			"\"pricetick\":1.0,\"covermode\":1,\"pricemode\":1,\"trademode\":0,\"precision\":0}}}";
	}
	{
		std::ofstream ofs(SAVEPATH + CONT_FILE);
		ofs << "{\"SHFE\":";
	}

#ifdef WT_T26_LOADJSON_TEST_HOOKS
	t26_test::load_json_test_hook::reset();
#endif
	CTraderSpi spi;
	CTraderSpiQueueTestAccess::load_from_json(spi);

	ASSERT_EQ(1u, _commodities.count("SHFE.rb"));
	EXPECT_TRUE(_contracts.empty());
#ifdef WT_T26_LOADJSON_TEST_HOOKS
	EXPECT_EQ(2u, t26_test::load_json_test_hook::loads());		// 坏 JSON 的 load 返回 NULL
	EXPECT_EQ(1u, t26_test::load_json_test_hook::releases());	// 只释放成功解析的 commodity root
	EXPECT_TRUE(t26_test::load_json_test_hook::every_loaded_variant_released_once());
#endif
}

/* ------------------------------------------------------------------ */
/* QRYFEES 链路：bIsLast → ReqQryCommission/ReqQryMargin → 费率回报主体  */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_flow, incremental_login_with_missing_files_still_reaches_query)
{
	t26::setup_defaults("f6b");
	T26CtpApiStub& stub = fresh_stub();
	stub.trading_day_ = "20251027";
	pUserApi = &stub;
	INCREMENTAL = true;						// 但 SAVEPATH 下没有 commodities/contracts 两个文件
	COMM_FILE = "no_such_comms.json";
	CONT_FILE = "no_such_conts.json";

	auto rsp = make_login_rsp(9, 8, "7");
	auto ok = t26::ok_rsp();
	CTraderSpi* spi = leaked_spi();
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
	spi->OnRspUserLogin(&rsp, &ok, 1, true);

	// L602-603 / L639-640：load_from_file 返回 NULL → 两段装载整体跳过，_commodities/_contracts 不动
	EXPECT_TRUE(_commodities.empty());
	EXPECT_TRUE(_contracts.empty());
	// 装载失败不拦登录链路的后半段：仍然发起 ReqQryInstrument（L175）
	ASSERT_TRUE(t26::wait_for([&stub]() { return stub.n_qry_inst_ >= 1u; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, last_instrument_response_with_fees_switch_queues_both_rate_requests)
{
	t26::setup_defaults("f7");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;
	QRYFEES = true;
	MAP_NAME["rb"] = "螺纹钢";

	CTraderSpi* spi = leaked_spi();
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
	seed_shfe_future(*spi, "rb2510", "rb");
	ASSERT_EQ(1u, _contracts.count("rb2510"));

	{
		auto ok = t26::ok_rsp();
		// L527-541：QRYFEES=true 的 bIsLast 不 exit，而是逐合约入列 commission+margin
		// （本次 AppendQuery 会创建 worker —— 所以外层从一开始就用 fd 重定向）
		spi->OnRspQryInstrument(NULL, &ok, 21, true);
	}

	// 队列限速 ~1010ms/条：两条都要等 worker 消费完
	ASSERT_TRUE(t26::wait_for([&stub]() {
		return !stub.comm_reqs_.empty() && !stub.margin_reqs_.empty();
	}));

	const CThostFtdcQryInstrumentCommissionRateField& cq = stub.comm_reqs_[0];
	EXPECT_EQ("9999", std::string(cq.BrokerID));
	EXPECT_EQ("88888", std::string(cq.InvestorID));
	EXPECT_EQ("rb2510", std::string(cq.InstrumentID));	// altcode 为空 → 用 m_strCode（L201-202）
	EXPECT_EQ("SHFE", std::string(cq.ExchangeID));

	const CThostFtdcQryInstrumentMarginRateField& mq = stub.margin_reqs_[0];
	EXPECT_EQ("9999", std::string(mq.BrokerID));
	EXPECT_EQ("88888", std::string(mq.InvestorID));
	EXPECT_EQ("rb2510", std::string(mq.InstrumentID));
	EXPECT_EQ("SHFE", std::string(mq.ExchangeID));
	EXPECT_EQ(THOST_FTDC_HF_Speculation, mq.HedgeFlag);	// L223：投机套保标志恒为投机
	std::this_thread::sleep_for(std::chrono::milliseconds(250));	// 最后一条「queue left」落进捕获缓冲
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, rate_requests_use_loaded_altcode_when_present)
{
	t26::setup_defaults("f7alt");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;
	QRYFEES = true;
	MAP_NAME["rb"] = "螺纹钢";

	CTraderSpi* spi = leaked_spi();
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
	seed_shfe_future(*spi, "rb2510", "rb");
	_contracts["rb2510"].m_strAltCode = "rb510";

	auto ok = t26::ok_rsp();
	spi->OnRspQryInstrument(NULL, &ok, 22, true);
	ASSERT_TRUE(t26::wait_for([&stub]() {
		return !stub.comm_reqs_.empty() && !stub.margin_reqs_.empty();
	}));

	ASSERT_EQ(1u, stub.comm_reqs_.size());
	ASSERT_EQ(1u, stub.margin_reqs_.size());
	EXPECT_EQ("rb510", std::string(stub.comm_reqs_[0].InstrumentID));
	EXPECT_EQ("rb510", std::string(stub.margin_reqs_[0].InstrumentID));
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, fee_filter_only_queues_contracts_matching_code_or_product)
{
	t26::setup_defaults("f8");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;
	QRYFEES = true;
	MAP_NAME["rb"] = "螺纹钢";
	MAP_NAME["au"] = "黄金";
	SET_FILTERS.insert("au");

	CTraderSpi* spi = leaked_spi();
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
	seed_shfe_future(*spi, "rb2510", "rb");
	seed_shfe_future(*spi, "au2512", "au");
	ASSERT_EQ(2u, _contracts.size());

	auto ok = t26::ok_rsp();
	spi->OnRspQryInstrument(NULL, &ok, 22, true);

	// au 的两条请求先后被限速隔开；rb 被 L534-536 过滤掉
	ASSERT_TRUE(t26::wait_for([&stub]() { return !stub.margin_reqs_.empty(); }));
	// 若过滤失效，rb 的两条会在再下一个 1010ms 窗口出现；多等一拍把它证伪
	std::this_thread::sleep_for(std::chrono::milliseconds(1400));
	EXPECT_EQ(1u, stub.comm_reqs_.size());
	EXPECT_EQ(1u, stub.margin_reqs_.size());
	if (!stub.comm_reqs_.empty())
		EXPECT_EQ("au2512", std::string(stub.comm_reqs_[0].InstrumentID));
	if (!stub.margin_reqs_.empty())
		EXPECT_EQ("au2512", std::string(stub.margin_reqs_[0].InstrumentID));
	pUserApi = NULL;
}

TEST(t26_ctploader_flow, commission_response_switches_between_volume_and_money_modes)
{
	t26::setup_defaults("f9");
	// QRYFEES=false：回报主体只填 _fees，不会走 L570-575 的 exit 出口
	QRYFEES = false;
	CTraderSpi spi;

	{
		CThostFtdcInstrumentCommissionRateField f;
		std::memset(&f, 0, sizeof(f));
		t26::set_fixed(f.InstrumentID, sizeof(f.InstrumentID), "rb2510");
		f.OpenRatioByMoney = 0.0;
		f.OpenRatioByVolume = 3.0;
		f.CloseRatioByVolume = 4.0;
		f.CloseTodayRatioByVolume = 5.0;
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrumentCommissionRate(&f, &ok, 30, true);
	}
	ASSERT_EQ(1u, _fees.count("rb2510"));
	EXPECT_TRUE(_fees["rb2510"]._byvol);
	EXPECT_NEAR(3.0, _fees["rb2510"]._open, 1e-9);
	EXPECT_NEAR(4.0, _fees["rb2510"]._close, 1e-9);
	EXPECT_NEAR(5.0, _fees["rb2510"]._closet, 1e-9);

	{
		// 0.0001 > decimal::EPSINON(1e-6) → 按额模式（L555-567）
		CThostFtdcInstrumentCommissionRateField f;
		std::memset(&f, 0, sizeof(f));
		t26::set_fixed(f.InstrumentID, sizeof(f.InstrumentID), "ao2512");
		f.OpenRatioByMoney = 0.0001;
		f.CloseRatioByMoney = 0.0002;
		f.CloseTodayRatioByMoney = 0.0003;
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrumentCommissionRate(&f, &ok, 31, true);
	}
	ASSERT_EQ(1u, _fees.count("ao2512"));
	EXPECT_FALSE(_fees["ao2512"]._byvol);
	EXPECT_NEAR(0.0001, _fees["ao2512"]._open, 1e-9);
	EXPECT_NEAR(0.0002, _fees["ao2512"]._close, 1e-9);
	EXPECT_NEAR(0.0003, _fees["ao2512"]._closet, 1e-9);

	{
		// 5e-7 < 1e-6：按额非零但仍被 decimal::eq 判 0 → 取「按量」值（eps 语义钉子）
		CThostFtdcInstrumentCommissionRateField f;
		std::memset(&f, 0, sizeof(f));
		t26::set_fixed(f.InstrumentID, sizeof(f.InstrumentID), "m2509");
		f.OpenRatioByMoney = 5e-7;
		f.OpenRatioByVolume = 9.0;
		f.CloseRatioByVolume = 8.0;
		f.CloseTodayRatioByVolume = 7.0;
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrumentCommissionRate(&f, &ok, 32, true);
	}
	ASSERT_EQ(1u, _fees.count("m2509"));
	EXPECT_TRUE(_fees["m2509"]._byvol);
	EXPECT_NEAR(9.0, _fees["m2509"]._open, 1e-9);
}

TEST(t26_ctploader_flow, margin_response_records_long_short_money_ratios)
{
	t26::setup_defaults("f10");
	QRYFEES = false;
	CTraderSpi spi;

	CThostFtdcInstrumentMarginRateField f;
	std::memset(&f, 0, sizeof(f));
	t26::set_fixed(f.InstrumentID, sizeof(f.InstrumentID), "rb2510");
	f.LongMarginRatioByMoney = 0.07;
	f.ShortMarginRatioByMoney = 0.09;
	auto ok = t26::ok_rsp();
	spi.OnRspQryInstrumentMarginRate(&f, &ok, 40, true);

	ASSERT_EQ(1u, _margins.count("rb2510"));
	EXPECT_NEAR(0.07, _margins["rb2510"]._long, 1e-9);
	EXPECT_NEAR(0.09, _margins["rb2510"]._short, 1e-9);
}

TEST(t26_ctploader_flow, gbk_error_message_is_transcoded_to_utf8_before_report)
{
	t26::setup_defaults("f11");
	// encode_text（TraderSpi.cpp L27-36）非 MSVC 下恒走 ChartoUTF8；t6 只测了纯 ASCII 文本
	auto err = t26::err_rsp(33);
	const std::string gbk = "\xd6\xd0\xce\xc4";					// 「中文」的 GBK 编码
	t26::set_fixed(err.ErrorMsg, sizeof(err.ErrorMsg), gbk);

	CTraderSpi spi;
	t26::CoutCapture cap;
	spi.OnRspError(&err, 50, true);
	const std::string out = cap.text();
	EXPECT_NE(std::string::npos, out.find("ErrorID=33")) << out;
	const std::string utf8 = "\xe4\xb8\xad\xe6\x96\x87";		// 「中文」UTF-8
	EXPECT_NE(std::string::npos, out.find(utf8)) << out;
	EXPECT_EQ(std::string::npos, out.find(gbk)) << out;
}

TEST(t26_ctploader_flow, concurrent_callbacks_reenter_queue_and_worker_stops_after_drain)
{
	// Keep a regression for the former unbounded join in a child so a future
	// worker-stop regression cannot hang the complete test process.
	fflush(NULL);
	pid_t pid = fork();
	ASSERT_GE(pid, 0) << "fork failed";
	if (pid == 0)
	{
		t26::setup_defaults("f_queue_sync");
		ReentrantQueueApiStub stub;
		pUserApi = &stub;
		QRYFEES = true;
		MAP_NAME["rb"] = "螺纹钢";

		t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
		std::unique_ptr<CTraderSpi> spi(new CTraderSpi());
		stub.set_spi(spi.get());
		seed_shfe_future(*spi, "rb2510", "rb");

		auto rsp = make_login_rsp(3, 4, "5");
		auto ok = t26::ok_rsp();
		spi->OnRspUserLogin(&rsp, &ok, 1, true);

		if (!stub.wait_for_all_rate_requests(std::chrono::seconds(12)))
			_exit(2);
		// Let the final API task return and the worker reach its empty-queue wait
		// before destroying the SPI, exercising the idle stop/join path.
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

		const auto join_started = std::chrono::steady_clock::now();
		spi.reset();
		const auto join_elapsed = std::chrono::steady_clock::now() - join_started;
		pUserApi = NULL;
		_exit(join_elapsed < std::chrono::seconds(2) ? 0 : 3);
	}

	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(16);
	for (;;)
	{
		pid_t done = waitpid(pid, &status, WNOHANG);
		if (done == pid)
			break;
		if (done < 0)
		{
			FAIL() << "waitpid failed";
			return;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			FAIL() << "worker did not drain and join within 16 seconds";
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally, status=" << status;
	EXPECT_EQ(0, WEXITSTATUS(status))
		<< "child return code: 2=missing API requests, 3=slow stop/join";
}

TEST(t26_ctploader_flow, stopping_worker_drops_pending_queries_and_joins_active_call)
{
	fflush(NULL);
	pid_t pid = fork();
	ASSERT_GE(pid, 0) << "fork failed";
	if (pid == 0)
	{
		t26::setup_defaults("f_queue_stop_pending");
		BlockingQueueApiStub stub;
		pUserApi = &stub;
		QRYFEES = true;
		MAP_NAME["rb"] = "螺纹钢";

		t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
		std::unique_ptr<CTraderSpi> spi(new CTraderSpi());
		seed_shfe_future(*spi, "rb2510", "rb");

		auto rsp = make_login_rsp(3, 4, "5");
		auto ok = t26::ok_rsp();
		spi->OnRspUserLogin(&rsp, &ok, 1, true);
		if (!stub.wait_until_instrument_entered(std::chrono::seconds(2)))
			_exit(2);

		// Queue fee requests behind a deliberately blocked instrument request.
		spi->OnRspQryInstrument(NULL, &ok, 41, true);
		CTraderSpi* spi_raw = spi.get();
		std::mutex join_mutex;
		std::condition_variable join_ready;
		bool joined = false;
		std::thread destroyer([&]() {
			spi.reset();
			{
				std::lock_guard<std::mutex> lock(join_mutex);
				joined = true;
			}
			join_ready.notify_all();
		});

		if (!CTraderSpiQueueTestAccess::wait_until_stopped(*spi_raw, std::chrono::seconds(2)))
			_exit(3);
		stub.release_instrument();

		std::unique_lock<std::mutex> lock(join_mutex);
		const bool completed = join_ready.wait_for(lock, std::chrono::seconds(1), [&]() { return joined; });
		lock.unlock();
		if (!completed)
			_exit(4);
		destroyer.join();
		pUserApi = NULL;
		_exit(stub.rate_request_count() == 0 ? 0 : 5);
	}

	int status = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	for (;;)
	{
		pid_t done = waitpid(pid, &status, WNOHANG);
		if (done == pid)
			break;
		if (done < 0)
		{
			FAIL() << "waitpid failed";
			return;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			FAIL() << "pending-query stop/join did not finish within 6 seconds";
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally, status=" << status;
	EXPECT_EQ(0, WEXITSTATUS(status))
		<< "child return code: 2=instrument API not entered, 3=stop not observed, "
		<< "4=join waited for pending requests, 5=pending request was sent";
}

/* ------------------------------------------------------------------ */
/* 明确不能启用的越界路径（只登记）                                       */
/* ------------------------------------------------------------------ */

// BUG-T26-002：没有月份数字的 CZCE 代码应安全拒绝，不得扫描过字符串结尾。
TEST(t26_ctploader_flow, fill_czce_code_on_letters_only_returns_empty)
{
	t26::setup_defaults("f13");
	std::string expanded;
	EXPECT_FALSE(fillCzceCode(std::string_view("SRCS", sizeof("SRCS")), 20251027, expanded));
	EXPECT_TRUE(expanded.empty());
}
