/*
 * lane R26 — OnRspQryInstrument 合约/品种映射主体用例
 *
 * 覆盖 TraderSpi.cpp L343-548 的映射主体（t6 lane 只摸到了守卫层）。
 * 全部用 bIsLast=false 调用（bIsLast=true 且 !QRYFEES 会以 exit(0) 收尾，见
 * t26_ctploader_dump_test.cpp 的 fork 处理）；错误应答/NULL 载荷两条守卫 t6 已有
 * 同形用例，本文件不复述。
 *
 * CZCE 两条（i11/i12）需要 _trading_day：该成员无构造初始化（TraderSpi.h L76），
 * 只由 OnRspUserLogin L171 写入，所以这两条先走「替身 API 登录」再喂合约，
 * spi 放堆上不析构（worker 挂死约束见 flow 文件头）。fillCzceCode 的 static
 * yearFlag 在**首次调用**时按传入 tdate 锁存（L310-315），一条用例一个进程正好隔离。
 */
#include "t26_ctploader_common.hpp"
#include "t26_ctp_api_stub.hpp"

namespace
{
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

	// 登录一次把 _trading_day 写成 atoi(trading_day_)；返回不析构的 spi（worker 已起）
	// g_login_stub 让用例收尾时能确认队列已排空（worker 停止打 stderr 后才允许退场）
	T26CtpApiStub* g_login_stub = NULL;

	CTraderSpi* login_for_trading_day(const std::string& day)
	{
		T26CtpApiStub& stub = fresh_stub();
		g_login_stub = &stub;
		stub.trading_day_ = day;
		pUserApi = &stub;
		auto rsp = make_login_rsp(11, 22, "0");
		auto ok = t26::ok_rsp();
		CTraderSpi* spi = new CTraderSpi();
		spi->OnRspUserLogin(&rsp, &ok, 1, true);
		return spi;
	}

	bool drain_login_queue()
	{
		if (g_login_stub == NULL)
			return true;
		bool done = t26::wait_for([]() { return g_login_stub->n_qry_inst_ >= 1u; });
		std::this_thread::sleep_for(std::chrono::milliseconds(250));	// 等「queue left」打完
		return done;
	}

	// 喂一条合约并返回给断言用的局部拷贝入口（bIsLast=false）
	void feed(CTraderSpi& spi, CThostFtdcInstrumentField& f)
	{
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrument(&f, &ok, 7, false);
	}
}

/* ------------------------------------------------------------------ */
/* CLASSMASK 准入                                                       */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_instrument, classmask_gates_future_option_and_other_classes)
{
	t26::setup_defaults("i1");
	CTraderSpi spi;

	// CLASSMASK=0：期货也不准入（L360-364 两个条件都 false → do-while 空转）
	CLASSMASK = 0;
	{
		auto f = t26::make_instrument("SHFE", "rb2510", "rb", THOST_FTDC_PC_Futures);
		feed(spi, f);
	}
	EXPECT_TRUE(_contracts.empty());
	EXPECT_TRUE(_commodities.empty());

	// CLASSMASK=1 只放期货：期权被拒
	CLASSMASK = 1;
	{
		auto f = t26::make_instrument("DCE", "mo2509-C-3000", "mo", THOST_FTDC_PC_Options);
		feed(spi, f);
	}
	EXPECT_TRUE(_contracts.empty());

	// CLASSMASK=2 只放期权：期货被拒
	CLASSMASK = 2;
	{
		auto f = t26::make_instrument("SHFE", "cu2512", "cu", THOST_FTDC_PC_Futures);
		feed(spi, f);
		EXPECT_TRUE(_contracts.empty());

		// 即期/组合/EFP 任何 mask 都不放（isGranted 只认期货/期权两个位，L357-364）
		auto spot = t26::make_instrument("BSE", "spot1", "sp1", THOST_FTDC_PC_Spot);
		feed(spi, spot);
		auto comb = t26::make_instrument("CZCE", "comb1", "cb1", THOST_FTDC_PC_Combination);
		feed(spi, comb);
		auto efp = t26::make_instrument("SHFE", "efp1", "ef1", THOST_FTDC_PC_EFP);
		feed(spi, efp);
		EXPECT_TRUE(_contracts.empty());
		EXPECT_TRUE(_commodities.empty());
	}

	// mask=2 时期权准入（正向对照，证明上面的拒绝不是「全拒」）
	{
		MAP_NAME["mo"] = "豆粕期权";
		auto f = t26::make_instrument("DCE", "mo2509-C-3000", "mo", THOST_FTDC_PC_Options);
		feed(spi, f);
		EXPECT_EQ(1u, _contracts.count("mo2509-C-3000"));
	}
}

