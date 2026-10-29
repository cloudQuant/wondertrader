/*
 * lane T4 —— CTPOptLoader 收发族用例
 *
 * 生产 TU：src/CTPOptLoader/CTPOptLoader.cpp + src/CTPOptLoader/TraderSpi.cpp，
 * 与 t_CTPOptLoader 的用例一起编进 CoverageTests_t_CTPOptLoader。
 *
 * CTPOptLoader 是「Loader 型 DLL 入口」：没有 createTrader/deleteTrader 工厂，
 * 唯一的 C 入口是 extern "C" int run(cfgfile, bAsync, isFile)，
 * 因此生命周期/空参防护落在 run() 的三条配置分支 + 必填项校验上。
 *
 * 刻意不进入的面（见 bugs/T4_femas_ctploader_mini.md）：
 *  - run() 的「必填项齐全」路径：会 DLLHelper::load_library 去 dlopen 厂商期权库，
 *    且在 g_ctpCreator == NULL 时仍然直接 pUserApi = g_ctpCreator(...) → 必崩；
 *    所有用例都靠 SAVEPATH 留空来命中 L198 的必填校验，在 dlopen 之前返回；
 *  - OnRspQryInstrument(bIsLast == true)：末尾无条件 exit(0)，会带走整个测试进程；
 *  - OnFrontConnected / ReqAuth / ReqUserLogin / ReqQryInstrument / OnRspUserLogin 成功分支：
 *    都要解引用全局 pUserApi（NULL）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>

#include "CTPOptLoader/TraderSpi.h"

#include "Includes/LoaderDef.hpp"

USING_NS_WTP;

/* --- 生产 TU 里的 C 入口与全局（都在 CTPOptLoader.cpp / TraderSpi.cpp 定义） --- */
extern "C" int run(const char* cfgfile, bool bAsync, bool isFile);

extern CThostFtdcTraderApi* pUserApi;
extern std::string FRONT_ADDR;
extern std::string BROKER_ID;
extern std::string INVESTOR_ID;
extern std::string PASSWORD;
extern std::string SAVEPATH;
extern std::string APPID;
extern std::string AUTHCODE;
extern std::string COMM_FILE;
extern std::string CONT_FILE;
extern std::string MODULE_NAME;
extern uint32_t CLASSMASK;
extern bool ONLYINCFG;
extern bool INCREMENTAL;
extern int iRequestID;
extern std::map<std::string, std::string> MAP_NAME;
extern std::map<std::string, std::string> MAP_SESSION;
extern loader::CommodityMap _commodities;
extern loader::ContractMap _contracts;

std::string extractProductID(const char* instrument);
std::string extractProductName(const char* cname);

namespace {

class CTPOptLoaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_commodities.clear();
		_contracts.clear();
		MAP_NAME.clear();
		MAP_SESSION.clear();
		CLASSMASK = 1 | 2 | 4;
		ONLYINCFG = false;
		INCREMENTAL = false;
		SAVEPATH = "";
		iRequestID = 0;
	}

	void TearDown() override
	{
		_commodities.clear();
		_contracts.clear();
		MAP_NAME.clear();
		MAP_SESSION.clear();
	}
};

CThostFtdcRspInfoField makeRsp(int errId, const char* msg)
{
	CThostFtdcRspInfoField rsp;
	memset(&rsp, 0, sizeof(rsp));
	rsp.ErrorID = errId;
	strncpy(rsp.ErrorMsg, msg, sizeof(rsp.ErrorMsg) - 1);
	return rsp;
}

CThostFtdcInstrumentField makeInstrument(const char* exchg, const char* id, const char* pid,
	const char* name, TThostFtdcProductClassType cls)
{
	CThostFtdcInstrumentField ins;
	memset(&ins, 0, sizeof(ins));
	strncpy(ins.ExchangeID, exchg, sizeof(ins.ExchangeID) - 1);
	strncpy(ins.InstrumentID, id, sizeof(ins.InstrumentID) - 1);
	strncpy(ins.ProductID, pid, sizeof(ins.ProductID) - 1);
	strncpy(ins.InstrumentName, name, sizeof(ins.InstrumentName) - 1);
	strncpy(ins.OpenDate, "20231101", sizeof(ins.OpenDate) - 1);
	strncpy(ins.ExpireDate, "20241130", sizeof(ins.ExpireDate) - 1);
	ins.ProductClass = cls;
	ins.MaxMarketOrderVolume = 500;
	ins.MaxLimitOrderVolume = 300;
	ins.MinMarketOrderVolume = 1;
	ins.MinLimitOrderVolume = 1;
	ins.VolumeMultiple = 10;
	ins.PriceTick = 1.0;
	ins.LongMarginRatio = 0.09;
	ins.ShortMarginRatio = 0.09;
	return ins;
}

}	// namespace

