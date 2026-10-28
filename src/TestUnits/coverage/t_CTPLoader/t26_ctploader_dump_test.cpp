/*
 * lane R26 — DumpToJson / DumpFees 落盘用例（fork 隔离 exit(0)）
 *
 * 现实约束：OnRspQryInstrument(bIsLast=true, !QRYFEES) 以 `DumpToJson(); exit(0);` 收尾
 * （L542-547），两条费率回报回调在 `QRYFEES && _queries.empty()` 时同样是
 * `DumpToJson(); DumpFees(); exit(0);`（L570-575 / L587-592）。直接在本进程触发会杀掉
 * 测试进程并丢 profraw，所以这些「终局」都放进 fork 的子进程里跑：
 * 父进程 waitpid 校验「子进程以退出码 0 正常结束」（exit(0) 本身就是被测行为的一部分），
 * 再从磁盘把 JSON 读回来做可证伪断言。fork 点之前不建任何 worker 线程（用 bIsLast=false /
 * SET_FILTERS 挡队列），单线程 fork 安全。
 */
#include "t26_ctploader_common.hpp"
#include "t26_ctp_api_stub.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <functional>

#include <rapidjson/document.h>
namespace rj = rapidjson;

namespace
{
	/// 在子进程里执行 fn（fn 应当以生产代码的 exit(0) 收尾），并断言子进程正常退出
	void run_in_child(const std::function<void()>& fn)
	{
		fflush(nullptr);
		pid_t pid = fork();
		ASSERT_TRUE(pid >= 0) << "fork failed";
		if (pid == 0)
		{
			fn();
			// fn 没在预期位置 exit(0) —— 用非零码让父进程能区分「dump 出口失效」
			_exit(42);
		}
		int status = 0;
		pid_t done = waitpid(pid, &status, 0);
		EXPECT_EQ(pid, done);
		if (WIFEXITED(status))
			EXPECT_EQ(0, WEXITSTATUS(status)) << "子进程应以 exit(0) 结束（落盘出口本身是断言对象）";
		else
			FAIL() << "子进程非正常退出，status=" << status;
	}

	const rj::Value* jget(const rj::Value& obj, const char* key)
	{
		auto it = obj.FindMember(key);
		return it == obj.MemberEnd() ? NULL : &it->value;
	}

	bool parse_dumped(const std::string& absPath, rj::Document& doc)
	{
		std::string text = t26::read_whole_file(absPath);
		if (text.empty())
			return false;
		doc.Parse(text.c_str());
		return !doc.HasParseError();
	}

	// 直接往全局映射塞一条合约（绕开回调，专测 DumpToJson 的「键=换算后代码」语义）
	loader::Contract make_contract(const std::string& exchg, const std::string& code,
		const std::string& alt, const std::string& product)
	{
		loader::Contract c;
		c.m_strCode = code;
		c.m_strAltCode = alt;
		c.m_strExchg = exchg;
		c.m_strName = code + "-name";
		c.m_strProduct = product;
		c.m_maxMktQty = 300;
		c.m_maxLmtQty = 500;
		c.m_minMktQty = 1;
		c.m_minLmtQty = 2;
		c.m_uOpenDate = 20240513;
		c.m_uExpireDate = 20250930;
		c.m_dLongMarginRatio = 0.08;
		c.m_dShortMarginRatio = 0.09;
		c.m_optType = OT_None;
		c.m_strikePrice = 0.0;
		c.m_dUnderlyingScale = 0.0;
		return c;
	}

	loader::Commodity make_commodity(const std::string& exchg, const std::string& product)
	{
		loader::Commodity m;
		m.m_strProduct = product;
		m.m_strName = product + "-comm";
		m.m_strExchg = exchg;
		m.m_strCurrency = "CNY";
		m.m_strSession = "s1";
		m.m_uVolScale = 10;
		m.m_fPriceTick = 0.5;
		m.m_uPrecision = 1;
		m.m_ccCategory = CC_Future;
		m.m_coverMode = CM_OpenCover;
		m.m_priceMode = PM_Both;
		m.m_tradeMode = TM_Both;
		return m;
	}
}