/* ------------------------------------------------------------------ */
/* 品种名命中（MAP_NAME）主体映射                                        */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_instrument, shfe_future_maps_contract_and_commodity_fields)
{
	t26::setup_defaults("i2");
	CLASSMASK = 1;
	MAP_NAME["rb"] = "螺纹钢";				// UTF-8（run() 装载时统一转成 UTF8，夹具同形）
	MAP_SESSION["SHFE.rb"] = "daynight";

	CTraderSpi spi;
	auto f = t26::make_instrument("SHFE", "rb2510", "rb", THOST_FTDC_PC_Futures);
	feed(spi, f);

	// —— 合约（L387-452）
	ASSERT_EQ(1u, _contracts.count("rb2510"));
	const loader::Contract& c = _contracts["rb2510"];
	EXPECT_EQ("rb2510", c.m_strCode);						// 非 CZCE → 原样（L428）
	EXPECT_TRUE(c.m_strAltCode.empty());					// 非 CZCE 不写 altcode
	EXPECT_EQ("SHFE", c.m_strExchg);
	EXPECT_EQ("螺纹钢2510", c.m_strName);					// pname + InstrumentID.substr(strlen(ProductID))（L394-399）
	EXPECT_EQ("rb", c.m_strProduct);
	EXPECT_EQ(500u, c.m_maxMktQty);
	EXPECT_EQ(1u, c.m_minMktQty);
	EXPECT_EQ(1000u, c.m_maxLmtQty);
	EXPECT_EQ(2u, c.m_minLmtQty);
	EXPECT_EQ(OT_None, c.m_optType);
	EXPECT_TRUE(c.m_strUnderlying.empty());
	EXPECT_NEAR(0.0, c.m_strikePrice, 1e-9);
	EXPECT_NEAR(0.0, c.m_dUnderlyingScale, 1e-9);
	EXPECT_EQ(20170101u, c.m_uOpenDate);					// convert::to_uint32(L448)
	EXPECT_EQ(20251015u, c.m_uExpireDate);
	EXPECT_NEAR(0.1, c.m_dLongMarginRatio, 1e-9);
	EXPECT_NEAR(0.12, c.m_dShortMarginRatio, 1e-9);

	// —— 品种（L454-517）：SHFE/INE → 平今 + 只限价（L485-501）
	ASSERT_EQ(1u, _commodities.count("SHFE.rb"));
	const loader::Commodity& m = _commodities["SHFE.rb"];
	EXPECT_EQ("rb", m.m_strProduct);
	EXPECT_EQ("螺纹钢", m.m_strName);
	EXPECT_EQ("SHFE", m.m_strExchg);
	EXPECT_EQ("CNY", m.m_strCurrency);
	EXPECT_EQ("daynight", m.m_strSession);
	EXPECT_EQ(CM_CoverToday, m.m_coverMode);
	EXPECT_EQ(PM_Limit, m.m_priceMode);
	EXPECT_EQ(TM_Both, m.m_tradeMode);
	EXPECT_EQ(CC_Future, m.m_ccCategory);
	EXPECT_EQ(10u, m.m_uVolScale);
	EXPECT_NEAR(1.0, m.m_fPriceTick, 1e-9);
	EXPECT_EQ(0u, m.m_uPrecision);							// PriceTick==1.0 → <1 不成立 → 0（L512-515）
}

