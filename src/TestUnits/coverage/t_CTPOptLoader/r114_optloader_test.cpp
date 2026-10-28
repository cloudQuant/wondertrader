/*
 * lane R114 —— CTPOptLoader 正向面测试
 *
 * 生产 TU：src/CTPOptLoader/CTPOptLoader.cpp（268 行）+ TraderSpi.cpp（623 行），
 * 与既有 ctpopt_loader_test.cpp 一起编进 CoverageTests_t_CTPOptLoader。
 *
 * 与 T4 的差异：本文件用 r114_ctpopt_api_stub.h 给出
 * ctp_sopt::CThostFtdcTraderApi::CreateFtdcTraderApi 的强定义，并把配置里的
 * module 指向测试可执行文件自身。macOS 的 dlopen 可执行文件 + dlsym 能取到该
 * 全局符号，因此 run() 的 203-267 正路径无需厂商 dylib、也不会有空 creator；
 * 所有 mock 方法只记调用，不起线程、不连柜台。
 */
#include <gtest/gtest.h>

#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

#include "CTPOptLoader/TraderSpi.h"

#include "Includes/LoaderDef.hpp"

#include "TestUnits/coverage/t_CTPOptLoader/r114_ctpopt_api_stub.h"

USING_NS_WTP;
using namespace loader;

/* --- 生产 TU 里的 C 入口与全局 --- */
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

extern TThostFtdcFrontIDType FRONT_ID;
extern TThostFtdcSessionIDType SESSION_ID;
extern TThostFtdcOrderRefType ORDER_REF;

typedef ctp_sopt::CThostFtdcTraderApi* (*R114Creator)(const char*);
extern R114Creator g_ctpCreator;

/* 生产 TU 内联函数（TraderSpi.cpp:74-225）的外部符号 */
ContractCategory wrapCategory(TThostFtdcProductClassType cType);
const char* wrapProductID(const char* rawProductID);

namespace {

class ProbeSpi : public CTraderSpi
{
public:
	int trading_date() const { return m_lTradingDate; }
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

class R114OptFixture : public ::testing::Test
{
protected:
	char _old_cwd[1024];
	std::string _dir;

	void SetUp() override
	{
		ASSERT_NE(getcwd(_old_cwd, sizeof(_old_cwd)), nullptr);
		char tmpl[] = "/tmp/r114_opt_XXXXXX";
		char* made = mkdtemp(tmpl);
		ASSERT_NE(made, nullptr);
		_dir = made;
		ASSERT_EQ(chdir(_dir.c_str()), 0);

		r114_ctpopt::reset();
		_commodities.clear();
		_contracts.clear();
		MAP_NAME.clear();
		MAP_SESSION.clear();
		pUserApi = nullptr;
		g_ctpCreator = nullptr;
		FRONT_ADDR.clear();
		BROKER_ID.clear();
		INVESTOR_ID.clear();
		PASSWORD.clear();
		SAVEPATH.clear();
		APPID.clear();
		AUTHCODE.clear();
		COMM_FILE.clear();
		CONT_FILE.clear();
		MODULE_NAME.clear();
		CLASSMASK = 1 | 2 | 4;
		ONLYINCFG = false;
		INCREMENTAL = false;
		iRequestID = 0;
	}

	void TearDown() override
	{
		chdir(_old_cwd);
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);

		r114_ctpopt::reset();
		_commodities.clear();
		_contracts.clear();
		MAP_NAME.clear();
		MAP_SESSION.clear();
		pUserApi = nullptr;
		g_ctpCreator = nullptr;
		SAVEPATH.clear();
		COMM_FILE.clear();
		CONT_FILE.clear();
		MODULE_NAME.clear();
		iRequestID = 0;
	}

	std::string write(const char* name, const std::string& content)
	{
		std::string path = _dir + "/" + name;
		std::ofstream f(path);
		f << content;
		f.close();
		return path;
	}
};

}	// namespace