/* ------------------------------------------------------------------ */
/* run()：C 入口的参数校验 / 三条配置分支                               */
/* ------------------------------------------------------------------ */

TEST(CTPOptLoaderRun, yaml_content_fills_globals_and_stops_before_module_load)
{
	// 故意不给 config.path —— 命中 L198 必填校验，在 dlopen 之前返回
	const char* cfg =
		"ctp:\n"
		"  front: tcp://127.0.0.1:1234\n"
		"  broker: brk9999\n"
		"  user: inv001\n"
		"  pass: pwd001\n"
		"  appid: app-1\n"
		"  authcode: auth-1\n"
		"config:\n"
		"  mask: 3\n";

	FRONT_ADDR = "untouched";
	pUserApi = nullptr;

	EXPECT_EQ(run(cfg, true, false), 0);
	EXPECT_EQ(FRONT_ADDR, "tcp://127.0.0.1:1234");
	EXPECT_EQ(BROKER_ID, "brk9999");
	EXPECT_EQ(INVESTOR_ID, "inv001");
	EXPECT_EQ(PASSWORD, "pwd001");
	EXPECT_EQ(APPID, "app-1");
	EXPECT_EQ(AUTHCODE, "auth-1");
	EXPECT_EQ(CLASSMASK, 3u);
	// commfile/contfile/module 没配 -> 走内建默认
	EXPECT_EQ(COMM_FILE, "commodities.json");
	EXPECT_EQ(CONT_FILE, "contracts.json");
#ifndef _WIN32
	EXPECT_EQ(MODULE_NAME, "./soptthosttraderapi_se.so");
#endif
	EXPECT_FALSE(ONLYINCFG);
	EXPECT_FALSE(INCREMENTAL);
	// 关键安全断言：没有创建厂商 API，也就没有 dlopen
	EXPECT_EQ(pUserApi, nullptr);
}

TEST(CTPOptLoaderRun, yaml_content_honours_configured_names_and_flags)
{
	const char* cfg =
		"ctp:\n"
		"  front: tcp://10.0.0.1:3333\n"
		"  broker: '8888'\n"
		"  user: inv002\n"
		"  pass: pwd002\n"
		"  onlyincfg: true\n"
		"  incremental: true\n"
		"config:\n"
		"  mask: 7\n"
		"  commfile: mycomm.json\n"
		"  contfile: mycont.json\n"
		"  module: ./myoptapi.so\n";

	EXPECT_EQ(run(cfg, true, false), 0);
	EXPECT_EQ(CLASSMASK, 7u);
	EXPECT_EQ(COMM_FILE, "mycomm.json");
	EXPECT_EQ(CONT_FILE, "mycont.json");
	EXPECT_EQ(MODULE_NAME, "./myoptapi.so");
	EXPECT_TRUE(ONLYINCFG);
	EXPECT_TRUE(INCREMENTAL);
	EXPECT_EQ(SAVEPATH, "");
	EXPECT_EQ(pUserApi, nullptr);
}

TEST(CTPOptLoaderRun, empty_content_returns_zero_without_touching_globals)
{
	COMM_FILE = "sentinel-comm";
	FRONT_ADDR = "sentinel-front";

	// 空内容 -> YAML 根节点为 null -> 直接 return 0，连配置都不解析
	EXPECT_EQ(run("", true, false), 0);
	EXPECT_EQ(COMM_FILE, "sentinel-comm");
	EXPECT_EQ(FRONT_ADDR, "sentinel-front");
	EXPECT_EQ(pUserApi, nullptr);
}

TEST(CTPOptLoaderRun, missing_json_file_returns_zero_without_touching_globals)
{
	// isFile=true 且不以 .ini 结尾 -> load_from_file 路径；文件不存在返回 NULL
	FRONT_ADDR = "sentinel-front";
	CONT_FILE = "sentinel-cont";
	EXPECT_EQ(run("t4_no_such_loader_cfg.json", true, true), 0);
	EXPECT_EQ(FRONT_ADDR, "sentinel-front");
	EXPECT_EQ(CONT_FILE, "sentinel-cont");
	EXPECT_EQ(pUserApi, nullptr);
}