/* ------------------------------------------------------------------ */
/* DumpToJson（经 OnRspQryInstrument bIsLast 出口）                      */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_dump, last_instrument_response_dumps_commodities_and_contracts)
{
	t26::setup_defaults("p1");
	CLASSMASK = 3;
	QRYFEES = false;						// 决定走 L542-547 的 DumpToJson+exit 一侧
	MAP_NAME["rb"] = "螺纹钢";
	MAP_NAME["m"] = "豆粕";
	MAP_SESSION["SHFE.rb"] = "dn";

	CTraderSpi spi;
	{
		auto f = t26::make_instrument("SHFE", "rb2510", "rb", THOST_FTDC_PC_Futures);
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrument(&f, &ok, 60, false);
	}
	{
		auto f = t26::make_instrument("DCE", "m2509-C-3000", "m", THOST_FTDC_PC_Options);
		t26::set_fixed(f.UnderlyingInstrID, sizeof(f.UnderlyingInstrID), "m2509");
		f.OptionsType = '1';
		f.StrikePrice = 3000.0;
		f.UnderlyingMultiple = 1.0;
		auto ok = t26::ok_rsp();
		spi.OnRspQryInstrument(&f, &ok, 61, false);
	}

	// —— 子进程触发终局（NULL 载荷 + bIsLast=true，主体跳过、只走出口）
	auto ok = t26::ok_rsp();
	run_in_child([&spi, &ok]() { spi.OnRspQryInstrument(NULL, &ok, 99, true); });

	// —— 品种文件
	rj::Document comms;
	ASSERT_TRUE(parse_dumped(SAVEPATH + COMM_FILE, comms));
	const rj::Value* shfe = jget(comms, "SHFE");
	ASSERT_TRUE(shfe != NULL);
	const rj::Value* rb = jget(*shfe, "rb");
	ASSERT_TRUE(rb != NULL);
	EXPECT_EQ("螺纹钢", std::string(jget(*rb, "name")->GetString()));
	EXPECT_EQ("SHFE", std::string(jget(*rb, "exchg")->GetString()));
	EXPECT_EQ("dn", std::string(jget(*rb, "session")->GetString()));
	EXPECT_EQ("CHINA", std::string(jget(*rb, "holiday")->GetString()));
	// currency 装载时恒为 CNY（L467），但 DumpToJson 根本不落这个字段 → 增量往返必丢（行为钉子）
	EXPECT_TRUE(jget(*rb, "currency") == NULL);
	// 枚举按数值落盘：CM_CoverToday=1 / PM_Limit=1 / CC_Future=1 / TM_Both=0（L751-757）
	EXPECT_EQ(1u, jget(*rb, "covermode")->GetUint());
	EXPECT_EQ(1u, jget(*rb, "pricemode")->GetUint());
	EXPECT_EQ(1u, jget(*rb, "category")->GetUint());
	EXPECT_EQ(0u, jget(*rb, "trademode")->GetUint());
	EXPECT_EQ(0u, jget(*rb, "precision")->GetUint());
	EXPECT_NEAR(1.0, jget(*rb, "pricetick")->GetDouble(), 1e-9);
	EXPECT_EQ(10u, jget(*rb, "volscale")->GetUint());

	const rj::Value* dce = jget(comms, "DCE");
	ASSERT_TRUE(dce != NULL);
	const rj::Value* mo = jget(*dce, "m");	// _commodities 的 key 是 DCE.m，落盘键用 m_strProduct（L765）
	if (mo != NULL)
	{
		EXPECT_EQ(2u, jget(*mo, "category")->GetUint());	// CC_FutOption
		EXPECT_EQ(0u, jget(*mo, "covermode")->GetUint());	// DCE → 开平
	}

	// —— 合约文件
	rj::Document conts;
	ASSERT_TRUE(parse_dumped(SAVEPATH + CONT_FILE, conts));
	const rj::Value* shfeC = jget(conts, "SHFE");
	ASSERT_TRUE(shfeC != NULL);
	const rj::Value* rb2510 = jget(*shfeC, "rb2510");
	ASSERT_TRUE(rb2510 != NULL);
	EXPECT_EQ("螺纹钢2510", std::string(jget(*rb2510, "name")->GetString()));
	EXPECT_EQ("rb2510", std::string(jget(*rb2510, "code")->GetString()));
	EXPECT_EQ("", std::string(jget(*rb2510, "altcode")->GetString()));
	EXPECT_EQ("rb", std::string(jget(*rb2510, "product")->GetString()));
	// make_instrument 的夹具形状：MaxLimit=1000 / MaxMarket=500 / MinLimit=2 / MinMarket=1
	EXPECT_EQ(1000u, jget(*rb2510, "maxlimitqty")->GetUint());
	EXPECT_EQ(500u, jget(*rb2510, "maxmarketqty")->GetUint());
	EXPECT_EQ(2u, jget(*rb2510, "minlimitqty")->GetUint());
	EXPECT_EQ(1u, jget(*rb2510, "minmarketqty")->GetUint());
	EXPECT_EQ(20170101u, jget(*rb2510, "opendate")->GetUint());
	EXPECT_EQ(20251015u, jget(*rb2510, "expiredate")->GetUint());
	EXPECT_NEAR(0.1, jget(*rb2510, "longmarginratio")->GetDouble(), 1e-9);
	EXPECT_NEAR(0.12, jget(*rb2510, "shortmarginratio")->GetDouble(), 1e-9);
	EXPECT_TRUE(jget(*rb2510, "option") == NULL);		// OT_None 不落 option 块（L801）

	const rj::Value* dceC = jget(conts, "DCE");
	ASSERT_TRUE(dceC != NULL);
	const rj::Value* opt = jget(*dceC, "m2509-C-3000");
	ASSERT_TRUE(opt != NULL);
	const rj::Value* optBlock = jget(*opt, "option");
	ASSERT_TRUE(optBlock != NULL);
	EXPECT_EQ(static_cast<unsigned int>(OT_Call), jget(*optBlock, "optiontype")->GetUint());
	EXPECT_EQ("m2509", std::string(jget(*optBlock, "underlying")->GetString()));
	EXPECT_NEAR(3000.0, jget(*optBlock, "strikeprice")->GetDouble(), 1e-9);
	EXPECT_NEAR(1.0, jget(*optBlock, "underlyingscale")->GetDouble(), 1e-9);
}