TEST(t26_ctploader_instrument, ine_takes_shfe_branch_and_missing_session_warns)
{
	t26::setup_defaults("i3");
	CLASSMASK = 1;
	MAP_NAME["au"] = "黄金";
	MAP_NAME["sc"] = "原油";

	CTraderSpi spi;
	{
		auto f = t26::make_instrument("INE", "sc2509", "sc", THOST_FTDC_PC_Futures);
		feed(spi, f);
	}
	{
		t26::CoutCapture cap;
		auto f = t26::make_instrument("DCE", "au2512", "au", THOST_FTDC_PC_Futures);
		feed(spi, f);
		// L469-471：MAP_SESSION 没配 → 品种行照建、session 留空、并打 warning
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("No session configured for DCE.au")) << out;
	}
	// INE 与 SHFE 同侧（平今+限价），其余交易所维持 开平+双模式（L485-503）
	ASSERT_EQ(1u, _commodities.count("INE.sc"));
	EXPECT_EQ(CM_CoverToday, _commodities["INE.sc"].m_coverMode);
	EXPECT_EQ(PM_Limit, _commodities["INE.sc"].m_priceMode);
	ASSERT_EQ(1u, _commodities.count("DCE.au"));
	EXPECT_EQ(CM_OpenCover, _commodities["DCE.au"].m_coverMode);
	EXPECT_EQ(PM_Both, _commodities["DCE.au"].m_priceMode);
	EXPECT_EQ("", _commodities["DCE.au"].m_strSession);		// MAP_SESSION 缺失 → 空串（L469-471）
}

TEST(t26_ctploader_instrument, price_tick_precision_ladder_and_zero_volscale_fallback)
{
	t26::setup_defaults("i4");
	CLASSMASK = 1;
	// 五个品种各挂一个 tick，命中 L506-515 的 5 个档位；VolumeMultiple=0 → volscale 兜底为 1（L474）
	struct rung { const char* pid; const char* inst; double tick; uint32_t prec; };
	const rung rungs[] = {
		{"ba", "ba2509", 1.0,   0u},
		{"bb", "bb2509", 0.5,   1u},
		{"bc", "bc2509", 0.05,  2u},
		{"bd", "bd2509", 0.005, 3u},
		{"be", "be2509", 0.0005, 4u},
	};
	CTraderSpi spi;
	for (const auto& r : rungs)
	{
		MAP_NAME[r.pid] = r.pid;
		auto f = t26::make_instrument("DCE", r.inst, r.pid, THOST_FTDC_PC_Futures);
		f.PriceTick = r.tick;
		f.VolumeMultiple = (std::string(r.pid) == "ba") ? 0 : 5;
		feed(spi, f);
	}
	for (const auto& r : rungs)
	{
		const std::string key = std::string("DCE.") + r.pid;
		ASSERT_EQ(1u, _commodities.count(key)) << key;
		EXPECT_EQ(r.prec, _commodities[key].m_uPrecision) << key;
		EXPECT_EQ(std::string("ba") == r.pid ? 1u : 5u, _commodities[key].m_uVolScale) << key;
	}
}

TEST(t26_ctploader_instrument, product_name_falls_back_to_exchange_scoped_key)
{
	t26::setup_defaults("i5");
	CLASSMASK = 1;
	MAP_NAME["DCE.i"] = "铁矿石";			// 只给「交易所.品种」形（L370-376 的兜底键）

	CTraderSpi spi;
	auto f = t26::make_instrument("DCE", "i2509", "i", THOST_FTDC_PC_Futures);
	feed(spi, f);

	ASSERT_EQ(1u, _contracts.count("i2509"));
	EXPECT_EQ("铁矿石2509", _contracts["i2509"].m_strName);
	ASSERT_EQ(1u, _commodities.count("DCE.i"));
	EXPECT_EQ("铁矿石", _commodities["DCE.i"].m_strName);
}

TEST(t26_ctploader_instrument, commodity_row_created_once_per_exchange_product)
{
	t26::setup_defaults("i6");
	CLASSMASK = 1;
	MAP_NAME["rb"] = "螺纹钢";

	CTraderSpi spi;
	{
		auto f = t26::make_instrument("SHFE", "rb2510", "rb", THOST_FTDC_PC_Futures);
		feed(spi, f);
	}
	{
		auto f = t26::make_instrument("SHFE", "rb2601", "rb", THOST_FTDC_PC_Futures);
		f.PriceTick = 0.5;					// 同品种第二条不得改写已存在的品种行（L455-457 find 命中即跳过）
		feed(spi, f);
	}
	EXPECT_EQ(2u, _contracts.size());
	ASSERT_EQ(1u, _commodities.count("SHFE.rb"));
	EXPECT_EQ(0u, _commodities["SHFE.rb"].m_uPrecision);	// 若被第二条改写会变成 1
	EXPECT_NEAR(1.0, _commodities["SHFE.rb"].m_fPriceTick, 1e-9);
	EXPECT_EQ("螺纹钢2601", _contracts["rb2601"].m_strName);
}