/* ------------------------------------------------------------------ */
/* CTPOptLoader.cpp:146-201 —— JSON 文件分支（isFile=true）             */
/* ------------------------------------------------------------------ */
TEST_F(R114OptFixture, run_json_file_branch_reads_all_ctp_and_config_keys)
{
	const std::string cfg = write("opt.json", R"({
		"ctp": {
			"front": "tcp://10.2.3.4:5678",
			"broker": "brk_json",
			"user": "inv_json",
			"pass": "pwd_json",
			"appid": "app-json",
			"authcode": "auth-json",
			"onlyincfg": true,
			"incremental": true
		},
		"config": {
			"mask": 7,
			"commfile": "json_comm.json",
			"contfile": "json_cont.json",
			"mapfiles": "",
			"module": "./json_opt_module.so"
		}
	})");

	ASSERT_EQ(run(cfg.c_str(), true, true), 0);
	EXPECT_EQ(FRONT_ADDR, "tcp://10.2.3.4:5678");
	EXPECT_EQ(BROKER_ID, "brk_json");
	EXPECT_EQ(INVESTOR_ID, "inv_json");
	EXPECT_EQ(PASSWORD, "pwd_json");
	EXPECT_EQ(APPID, "app-json");
	EXPECT_EQ(AUTHCODE, "auth-json");
	EXPECT_EQ(CLASSMASK, 7u);
	EXPECT_EQ(COMM_FILE, "json_comm.json");
	EXPECT_EQ(CONT_FILE, "json_cont.json");
	EXPECT_EQ(MODULE_NAME, "./json_opt_module.so");
	EXPECT_TRUE(ONLYINCFG);
	EXPECT_TRUE(INCREMENTAL);
	// 没有 config.path -> CTPOptLoader.cpp:198-201 的必填项校验在 dlopen 前返回
	EXPECT_TRUE(SAVEPATH.empty());
	EXPECT_EQ(pUserApi, nullptr);
}

/* ------------------------------------------------------------------ */
/* CTPOptLoader.cpp:117-145 —— ini 分支读取显式值                       */
/* ------------------------------------------------------------------ */
TEST_F(R114OptFixture, run_ini_file_branch_reads_explicit_values)
{
	const std::string cfg = write("opt.ini",
		"[ctp]\n"
		"front=tcp://127.0.0.1:19001\n"
		"broker=brk_ini\n"
		"user=inv_ini\n"
		"pass=pwd_ini\n"
		"appid=app-ini\n"
		"authcode=auth-ini\n"
		"[config]\n"
		"mask=3\n"
		"commfile=ini_comm.json\n"
		"contfile=ini_cont.json\n"
		"module=./ini_opt_module.so\n"
		"onlyincfg=true\n"
		"incremental=true\n");

	ASSERT_EQ(run(cfg.c_str(), true, true), 0);
	EXPECT_EQ(FRONT_ADDR, "tcp://127.0.0.1:19001");
	EXPECT_EQ(BROKER_ID, "brk_ini");
	EXPECT_EQ(INVESTOR_ID, "inv_ini");
	EXPECT_EQ(PASSWORD, "pwd_ini");
	EXPECT_EQ(APPID, "app-ini");
	EXPECT_EQ(AUTHCODE, "auth-ini");
	EXPECT_EQ(CLASSMASK, 3u);
	EXPECT_EQ(COMM_FILE, "ini_comm.json");
	EXPECT_EQ(CONT_FILE, "ini_cont.json");
	EXPECT_EQ(MODULE_NAME, "./ini_opt_module.so");
	EXPECT_TRUE(ONLYINCFG);
	EXPECT_TRUE(INCREMENTAL);
	EXPECT_TRUE(SAVEPATH.empty());
	EXPECT_EQ(pUserApi, nullptr);
}