TEST(t26_ctploader_dump, czce_contract_is_keyed_by_expanded_code_with_altcode_kept)
{
	t26::setup_defaults("p2");
	// 直接装配映射：证明 DumpToJson 的落盘键是「换算后四位代码」、altcode 字段留 CTP 原码
	_contracts["SR2510"] = make_contract("CZCE", "SR2510", "SR510", "SR");
	_commodities["CZCE.SR"] = make_commodity("CZCE", "SR");

	CTraderSpi spi;
	auto ok = t26::ok_rsp();
	run_in_child([&spi, &ok]() { spi.OnRspQryInstrument(NULL, &ok, 99, true); });

	rj::Document comms;
	ASSERT_TRUE(parse_dumped(SAVEPATH + COMM_FILE, comms));
	const rj::Value* czce = jget(comms, "CZCE");
	ASSERT_TRUE(czce != NULL);
	const rj::Value* sr = jget(*czce, "SR");
	ASSERT_TRUE(sr != NULL);
	EXPECT_NEAR(0.5, jget(*sr, "pricetick")->GetDouble(), 1e-9);
	EXPECT_EQ(1u, jget(*sr, "precision")->GetUint());

	rj::Document conts;
	ASSERT_TRUE(parse_dumped(SAVEPATH + CONT_FILE, conts));
	const rj::Value* c = jget(*jget(conts, "CZCE"), "SR2510");
	ASSERT_TRUE(c != NULL);
	EXPECT_EQ("SR2510", std::string(jget(*c, "code")->GetString()));
	EXPECT_EQ("SR510", std::string(jget(*c, "altcode")->GetString()));
}

/* ------------------------------------------------------------------ */
/* DumpFees（经费率回报出口；顺带覆盖两个 exit 出口各一侧）                */
/* ------------------------------------------------------------------ */

TEST(t26_ctploader_dump, margin_response_dumps_fees_to_fixed_relative_name_with_product_fallback)
{
	t26::setup_defaults("p3");
	QRYFEES = true;							// L587-592 的 DumpToJson+DumpFees+exit(0) 出口
	FEES_FILE = "custom_fees.json";			// 相对名 —— 产品落盘却硬编码 "fees.json"（L717-721，BUG-T26-006 钉子）

	_contracts["ao2512"] = make_contract("SHFE", "ao2512", "", "ao");
	_commodities["SHFE.ao"] = make_commodity("SHFE", "ao");
	loader::FeeInfo prodFee;
	prodFee._open = 0.5;
	prodFee._close = 0.6;
	prodFee._closet = 0.7;
	prodFee._byvol = true;
	_fees["ao"] = prodFee;					// 只有品种级费率 → DumpFees 的 L697-698 兜底命中

	CThostFtdcInstrumentMarginRateField mf;
	std::memset(&mf, 0, sizeof(mf));
	t26::set_fixed(mf.InstrumentID, sizeof(mf.InstrumentID), "ao2512");
	mf.LongMarginRatioByMoney = 0.07;
	mf.ShortMarginRatioByMoney = 0.08;

	CTraderSpi spi;
	auto ok = t26::ok_rsp();
	// _queries 全程为空（QRYFEES 的 bIsLast 队列路径没走），回报一进即触发终局
	run_in_child([&spi, &mf, &ok]() { spi.OnRspQryInstrumentMarginRate(&mf, &ok, 77, true); });

	const std::string feesPath = SAVEPATH + "fees.json";
	ASSERT_TRUE(std::filesystem::exists(feesPath));
	EXPECT_FALSE(std::filesystem::exists(SAVEPATH + FEES_FILE));	// 配置名被硬编码覆盖（行为钉子）

	rj::Document fees;
	ASSERT_TRUE(parse_dumped(feesPath, fees));
	const rj::Value* row = jget(fees, "SHFE.ao2512");				// 键 = {}.{} exchg.code（L708）
	ASSERT_TRUE(row != NULL);
	EXPECT_NEAR(0.5, jget(*row, "open")->GetDouble(), 1e-9);
	EXPECT_NEAR(0.6, jget(*row, "close")->GetDouble(), 1e-9);
	EXPECT_NEAR(0.7, jget(*row, "closetoday")->GetDouble(), 1e-9);
	EXPECT_TRUE(jget(*row, "byvolume")->GetBool());
	EXPECT_NEAR(0.07, jget(*row, "margin")->GetDouble(), 1e-9);		// margin 只取多头比例（L706）
}