/* ------------------------------------------------------------------ */
/* ONLYINCFG 与自动定名                                                  */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_instrument, onlyincfg_drops_unknown_product_and_logs_instrument)
{
	t26::setup_defaults("i7");
	CLASSMASK = 1;
	ONLYINCFG = true;

	CTraderSpi spi;
	{
		t26::CoutCapture cap;
		auto f = t26::make_instrument("GFEX", "si512", "si", THOST_FTDC_PC_Futures);
		feed(spi, f);
		const std::string out = cap.text();
		// L381-385：break 出 do-while，既不进 _contracts 也不进 _commodities，且打印该行证伪
		EXPECT_NE(std::string::npos, out.find("OnRspQryInstrument: GFEX.si512")) << out;
	}
	EXPECT_TRUE(_contracts.empty());
	EXPECT_TRUE(_commodities.empty());

	{
		// ONLYINCFG=true 不拦「映射里有名分」的品种（走 L392-412 正常分支）
		MAP_NAME["si"] = "工业硅";
		auto f = t26::make_instrument("GFEX", "si512", "si", THOST_FTDC_PC_Futures);
		feed(spi, f);
	}
	ASSERT_EQ(1u, _contracts.count("si512"));
	EXPECT_EQ("工业硅512", _contracts["si512"].m_strName);
}

TEST(t26_ctploader_instrument, autoname_future_strips_month_and_transcodes_gbk_name)
{
	t26::setup_defaults("i8");
	CLASSMASK = 1;
	ONLYINCFG = false;
	// MAP_NAME 无 "sp" → L387-390：cname=InstrumentName，pname=extractProductName(InstrumentName)
	// InstrumentName 给 GBK「中文2512」：cname 过 ChartoUTF8（L417-418），品种名同理（L460-461）

	CTraderSpi spi;
	auto f = t26::make_instrument("SHFE", "sp2512", "sp", THOST_FTDC_PC_Futures);
	t26::set_fixed(f.InstrumentName, sizeof(f.InstrumentName), "\xd6\xd0\xce\xc4" "2512");
	feed(spi, f);

	const std::string utf8_zhongwen = "\xe4\xb8\xad\xe6\x96\x87";	// 「中文」UTF-8
	ASSERT_EQ(1u, _contracts.count("sp2512"));
	EXPECT_EQ(utf8_zhongwen + "2512", _contracts["sp2512"].m_strName);
	ASSERT_EQ(1u, _commodities.count("SHFE.sp"));
	EXPECT_EQ(utf8_zhongwen, _commodities["SHFE.sp"].m_strName);		// extractProductName 掉尾数字后转码
}

TEST(t26_ctploader_instrument, autoname_option_keeps_full_instrument_name)
{
	t26::setup_defaults("i9");
	CLASSMASK = 3;
	ONLYINCFG = false;
	// 期权无名分 → L388：pname=InstrumentName 全名（不做 extractProductName 截尾）

	CTraderSpi spi;
	auto f = t26::make_instrument("DCE", "m2509-C-3000", "mo", THOST_FTDC_PC_Options);
	t26::set_fixed(f.InstrumentName, sizeof(f.InstrumentName), "豆粕期权2509");
	f.OptionsType = '1';
	f.StrikePrice = 3000.0;
	t26::set_fixed(f.UnderlyingInstrID, sizeof(f.UnderlyingInstrID), "m2509");
	feed(spi, f);

	ASSERT_EQ(1u, _contracts.count("m2509-C-3000"));
	const loader::Contract& c = _contracts["m2509-C-3000"];
	EXPECT_EQ("豆粕期权2509", c.m_strName);
	EXPECT_EQ(OT_Call, c.m_optType);									// (OptionType)'1'（L440）
	EXPECT_EQ("m2509", c.m_strUnderlying);								// 非 CZCE 原样（L444）
	EXPECT_NEAR(3000.0, c.m_strikePrice, 1e-9);
	ASSERT_EQ(1u, _commodities.count("DCE.mo"));
	EXPECT_EQ("豆粕期权2509", _commodities["DCE.mo"].m_strName);
	EXPECT_EQ(CC_FutOption, _commodities["DCE.mo"].m_ccCategory);
}