/* ------------------------------------------------------------------ */
/* TraderSpi.cpp:111-178、397-485 —— 登录成功 + LoadFromJson 正/负路径   */
/* ------------------------------------------------------------------ */
TEST_F(R114OptFixture, login_success_fills_session_and_loads_incremental_cache)
{
	r114_ctpopt::state().trading_day = "20261028";
	ProbeSpi spi;

	FRONT_ADDR = "tcp://127.0.0.1:21001";
	BROKER_ID = "brk_login";
	INVESTOR_ID = "inv_login";
	PASSWORD = "pwd_login";
	APPID = "app-login";
	AUTHCODE = "auth-login";
	CLASSMASK = 1 | 2 | 4;
	INCREMENTAL = true;
	SAVEPATH = _dir + "/";
	COMM_FILE = "cache_comm.json";
	CONT_FILE = "cache_cont.json";
	pUserApi = new R114TraderApi();

	write("cache_comm.json", R"({
		"SHFE": {
			"rb": {"name": "Rebar", "category": 1, "volscale": 10, "pricetick": 1.0,
				"covermode": 1, "pricemode": 2, "trademode": 0, "precision": 0},
			"ag": {"name": "Silver", "category": 1, "volscale": 15, "pricetick": 1.0,
				"covermode": 1, "pricemode": 2, "trademode": 0, "precision": 0}
		}
	})");
	write("cache_cont.json", R"({
		"SHFE": {
			"rb2601": {"name": "Rebar2601", "product": "rb", "maxmarketqty": 500,
				"maxlimitqty": 300, "minmarketqty": 1, "minlimitqty": 1,
				"opendate": 20231101, "expiredate": 20241130, "volscale": 10,
				"longmarginratio": 0.09, "shortmarginratio": 0.09},
			"rb2601C3000": {"name": "RebarOpt", "product": "rb", "maxmarketqty": 10,
				"maxlimitqty": 10, "minmarketqty": 1, "minlimitqty": 1,
				"opendate": 20231101, "expiredate": 20241130, "volscale": 10,
				"longmarginratio": 0.09, "shortmarginratio": 0.09,
				"option": {"optiontype": 1, "underlying": "rb2601",
					"underlyingscale": 10.0}}
		}
	})");

	spi.OnFrontConnected();
	EXPECT_EQ(r114_ctpopt::state().auth_reqs, 1);
	EXPECT_EQ(r114_ctpopt::state().broker, "brk_login");
	EXPECT_EQ(r114_ctpopt::state().user, "inv_login");
	EXPECT_EQ(r114_ctpopt::state().appid, "app-login");
	EXPECT_EQ(r114_ctpopt::state().authcode, "auth-login");

	CThostFtdcRspAuthenticateField auth;
	memset(&auth, 0, sizeof(auth));
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	spi.OnRspAuthenticate(&auth, &ok, 1, true);
	EXPECT_EQ(r114_ctpopt::state().login_reqs, 1);
	EXPECT_EQ(r114_ctpopt::state().password, "pwd_login");

	CThostFtdcRspUserLoginField login;
	memset(&login, 0, sizeof(login));
	login.FrontID = 11;
	login.SessionID = 22;
	strncpy(login.MaxOrderRef, "7", sizeof(login.MaxOrderRef) - 1);
	spi.OnRspUserLogin(&login, &ok, 2, true);

	EXPECT_EQ(FRONT_ID, 11);
	EXPECT_EQ(SESSION_ID, 22);
	EXPECT_STREQ(ORDER_REF, "8");
	EXPECT_EQ(spi.trading_date(), 20261028);
	EXPECT_EQ(r114_ctpopt::state().trading_day_calls, 1);
	EXPECT_EQ(r114_ctpopt::state().instrument_reqs, 1);

	// LoadFromJson（:404-436 / :441-483）把两个缓存文件读进全局表
	ASSERT_EQ(_commodities.count("SHFE.rb"), 1u);
	EXPECT_EQ(_commodities["SHFE.rb"].m_strName, "Rebar");
	EXPECT_EQ(_commodities["SHFE.rb"].m_uVolScale, 10u);
	ASSERT_EQ(_contracts.count("SHFE.rb2601"), 1u);
	EXPECT_EQ(_contracts["SHFE.rb2601"].m_strProduct, "rb");
	ASSERT_EQ(_contracts.count("SHFE.rb2601C3000"), 1u);
	EXPECT_EQ(_contracts["SHFE.rb2601C3000"].m_optType, (OptionType)1);
	EXPECT_EQ(_contracts["SHFE.rb2601C3000"].m_strUnderlying, "rb2601");

	// 缓存文件不存在：两条 if(root) 都为假，不崩
	SAVEPATH = _dir + "/no_such_dir/";
	spi.OnRspUserLogin(&login, &ok, 3, true);
	EXPECT_EQ(r114_ctpopt::state().instrument_reqs, 2);

	// 非增量：LoadFromJson 在 :399-400 早退
	INCREMENTAL = false;
	spi.OnRspUserLogin(&login, &ok, 4, true);
	EXPECT_EQ(r114_ctpopt::state().instrument_reqs, 3);

	R114TraderApi* fake = static_cast<R114TraderApi*>(pUserApi);
	pUserApi = nullptr;
	delete fake;
}