TEST(CTPOptLoaderRun, yaml_content_missing_ctp_section_returns_zero_without_touching_globals)
{
	const char* cfg =
		"config:\n"
		"  path: ''\n"
		"  mask: 0\n";
	FRONT_ADDR = "sentinel-front";
	CONT_FILE = "sentinel-cont";

	EXPECT_EQ(run(cfg, true, false), 0);
	EXPECT_EQ(FRONT_ADDR, "sentinel-front");
	EXPECT_EQ(CONT_FILE, "sentinel-cont");
	EXPECT_EQ(pUserApi, nullptr);
}

TEST(CTPOptLoaderRun, yaml_content_missing_config_section_returns_zero_without_touching_globals)
{
	const char* cfg =
		"ctp:\n"
		"  front: tcp://127.0.0.1:1234\n"
		"  broker: brk9999\n"
		"  user: inv001\n"
		"  pass: pwd001\n"
		"  appid: app-1\n"
		"  authcode: auth-1\n";
	FRONT_ADDR = "sentinel-front";
	CONT_FILE = "sentinel-cont";

	EXPECT_EQ(run(cfg, true, false), 0);
	EXPECT_EQ(FRONT_ADDR, "sentinel-front");
	EXPECT_EQ(CONT_FILE, "sentinel-cont");
	EXPECT_EQ(pUserApi, nullptr);
}

TEST(CTPOptLoaderRun, ini_branch_applies_builtin_defaults_for_absent_file)
{
	// .ini 后缀走 IniHelper 分支；文件不存在时 load() 内部吞掉异常，全部取默认值
	CLASSMASK = 0;
	COMM_FILE = "sentinel";
	ONLYINCFG = true;
	INCREMENTAL = true;

	EXPECT_EQ(run("t4_no_such_loader_cfg.ini", true, true), 0);
	// ini 分支的 mask 默认是 1|2|4，与 yaml 分支的 0 默认值不同
	EXPECT_EQ(CLASSMASK, 7u);
	EXPECT_EQ(COMM_FILE, "commodities.json");
	EXPECT_EQ(CONT_FILE, "contracts.json");
	EXPECT_EQ(FRONT_ADDR, "");
	EXPECT_FALSE(ONLYINCFG);
	EXPECT_FALSE(INCREMENTAL);
	EXPECT_EQ(pUserApi, nullptr);
}

/* ------------------------------------------------------------------ */
/* 代码/合约换算自由函数                                                */
/* ------------------------------------------------------------------ */

TEST(CTPOptLoaderCodes, extract_product_id_stops_at_first_digit)
{
	EXPECT_EQ(extractProductID("rb2401"), "rb");
	EXPECT_EQ(extractProductID("IF2403"), "IF");
	EXPECT_EQ(extractProductID("wc1911"), "wc");
	// 纯数字代码 -> 空串
	EXPECT_EQ(extractProductID("2401"), "");
	// 字母全部保留（含 '_' 这类落在 'Z'~'z' 之间的字符，见 bug 记录）
	EXPECT_EQ(extractProductID("rb"), "rb");
}

TEST(CTPOptLoaderCodes, extract_product_name_strips_trailing_digits)
{
	EXPECT_EQ(extractProductName("Rebar2401"), "Rebar");
	EXPECT_EQ(extractProductName("RB2401"), "RB");
	// 没有尾缀数字 -> 原样返回
	EXPECT_EQ(extractProductName("Rebar"), "Rebar");
	// 只剥尾部连续数字，中间数字不动
	EXPECT_EQ(extractProductName("copper2401option9"), "copper2401option");
}

/* ------------------------------------------------------------------ */
/* OnRspQryInstrument：厂商合约结构 → loader 结构                       */
/* ------------------------------------------------------------------ */