TEST(t26_ctploader_dump, commission_response_with_absolute_feesfile_dumps_to_that_path)
{
	t26::setup_defaults("p4");
	QRYFEES = true;
	const std::string absFees = SAVEPATH + "abs_fees.json";
	FEES_FILE = absFees;					// 绝对路径 → L713-716 直用 FEES_FILE

	_contracts["rb2510"] = make_contract("SHFE", "rb2510", "", "rb");
	_commodities["SHFE.rb"] = make_commodity("SHFE", "rb");
	loader::MarginInfo mi;
	mi._long = 0.11;
	mi._short = 0.13;
	_margins["rb2510"] = mi;				// 先把保证金表备好（否则没有行可落）

	CThostFtdcInstrumentCommissionRateField cf;
	std::memset(&cf, 0, sizeof(cf));
	t26::set_fixed(cf.InstrumentID, sizeof(cf.InstrumentID), "rb2510");
	cf.OpenRatioByMoney = 0.0001;				// > eps(1e-6) → 按额
	cf.CloseRatioByMoney = 0.0002;
	cf.CloseTodayRatioByMoney = 0.0003;

	CTraderSpi spi;
	auto ok = t26::ok_rsp();
	run_in_child([&spi, &cf, &ok]() { spi.OnRspQryInstrumentCommissionRate(&cf, &ok, 78, true); });

	ASSERT_TRUE(std::filesystem::exists(absFees));
	rj::Document fees;
	ASSERT_TRUE(parse_dumped(absFees, fees));
	const rj::Value* row = jget(fees, "SHFE.rb2510");
	ASSERT_TRUE(row != NULL);
	EXPECT_NEAR(0.0001, jget(*row, "open")->GetDouble(), 1e-9);
	EXPECT_NEAR(0.0002, jget(*row, "close")->GetDouble(), 1e-9);
	EXPECT_NEAR(0.0003, jget(*row, "closetoday")->GetDouble(), 1e-9);
	EXPECT_FALSE(jget(*row, "byvolume")->GetBool());
	EXPECT_NEAR(0.11, jget(*row, "margin")->GetDouble(), 1e-9);

	// 同一出口还顺手 DumpToJson：两个主文件也应落盘（覆盖 L572 的另一半）
	EXPECT_TRUE(std::filesystem::exists(SAVEPATH + COMM_FILE));
	EXPECT_TRUE(std::filesystem::exists(SAVEPATH + CONT_FILE));
}

/* ------------------------------------------------------------------ */
/* 不能启用的落盘路径                                                     */
/* ------------------------------------------------------------------ */

// DumpFees 对「保证金里有、费率表与品种兜底都没有」的代码直接 `it->second`
// （L696-699 两次 find 都 miss 后没有 end() 判定）→ 解引用 end 迭代器，UB，
// 崩不崩取决于 allocator 邻接内存，无法写可证伪断言，只登记；BUG-T26-003。
TEST(DISABLED_t26_ctploader_dump_crash, fees_dump_of_unknown_margin_code_dereferences_end_iterator)
{
	t26::setup_defaults("p5");
	QRYFEES = true;
	_margins["ZZ99"] = loader::MarginInfo{ 0.1, 0.1 };	// 既无合约行也无费率行

	CThostFtdcInstrumentMarginRateField mf;
	std::memset(&mf, 0, sizeof(mf));
	t26::set_fixed(mf.InstrumentID, sizeof(mf.InstrumentID), "ZZ99");

	CTraderSpi spi;
	auto ok = t26::ok_rsp();
	spi.OnRspQryInstrumentMarginRate(&mf, &ok, 79, true);	// ← DumpFees UB / 崩溃
}