/* ------------------------------------------------------------------ */
/* TraderSpi.cpp:228-395 —— 股票/期权/精度/映射/编码分支               */
/* ------------------------------------------------------------------ */
TEST_F(R114OptFixture, query_instrument_covers_stock_option_precision_and_encoding)
{
	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CLASSMASK = 1 | 2 | 4;

	// 股票：ASTOCK -> STK，NoUseHistory -> TM_LongT1，PriceTick 0.01 -> precision 2
	CThostFtdcInstrumentField stock =
		makeInstrument("SSE", "600000", "ASTOCK", "PingAn", THOST_FTDC_PC_Stock);
	stock.PositionDateType = THOST_FTDC_PDT_NoUseHistory;
	stock.PriceTick = 0.01;
	spi.OnRspQryInstrument(&stock, &ok, 1, false);
	ASSERT_EQ(_commodities.count("SSE.STK"), 1u);
	EXPECT_EQ(_commodities["SSE.STK"].m_ccCategory, CC_Stock);
	EXPECT_EQ(_commodities["SSE.STK"].m_tradeMode, TM_LongT1);
	EXPECT_EQ(_commodities["SSE.STK"].m_uPrecision, 2u);

	// 另一只股票：非 NoUseHistory -> TM_Long；PriceTick 0.0001 -> precision 4
	CThostFtdcInstrumentField stock2 =
		makeInstrument("SSE", "000001", "ASTOCK2", "NSBank", THOST_FTDC_PC_Stock);
	stock2.PositionDateType = THOST_FTDC_PDT_UseHistory;
	stock2.PriceTick = 0.0001;
	stock2.LongMarginRatio = DBL_MAX;
	stock2.ShortMarginRatio = FLT_MAX;
	spi.OnRspQryInstrument(&stock2, &ok, 2, false);
	ASSERT_EQ(_commodities.count("SSE.ASTOCK2"), 1u);
	EXPECT_EQ(_commodities["SSE.ASTOCK2"].m_tradeMode, TM_Long);
	EXPECT_EQ(_commodities["SSE.ASTOCK2"].m_uPrecision, 4u);
	EXPECT_DOUBLE_EQ(_contracts["SSE.000001"].m_dLongMarginRatio, 0.0);
	EXPECT_DOUBLE_EQ(_contracts["SSE.000001"].m_dShortMarginRatio, 0.0);

	// 期权：ASTOCK_O -> STKO，已映射品种走 !bFuture 的 cname=InstrumentName 分支
	MAP_NAME["STKO"] = "StockOpt";
	CThostFtdcInstrumentField spot_opt =
		makeInstrument("SSE", "10000001", "ASTOCK_O", "StockOpt", THOST_FTDC_PC_SpotOption);
	spot_opt.VolumeMultiple = 0;
	spot_opt.OptionsType = 2;
	spot_opt.PriceTick = 0.005;
	spi.OnRspQryInstrument(&spot_opt, &ok, 3, false);
	ASSERT_EQ(_commodities.count("SSE.STKO"), 1u);
	EXPECT_EQ(_commodities["SSE.STKO"].m_ccCategory, CC_SpotOption);
	EXPECT_EQ(_commodities["SSE.STKO"].m_uPrecision, 3u);
	EXPECT_EQ(_commodities["SSE.STKO"].m_strName, "StockOpt");
	EXPECT_EQ(_contracts["SSE.10000001"].m_uVolScale, 1u);

	// 期权：ETF_O -> ETFO，未映射品种自动用厂商合约名；VolumeMultiple 100
	CThostFtdcInstrumentField etf_opt =
		makeInstrument("SSE", "10000002", "ETF_O", "ETF-Opt", THOST_FTDC_PC_ETFOption);
	etf_opt.VolumeMultiple = 100;
	etf_opt.PriceTick = 0.05;
	spi.OnRspQryInstrument(&etf_opt, &ok, 4, false);
	ASSERT_EQ(_commodities.count("SSE.ETFO"), 1u);
	EXPECT_EQ(_commodities["SSE.ETFO"].m_ccCategory, CC_ETFOption);
	EXPECT_EQ(_commodities["SSE.ETFO"].m_uPrecision, 2u);
	EXPECT_EQ(_contracts["SSE.10000002"].m_uVolScale, 100u);

	// 期货：GBK 品种名 -> :292/:295 的 ChartoUTF8 分支
	MAP_NAME["SHFE.rb"] = "\xD6\xD0\xCE\xC4";
	CThostFtdcInstrumentField gbk_future =
		makeInstrument("SHFE", "rb2601", "rb", "\xD6\xD0\xCE\xC4", THOST_FTDC_PC_Futures);
	gbk_future.PriceTick = 1.0;
	spi.OnRspQryInstrument(&gbk_future, &ok, 5, false);
	ASSERT_EQ(_contracts.count("SHFE.rb2601"), 1u);
	EXPECT_FALSE(_contracts["SHFE.rb2601"].m_strName.empty());
	ASSERT_EQ(_commodities.count("SHFE.rb"), 1u);
	EXPECT_FALSE(_commodities["SHFE.rb"].m_strName.empty());

	// 期货：PriceTick 0.5 -> precision 1（既有测试同型，这里保持独立）
	CThostFtdcInstrumentField dce_future =
		makeInstrument("DCE", "i2605", "i", "IronOre2605", THOST_FTDC_PC_Futures);
	dce_future.PriceTick = 0.5;
	spi.OnRspQryInstrument(&dce_future, &ok, 6, false);
	EXPECT_EQ(_commodities["DCE.i"].m_uPrecision, 1u);
}

