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
 * 线程约束（实测读码，不是转述）：AppendQuery（L230-273）会起一个 boost worker；
 * worker 的内层 `while (_queries.empty()) sleep(10ms)` **不回看 _stopped**（L243-246），
 * 队列排空后析构 spi 会 join 挂死 → 凡是驱动过队列的用例都把 spi 放在堆上「不删」。
 * 门禁按一条用例一个进程跑，泄漏只活到该用例进程结束。
 */
#include "t26_ctploader_common.hpp"
#include "t26_ctp_api_stub.hpp"

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
			"\"option\":{\"optiontype\":49,\"underlying\":\"SR510\",\"strikeprice\":5700,\"underlyingscale\":1}}}}";
	}

	auto rsp = make_login_rsp(1, 2, "0");
	auto ok = t26::ok_rsp();
	CTraderSpi* spi = leaked_spi();
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");	// 同上：worker 噪声走 fd 重定向
	spi->OnRspUserLogin(&rsp, &ok, 1, true);				// L173 → LoadFromJson（L595-682）

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

	// —— 期权块按「嵌套写、平铺读」的现实形状回读 → 全部读丢（BUG-T26-004/005 的行为钉子）
	EXPECT_EQ(OT_None, cont.m_optType);					// 顶层没有 optiontype → 0
	EXPECT_TRUE(cont.m_strUnderlying.empty());			// underlying 嵌在 option 里 → 读到 ""
	EXPECT_NEAR(0.0, cont.m_strikePrice, 1e-9);			// strike=getDouble("product")=strtod("SR")=0（L666）
	EXPECT_NEAR(0.0, cont.m_dUnderlyingScale, 1e-9);
	// altcode 在装载里根本没有对应的读取语句 → 平铺 json 也丢（L651-655 无 m_strAltCode 赋值）
	EXPECT_TRUE(cont.m_strAltCode.empty());
	ASSERT_TRUE(t26::wait_for([&stub]() { return stub.n_qry_inst_ >= 1u; }));	// 让队列排空、worker 闭嘴
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	pUserApi = NULL;
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

/* ------------------------------------------------------------------ */
/* 明确不能启用的路径（挂死/越界，只登记）                                 */
/* ------------------------------------------------------------------ */

// AppendQuery 的 worker 在队列排空后卡死在 `while(_queries.empty())` 内层循环（L243-246），
// 该循环不回看 _stopped → ~CTraderSpi 的 join（TraderSpi.h L24-29）永不返回。
// 启用必 HANG(60s)，所以只登记；BUG-T26-001。
TEST(DISABLED_t26_ctploader_flow_hang, destructor_joins_forever_after_queue_drained)
{
	t26::setup_defaults("f12");
	T26CtpApiStub& stub = fresh_stub();
	pUserApi = &stub;

	auto rsp = make_login_rsp(3, 4, "5");
	auto ok = t26::ok_rsp();
	CTraderSpi* spi = new CTraderSpi();
	spi->OnRspUserLogin(&rsp, &ok, 1, true);
	t26::wait_for([&stub]() { return stub.n_qry_inst_ >= 1u; });
	delete spi;											// ← 挂死点：join 永不返回
}

// fillCzceCode 的 `for(;;) if (isdigit(code[pid_len++])) break;`（L318-324）不检查 '\0'：
// 纯字母代码会一路越界读，直至读到随机数字或段错误。崩溃形态不可证伪，只登记；BUG-T26-002。
TEST(DISABLED_t26_ctploader_flow_crash, fill_czce_code_on_letters_only_scans_past_terminator)
{
	t26::setup_defaults("f13");
	volatile size_t n = fillCzceCode("SRCS", 20251027).size();
	(void)n;
	SUCCEED();
}