TEST_F(CTPOptLoaderTest, futures_instrument_builds_commodity_and_contract)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcInstrumentField ins =
		makeInstrument("SHFE", "rb2401", "rb", "Rebar2401", THOST_FTDC_PC_Futures);

	// bIsLast 必须为 false：末尾无条件 exit(0)
	spi.OnRspQryInstrument(&ins, &ok, 1, false);

	ASSERT_EQ(_commodities.size(), 1u);
	const loader::Commodity& comm = _commodities["SHFE.rb"];
	EXPECT_EQ(comm.m_strProduct, "rb");
	// 未配置映射时，品种名由合约名去掉尾缀数字推导
	EXPECT_EQ(comm.m_strName, "Rebar");
	EXPECT_EQ(comm.m_strExchg, "SHFE");
	EXPECT_EQ(comm.m_strCurrency, "CNY");
	EXPECT_EQ(comm.m_ccCategory, CC_Future);
	EXPECT_EQ(comm.m_uVolScale, 10u);
	EXPECT_DOUBLE_EQ(comm.m_fPriceTick, 1.0);
	// PriceTick 不小于 1 -> 精度 0
	EXPECT_EQ(comm.m_uPrecision, 0u);
	// 上期所期货：平今 + 只支持限价
	EXPECT_EQ(comm.m_coverMode, CM_CoverToday);
	EXPECT_EQ(comm.m_priceMode, PM_Limit);
	EXPECT_EQ(comm.m_tradeMode, TM_Both);

	ASSERT_EQ(_contracts.size(), 1u);
	const loader::Contract& cont = _contracts["SHFE.rb2401"];
	EXPECT_EQ(cont.m_strCode, "rb2401");
	EXPECT_EQ(cont.m_strExchg, "SHFE");
	EXPECT_EQ(cont.m_strName, "Rebar2401");
	EXPECT_EQ(cont.m_strProduct, "rb");
	EXPECT_EQ(cont.m_maxMktQty, 500u);
	EXPECT_EQ(cont.m_maxLmtQty, 300u);
	EXPECT_EQ(cont.m_uOpenDate, 20231101u);
	EXPECT_EQ(cont.m_uExpireDate, 20241130u);
	EXPECT_DOUBLE_EQ(cont.m_dLongMarginRatio, 0.09);
	// 非期权 -> OT_None，且 volScale 不在合约上单独保存
	EXPECT_EQ(cont.m_optType, OT_None);
	EXPECT_EQ(cont.m_uVolScale, 0u);
}

TEST_F(CTPOptLoaderTest, non_shfe_future_uses_open_cover_and_both_price_modes)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcInstrumentField ins =
		makeInstrument("DCE", "i2405", "i", "IronOre2405", THOST_FTDC_PC_Futures);
	ins.PriceTick = 0.5;

	spi.OnRspQryInstrument(&ins, &ok, 1, false);

	ASSERT_EQ(_commodities.count("DCE.i"), 1u);
	const loader::Commodity& comm = _commodities["DCE.i"];
	EXPECT_EQ(comm.m_coverMode, CM_OpenCover);
	EXPECT_EQ(comm.m_priceMode, PM_Both);
	// 0.5 < PriceTick < 1 -> 1 位小数
	EXPECT_EQ(comm.m_uPrecision, 1u);
	EXPECT_EQ(_contracts.count("DCE.i2405"), 1u);
}

TEST_F(CTPOptLoaderTest, option_instrument_keeps_contract_level_volume_multiple)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcInstrumentField ins =
		makeInstrument("DCE", "m2409-C-3000", "m_o", "M-Opt", THOST_FTDC_PC_Options);
	ins.VolumeMultiple = 0;			// 0 -> 兜底成 1
	ins.OptionsType = 1;
	strncpy(ins.UnderlyingInstrID, "m2409", sizeof(ins.UnderlyingInstrID) - 1);
	ins.StrikePrice = 3000.0;
	ins.UnderlyingMultiple = 1.0;

	spi.OnRspQryInstrument(&ins, &ok, 1, false);

	ASSERT_EQ(_commodities.count("DCE.m_o"), 1u);
	const loader::Commodity& comm = _commodities["DCE.m_o"];
	EXPECT_EQ(comm.m_ccCategory, CC_FutOption);
	EXPECT_EQ(comm.m_uVolScale, 1u);
	// 非期货 -> 开平 + 限价/市价都支持
	EXPECT_EQ(comm.m_coverMode, CM_OpenCover);
	EXPECT_EQ(comm.m_priceMode, PM_Both);

	ASSERT_EQ(_contracts.count("DCE.m2409-C-3000"), 1u);
	const loader::Contract& cont = _contracts["DCE.m2409-C-3000"];
	EXPECT_EQ(cont.m_optType, (OptionType)1);
	EXPECT_EQ(cont.m_strUnderlying, "m2409");
	EXPECT_DOUBLE_EQ(cont.m_strikePrice, 3000.0);
	// 期权合约的 volScale 单独落一份（0 被兜底成 1）
	EXPECT_EQ(cont.m_uVolScale, 1u);
	// 期权名不拆数字，直接用厂商合约名
	EXPECT_EQ(cont.m_strName, "M-Opt");
}