TEST(t26_ctploader_instrument, spot_option_maps_to_spot_option_category)
{
	t26::setup_defaults("i10");
	CLASSMASK = 2;
	MAP_NAME["IO"] = "股指期权";

	CTraderSpi spi;
	auto f = t26::make_instrument("CFFEX", "IO2510-C-4000", "IO", THOST_FTDC_PC_SpotOption);
	// bOption + 有名分 → L400-406：underlyPID=extractProductID("IF2510")="IF"，
	// month=InstrumentID.substr(2)="2510-C-4000" → cname="股指期权2510-C-4000"（拼接语义钉子）
	t26::set_fixed(f.UnderlyingInstrID, sizeof(f.UnderlyingInstrID), "IF2510");
	f.OptionsType = '2';
	f.UnderlyingMultiple = 0.5;
	feed(spi, f);

	ASSERT_EQ(1u, _contracts.count("IO2510-C-4000"));
	const loader::Contract& c = _contracts["IO2510-C-4000"];
	EXPECT_EQ("股指期权2510-C-4000", c.m_strName);
	EXPECT_EQ(OT_Put, c.m_optType);
	EXPECT_EQ("IF2510", c.m_strUnderlying);
	EXPECT_NEAR(0.5, c.m_dUnderlyingScale, 1e-9);
	ASSERT_EQ(1u, _commodities.count("CFFEX.IO"));
	EXPECT_EQ(CC_SpotOption, _commodities["CFFEX.IO"].m_ccCategory);
	EXPECT_EQ(CM_OpenCover, _commodities["CFFEX.IO"].m_coverMode);		// CFFEX 非 SHFE/INE
}

TEST(t26_ctploader_instrument, duplicate_instrument_is_skipped_with_log)
{
	t26::setup_defaults("i11");
	CLASSMASK = 1;
	MAP_NAME["rb"] = "螺纹钢";

	CTraderSpi spi;
	{
		auto f = t26::make_instrument("SHFE", "rb2510", "rb", THOST_FTDC_PC_Futures);
		feed(spi, f);
	}
	size_t before = _contracts.size();
	{
		t26::CoutCapture cap;
		auto f = t26::make_instrument("SHFE", "rb2510", "rb", THOST_FTDC_PC_Futures);
		f.MaxMarketOrderVolume = 888;						// 若「跳过」失效，这里会被改写
		feed(spi, f);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("SHFE.rb2510 already exists, skipped")) << out;
	}
	EXPECT_EQ(before, _contracts.size());
	ASSERT_EQ(1u, _contracts.count("rb2510"));
	EXPECT_EQ(500u, _contracts["rb2510"].m_maxMktQty);		// 第一条的值原样保留
}

TEST(t26_ctploader_instrument, margin_sentinels_dblmax_and_fltmax_become_zero)
{
	t26::setup_defaults("i12");
	CLASSMASK = 1;
	MAP_NAME["fu"] = "燃料油";

	CTraderSpi spi;
	auto f = t26::make_instrument("SHFE", "fu2512", "fu", THOST_FTDC_PC_Futures);
	f.LongMarginRatio = DBL_MAX;
	f.ShortMarginRatio = FLT_MAX;							// checkValid（L109-115）两个哨兵值都要归 0
	feed(spi, f);

	ASSERT_EQ(1u, _contracts.count("fu2512"));
	EXPECT_NEAR(0.0, _contracts["fu2512"].m_dLongMarginRatio, 1e-9);
	EXPECT_NEAR(0.0, _contracts["fu2512"].m_dShortMarginRatio, 1e-9);
}