/* ------------------------------------------------------------------ */
/* TraderSpi.cpp:180-225 —— 类别/品种换算的其余枚举                     */
/* ------------------------------------------------------------------ */
TEST(R114OptCodes, wrap_category_and_product_id_cover_all_defined_values)
{
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_Futures), CC_Future);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_Options), CC_FutOption);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_Combination), CC_Combination);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_Spot), CC_Spot);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_EFP), CC_EFP);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_SpotOption), CC_SpotOption);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_ETFOption), CC_ETFOption);
	EXPECT_EQ(wrapCategory(THOST_FTDC_PC_Stock), CC_Stock);
	EXPECT_THROW(wrapCategory((TThostFtdcProductClassType)99), std::runtime_error);

	EXPECT_STREQ(wrapProductID("ASTOCK"), "STK");
	EXPECT_STREQ(wrapProductID("ASTOCK_O"), "STKO");
	EXPECT_STREQ(wrapProductID("ETF_O"), "ETFO");
	EXPECT_STREQ(wrapProductID("rb"), "rb");
}

/* ------------------------------------------------------------------ */
/* TraderSpi.cpp:390-394、488-600 —— 最后一包落盘 + exit(0)             */
/* ------------------------------------------------------------------ */
TEST_F(R114OptFixture, dump_to_json_writes_two_files_and_exits_zero)
{
	SAVEPATH = _dir + "/";
	COMM_FILE = "r114_comm.json";
	CONT_FILE = "r114_cont.json";

	CTraderSpi spi;
	CThostFtdcRspInfoField ok = makeRsp(0, "");
	CLASSMASK = 1 | 2 | 4;

	// 同一交易所两个品种 -> DumpToJson 的 !HasMember 两分支
	CThostFtdcInstrumentField rb =
		makeInstrument("SHFE", "rb2601", "rb", "Rebar2601", THOST_FTDC_PC_Futures);
	spi.OnRspQryInstrument(&rb, &ok, 1, false);
	CThostFtdcInstrumentField ag =
		makeInstrument("SHFE", "ag2601", "ag", "Silver2601", THOST_FTDC_PC_Futures);
	spi.OnRspQryInstrument(&ag, &ok, 2, false);
	// 另一交易所的期权 -> option 块 + volscale 非 0 分支
	CThostFtdcInstrumentField opt =
		makeInstrument("DCE", "m2601-C-3000", "m_o", "M-Opt", THOST_FTDC_PC_Options);
	opt.OptionsType = 1;
	opt.VolumeMultiple = 5;
	strncpy(opt.UnderlyingInstrID, "m2601", sizeof(opt.UnderlyingInstrID) - 1);
	opt.StrikePrice = 3000.0;
	opt.UnderlyingMultiple = 5.0;
	spi.OnRspQryInstrument(&opt, &ok, 3, false);

	// bIsLast=true -> DumpToJson() + exit(0)（BUG-T4-015）。死亡测试把它关在子进程里，
	// 父进程继续断言两个 JSON 文件确实写出。
	EXPECT_EXIT(spi.OnRspQryInstrument(&opt, &ok, 4, true),
		::testing::ExitedWithCode(0), "");

	std::ifstream cf(_dir + "/r114_comm.json");
	std::string comms((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
	std::ifstream tf(_dir + "/r114_cont.json");
	std::string conts((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());

	ASSERT_FALSE(comms.empty());
	ASSERT_FALSE(conts.empty());
	EXPECT_NE(comms.find("\"SHFE\""), std::string::npos);
	EXPECT_NE(comms.find("\"rb\""), std::string::npos);
	EXPECT_NE(comms.find("\"ag\""), std::string::npos);
	EXPECT_NE(comms.find("\"DCE\""), std::string::npos);
	EXPECT_NE(comms.find("\"m_o\""), std::string::npos);
	EXPECT_NE(conts.find("\"rb2601\""), std::string::npos);
	EXPECT_NE(conts.find("\"m2601-C-3000\""), std::string::npos);
	EXPECT_NE(conts.find("\"option\""), std::string::npos);
	EXPECT_NE(conts.find("\"volscale\""), std::string::npos);
}