TEST_F(CTPOptLoaderTest, class_mask_filters_ungranted_categories)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");

	CLASSMASK = 2;		// 只要期权
	CThostFtdcInstrumentField fut =
		makeInstrument("SHFE", "rb2401", "rb", "Rebar2401", THOST_FTDC_PC_Futures);
	spi.OnRspQryInstrument(&fut, &ok, 1, false);
	EXPECT_TRUE(_commodities.empty());
	EXPECT_TRUE(_contracts.empty());

	CLASSMASK = 1;		// 只要期货，期权包被丢掉
	CThostFtdcInstrumentField opt =
		makeInstrument("DCE", "m2409-C-3000", "m_o", "M-Opt", THOST_FTDC_PC_Options);
	spi.OnRspQryInstrument(&opt, &ok, 2, false);
	EXPECT_TRUE(_contracts.empty());

	// 既非期货/期权/股票的类别（组合）在 isGranted 判断里直接被排除
	CLASSMASK = 1 | 2 | 4;
	CThostFtdcInstrumentField combo =
		makeInstrument("SHFE", "spf2401", "spf", "Spread2401", THOST_FTDC_PC_Combination);
	spi.OnRspQryInstrument(&combo, &ok, 3, false);
	EXPECT_TRUE(_commodities.empty());
	EXPECT_TRUE(_contracts.empty());
}

TEST_F(CTPOptLoaderTest, onlyincfg_drops_instruments_without_a_name_mapping)
{
	ONLYINCFG = true;
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcInstrumentField ins =
		makeInstrument("SHFE", "rb2401", "rb", "Rebar2401", THOST_FTDC_PC_Futures);

	spi.OnRspQryInstrument(&ins, &ok, 1, false);

	// MAP_NAME 里没有 rb / SHFE.rb -> break，一条都不落地
	EXPECT_TRUE(_commodities.empty());
	EXPECT_TRUE(_contracts.empty());

	MAP_NAME["SHFE.rb"] = "Rebar";
	spi.OnRspQryInstrument(&ins, &ok, 2, false);
	EXPECT_EQ(_commodities.count("SHFE.rb"), 1u);
}

TEST_F(CTPOptLoaderTest, mapped_product_name_rebuilds_contract_name)
{
	// 命中映射分支：合约名 = 品种名 + InstrumentID 去掉 pid 前缀
	MAP_NAME["rb"] = "Rebar";
	MAP_SESSION["SHFE.rb"] = "SN001";

	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcInstrumentField ins =
		makeInstrument("SHFE", "rb2401", "rb", "RB2401-ANY", THOST_FTDC_PC_Futures);

	spi.OnRspQryInstrument(&ins, &ok, 1, false);

	ASSERT_EQ(_contracts.count("SHFE.rb2401"), 1u);
	EXPECT_EQ(_contracts["SHFE.rb2401"].m_strName, "Rebar2401");
	EXPECT_EQ(_commodities["SHFE.rb"].m_strName, "Rebar");
	// 交易时段来自 MAP_SESSION[exchg.pid]
	EXPECT_EQ(_commodities["SHFE.rb"].m_strSession, "SN001");
}

TEST_F(CTPOptLoaderTest, error_packet_and_null_instrument_collect_nothing)
{
	CTraderSpi spi;
	CThostFtdcInstrumentField ins =
		makeInstrument("SHFE", "rb2401", "rb", "Rebar2401", THOST_FTDC_PC_Futures);
	CThostFtdcRspInfoField bad = makeRsp(3, "query failed");

	spi.OnRspQryInstrument(&ins, &bad, 1, false);
	spi.OnRspQryInstrument(nullptr, nullptr, 2, false);

	EXPECT_TRUE(_commodities.empty());
	EXPECT_TRUE(_contracts.empty());
}