/* ------------------------------------------------------------------ */
/* CZCE 三位年码展开（需要 _trading_day，登录驱动）                       */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_instrument, czce_future_code_is_expanded_and_altcode_kept)
{
	t26::setup_defaults("i13");
	CLASSMASK = 1;
	MAP_NAME["SR"] = "白砂糖";

	// 20251027 → yearFlag=5, yearPref=2, nextYPref=3（fillCzceCode L310-315 首调用锁存）
	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");	// worker stderr 走 fd 重定向（并发 rdbuf 换体会踩坏堆）
	CTraderSpi* spi = login_for_trading_day("20251027");
	auto f = t26::make_instrument("CZCE", "SR510", "SR", THOST_FTDC_PC_Futures);
	t26::set_fixed(f.UnderlyingInstrID, sizeof(f.UnderlyingInstrID), "SR510");
	feed(*spi, f);

	ASSERT_EQ(1u, _contracts.count("SR510"));				// map key 仍是 CTP 原码（L350/L520）
	const loader::Contract& c = _contracts["SR510"];
	EXPECT_EQ("SR2510", c.m_strCode);						// 补出的四位码（L423）
	EXPECT_EQ("SR510", c.m_strAltCode);					// 原码留档（L424）
	EXPECT_EQ("SR2510", c.m_strUnderlying);				// CZCE 连 underlying 也补位（L441-442）
	EXPECT_EQ("白砂糖510", c.m_strName);					// month 用「原码去品种前缀」（L396-398）
	ASSERT_EQ(1u, _commodities.count("CZCE.SR"));
	EXPECT_EQ(CM_OpenCover, _commodities["CZCE.SR"].m_coverMode);
	EXPECT_EQ(PM_Both, _commodities["CZCE.SR"].m_priceMode);
	ASSERT_TRUE(drain_login_queue());
	pUserApi = NULL;
}

TEST(t26_ctploader_instrument, czce_option_expands_code_and_strikes_through)
{
	t26::setup_defaults("i14");
	CLASSMASK = 3;
	MAP_NAME["SR"] = "白砂糖";

	t26::FdRedirectGuard worker_noise(SAVEPATH + "worker_noise.log");
	CTraderSpi* spi = login_for_trading_day("20251027");
	auto f = t26::make_instrument("CZCE", "SR510CS5600", "SR", THOST_FTDC_PC_Options);
	t26::set_fixed(f.UnderlyingInstrID, sizeof(f.UnderlyingInstrID), "SR510");
	f.OptionsType = '1';
	f.StrikePrice = 5700.5;
	f.UnderlyingMultiple = 2.5;
	feed(*spi, f);

	ASSERT_EQ(1u, _contracts.count("SR510CS5600"));
	const loader::Contract& c = _contracts["SR510CS5600"];
	EXPECT_EQ("SR2510CS5600", c.m_strCode);				// 第一个数字位前插十年位（L423）
	EXPECT_EQ("SR510CS5600", c.m_strAltCode);
	EXPECT_EQ("SR2510", c.m_strUnderlying);
	EXPECT_EQ("白砂糖510CS5600", c.m_strName);			// pname + InstrumentID.substr("SR"=2)（L400-406）
	EXPECT_EQ(OT_Call, c.m_optType);
	EXPECT_NEAR(5700.5, c.m_strikePrice, 1e-9);
	EXPECT_NEAR(2.5, c.m_dUnderlyingScale, 1e-9);
	ASSERT_TRUE(drain_login_queue());
	pUserApi = NULL;
}

/* ------------------------------------------------------------------ */
/* fillCzceCode 直调（锁存语义各占一个进程）                              */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_names, fill_czce_latches_decade_on_first_call_2025)
{
	t26::setup_defaults("i15");
	// 空码提前返回、**不消耗锁存**（L305-306 在 static 判定之前）
	EXPECT_EQ("", fillCzceCode("", 20251027));
	// 首调用 tdate=20251027 → yearFlag=5/yearPref=2/nextY=3，此后所有调用共用该锁存
	EXPECT_EQ("SR2510", fillCzceCode("SR510", 20251027));
	EXPECT_EQ("MA3412", fillCzceCode("MA412", 19990101));	// 4<5 → 借下个十年位 3；第二个参数被锁存忽略
	EXPECT_EQ("CF2901", fillCzceCode("CF901", 20251027));	// 9>=5 → 十年位 2
}

TEST(t26_ctploader_names, fill_czce_decade_zero_prefers_current_decade_digit)
{
	t26::setup_defaults("i16");
	// 首调用 tdate=20200106 → yearFlag=0 → 恒走 yearPref=2 分支（L326 的 yearFlag==0 一侧）
	EXPECT_EQ("CF2012", fillCzceCode("CF012", 20200106));
	EXPECT_EQ("SR2510", fillCzceCode("SR510", 20200106));	// cYFlag=5 也走同一条（yearFlag==0 短路）
}