/* ------------------------------------------------------------------ */
/* 其余 SPI 回调（不得触碰 pUserApi）                                   */
/* ------------------------------------------------------------------ */

TEST_F(CTPOptLoaderTest, authenticate_rejection_does_not_fire_login_request)
{
	CTraderSpi spi;
	CThostFtdcRspAuthenticateField auth;
	memset(&auth, 0, sizeof(auth));
	CThostFtdcRspInfoField bad = makeRsp(2, "appid invalid");

	iRequestID = 100;
	spi.OnRspAuthenticate(&auth, &bad, 5, true);

	// 错误应答 -> 不再发登录请求，因此请求号没有变化
	EXPECT_EQ(iRequestID, 100);
	EXPECT_EQ(pUserApi, nullptr);
}

TEST_F(CTPOptLoaderTest, login_rejection_and_error_packet_are_side_effect_free)
{
	CTraderSpi spi;
	CThostFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));
	login.FrontID = 1;
	login.SessionID = 2;
	strncpy(login.MaxOrderRef, "7", sizeof(login.MaxOrderRef) - 1);
	CThostFtdcRspInfoField bad = makeRsp(63, "not authorized");

	iRequestID = 55;
	spi.OnRspUserLogin(&login, &bad, 1, true);

	// 被拒绝的登录不会推进到查询阶段（推进就会解引用 NULL 的 pUserApi）
	EXPECT_EQ(iRequestID, 55);
	EXPECT_EQ(pUserApi, nullptr);

	spi.OnRspError(&bad, 2, true);
	EXPECT_EQ(iRequestID, 55);
	EXPECT_NO_THROW(spi.OnFrontDisconnected(4));
	EXPECT_TRUE(_commodities.empty());
}

/* ------------------------------------------------------------------ */
/* 崩溃/退出路径：整条 DISABLED，说明见 bugs/T4_femas_ctploader_mini.md  */
/* ------------------------------------------------------------------ */

/*
 * BUG-T4-015：OnRspQryInstrument 收到最后一包时无条件 DumpToJson() + exit(0)。
 * 在测试进程里 exit(0) 会直接把整个 gtest 二进制带走（返回值还会被当成「全部通过」），
 * 所以这条只能禁用；启用侧的所有用例都用 bIsLast=false 绕开。
 */
TEST_F(CTPOptLoaderTest, DISABLED_last_instrument_packet_exits_the_process)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcInstrumentField ins =
		makeInstrument("SHFE", "rb2401", "rb", "Rebar2401", THOST_FTDC_PC_Futures);

	spi.OnRspQryInstrument(&ins, &ok, 1, true);
	// 修复前永远走不到这里
	EXPECT_TRUE(_contracts.empty());
}

/*
 * BUG-T4-014：run() 的必填项齐全后就会 DLLHelper::load_library dlopen 厂商期权库，
 * 且 `if (g_ctpCreator == NULL)` 只 printf 不返回，下一行仍然 g_ctpCreator(flowPath)
 * → 空函数指针调用；同时 fs::create_directories("./CTPFlow/...") 会往 CWD 写目录。
 * 与「不 dlopen / 不写产物」约束冲突，不启用。
 */
TEST(CTPOptLoaderDisabled, DISABLED_complete_config_reaches_dlopen_and_null_creator)
{
	const char* cfg =
		"ctp:\n"
		"  front: tcp://127.0.0.1:1234\n"
		"  broker: brk9999\n"
		"  user: inv001\n"
		"  pass: pwd001\n"
		"config:\n"
		"  path: ./t4_ctpopt_dump\n"
		"  mask: 3\n";

	run(cfg, true, false);
	// 修复后应当：模块加载失败就返回非 0，而不是继续调用空 creator
	EXPECT_EQ(pUserApi, nullptr);
}

/*
 * BUG-T4-018 家族：OnFrontConnected → ReqAuth、OnRspUserLogin 成功分支
 * 都要解引用全局 pUserApi（未 run 过时是 NULL）→ 必崩。
 */
TEST(CTPOptLoaderDisabled, DISABLED_front_connected_and_login_success_require_vendor_api)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CThostFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));
	strncpy(login.MaxOrderRef, "7", sizeof(login.MaxOrderRef) - 1);

	spi.OnRspUserLogin(&login, &ok, 1, true);
	// 修复后（有 API 实例）：会话号被保存并推进到查询阶段
	EXPECT_EQ(iRequestID, 0);
}
