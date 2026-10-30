/*
 * lane T6（收发族第六波）— CTPLoader 可直达面用例
 *
 * 目标文件：src/CTPLoader/CTPLoader.cpp（290 行）+ src/CTPLoader/TraderSpi.cpp（867 行）
 *
 * 可达性口径（重要，避免假装覆盖）：
 *  - 本模块是「装载入口」形态：唯一对外入口是 extern "C" int run(cfgfile, bAsync, isFile)。
 *    配置来源三条分支：内联 YAML（isFile=false）、*.ini、其它（JSON/YAML 文件）。
 *  - 三条分支最终都会在 L256-288 落到厂商实现（__APPLE__ 下直接取
 *    CThostFtdcTraderApi::CreateFtdcTraderApi，再 RegisterFront/Init/Join），会起厂商线程
 *    并阻塞在 Join。因此本文件只进入「必将在 L201-204 必填项守卫处返回 0」的配置组合：
 *    任何一次 run() 调用都不创建 CTraderSpi、不碰厂商 API、不起线程。
 *  - TraderSpi 侧：pUserApi 是 CTPLoader.cpp L27 的全局指针，未成功 run() 前恒为 NULL，
 *    所以 OnFrontConnected / 成功分支的 OnRspUserLogin / ReqQryInstrument 都会解引用空指针；
 *    OnRspQryInstrument 在 bIsLast=true 时以 DumpToJson()+exit(0) 收尾（L542-548），
 *    会直接杀掉宿主进程 → 本文件一律用 bIsLast=false 且带非零 ErrorID 调用，只覆盖守卫。
 *    合约/费率映射主体（L348-530、L550-594）依赖 CLASSMASK/MAP_NAME/ONLYINCFG/QRYFEES 全局，
 *    且没有 fake 柜台接缝，本轮未进入，见 manifests 与 bugs 文档。
 *  - CoutCapture 只捕获 TraderSpi 写入 std::cerr 的消息；run() 中的 printf 写入 stdout。
 *    配置入口用返回值与全局状态断言，不用 stderr 捕获结果推断 stdout 是否输出。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "CTPLoader/TraderSpi.h"

USING_NS_WTP;

/// 生产入口：src/CTPLoader/CTPLoader.cpp L71（extern "C" 声明）/ L76（定义）
extern "C" int run(const char* cfgfile, bool bAsync, bool isFile);

extern std::string FRONT_ADDR;
extern std::string BROKER_ID;
extern std::string INVESTOR_ID;
extern std::string PASSWORD;
extern std::string SAVEPATH;
extern std::string APPID;
extern std::string AUTHCODE;
extern uint32_t CLASSMASK;
extern bool ONLYINCFG;
extern bool INCREMENTAL;
extern bool QRYFEES;
extern std::string COMM_FILE;
extern std::string CONT_FILE;
extern std::string FEES_FILE;
extern std::string MODULE_NAME;
extern std::string FEE_FILTERS;
extern std::map<std::string, std::string> MAP_NAME;
extern std::map<std::string, std::string> MAP_SESSION;
extern std::set<std::string> SET_FILTERS;

/// 生产自由函数：src/CTPLoader/TraderSpi.cpp L85-96 / L98-109
std::string extractProductID(const char* instrument);
std::string extractProductName(const char* cname);

namespace
{
	/// 把 std::cerr 接进内存：厂商胶水层的日志就是这个模块的可观察输出
	class CoutCapture
	{
	public:
		CoutCapture() : _old(std::cerr.rdbuf()) { std::cerr.rdbuf(_ss.rdbuf()); }
		~CoutCapture() { std::cerr.rdbuf(_old); }

		std::string text() const { return _ss.str(); }

	private:
		std::ostringstream _ss;
		std::streambuf* _old;
	};

	/// 临时配置文件：只落在系统临时目录，析构即删
	class TempCfgFile
	{
	public:
		TempCfgFile(const char* name, const std::string& content)
		{
			_path = std::filesystem::temp_directory_path() / std::filesystem::path(name);
			_cstr = _path.string();
			std::ofstream ofs(_cstr);
			ofs << content;
			ofs.close();
		}

		~TempCfgFile()
		{
			std::error_code ec;
			std::filesystem::remove(_path, ec);
		}

		const char* c_str() const { return _cstr.c_str(); }

	private:
		std::filesystem::path _path;
		std::string _cstr;
	};

	struct ConfigState
	{
		std::string front = FRONT_ADDR;
		std::string broker = BROKER_ID;
		std::string investor = INVESTOR_ID;
		std::string password = PASSWORD;
		std::string savepath = SAVEPATH;
		std::string appid = APPID;
		std::string authcode = AUTHCODE;
		uint32_t classmask = CLASSMASK;
		bool onlyincfg = ONLYINCFG;
		bool incremental = INCREMENTAL;
		bool qryfees = QRYFEES;
		std::string commFile = COMM_FILE;
		std::string contFile = CONT_FILE;
		std::string feesFile = FEES_FILE;
		std::string module = MODULE_NAME;
		std::string feeFilters = FEE_FILTERS;
		std::map<std::string, std::string> mapName = MAP_NAME;
		std::map<std::string, std::string> mapSession = MAP_SESSION;
		std::set<std::string> filters = SET_FILTERS;
	};

	void set_config_sentinel()
	{
		FRONT_ADDR = "sentinel-front";
		BROKER_ID = "sentinel-broker";
		INVESTOR_ID = "sentinel-user";
		PASSWORD = "sentinel-password";
		SAVEPATH = "sentinel-path";
		APPID = "sentinel-appid";
		AUTHCODE = "sentinel-authcode";
		CLASSMASK = 0x5a5a;
		ONLYINCFG = true;
		INCREMENTAL = true;
		QRYFEES = true;
		COMM_FILE = "sentinel-commodities.json";
		CONT_FILE = "sentinel-contracts.json";
		FEES_FILE = "sentinel-fees.json";
		MODULE_NAME = "sentinel-module";
		FEE_FILTERS = "sentinel-filter";
		MAP_NAME.clear();
		MAP_NAME["sentinel-name"] = "sentinel-value";
		MAP_SESSION.clear();
		MAP_SESSION["sentinel-session"] = "sentinel-value";
		SET_FILTERS.clear();
		SET_FILTERS.insert("sentinel-filter");
	}

	void expect_config_state_unchanged(const ConfigState& before)
	{
		EXPECT_EQ(before.front, FRONT_ADDR);
		EXPECT_EQ(before.broker, BROKER_ID);
		EXPECT_EQ(before.investor, INVESTOR_ID);
		EXPECT_EQ(before.password, PASSWORD);
		EXPECT_EQ(before.savepath, SAVEPATH);
		EXPECT_EQ(before.appid, APPID);
		EXPECT_EQ(before.authcode, AUTHCODE);
		EXPECT_EQ(before.classmask, CLASSMASK);
		EXPECT_EQ(before.onlyincfg, ONLYINCFG);
		EXPECT_EQ(before.incremental, INCREMENTAL);
		EXPECT_EQ(before.qryfees, QRYFEES);
		EXPECT_EQ(before.commFile, COMM_FILE);
		EXPECT_EQ(before.contFile, CONT_FILE);
		EXPECT_EQ(before.feesFile, FEES_FILE);
		EXPECT_EQ(before.module, MODULE_NAME);
		EXPECT_EQ(before.feeFilters, FEE_FILTERS);
		EXPECT_EQ(before.mapName, MAP_NAME);
		EXPECT_EQ(before.mapSession, MAP_SESSION);
		EXPECT_EQ(before.filters, SET_FILTERS);
	}

	void expect_rejected_without_mutation(const char* cfgfile, bool isFile)
	{
		set_config_sentinel();
		const ConfigState before;
		int result = -1;
		EXPECT_NO_THROW(result = run(cfgfile, false, isFile));
		EXPECT_EQ(0, result);
		expect_config_state_unchanged(before);
	}

	CThostFtdcRspInfoField make_rsp(int errId, const char* msg = "unit-test-error")
	{
		CThostFtdcRspInfoField rsp;
		std::memset(&rsp, 0, sizeof(rsp));
		rsp.ErrorID = errId;
		if (msg != NULL)
			snprintf(rsp.ErrorMsg, sizeof(rsp.ErrorMsg), "%s", msg);
		return rsp;
	}

	// 内联 YAML：config 段故意不给 path → SAVEPATH 为空 → 必在 L201-204 守卫返回 0
	const char* kInlineYamlWithoutSavePath =
		"ctp:\n"
		"  front: tcp://127.0.0.1:1\n"
		"  broker: \"9999\"\n"
		"  user: \"88888\"\n"
		"  pass: \"secret\"\n"
		"  appid: \"app-t6\"\n"
		"config:\n"
		"  mask: 7\n";
}
/* ------------------------------------------------------------------ */
/* run()：三种配置来源 + 空值/必填守卫                                    */
/* ------------------------------------------------------------------ */

TEST(t6_ctploader_run, empty_inline_content_is_rejected_before_any_loading)
{
	// 空 YAML 根返回 0，且配置解析失败不能改写已有全局配置。
	expect_rejected_without_mutation("", false);
}

TEST(t6_ctploader_run, inline_yaml_without_savepath_leaves_global_state_unchanged)
{
	// 配置结构正确但缺必填 path；失败时必须在提交全局状态前返回。
	expect_rejected_without_mutation(kInlineYamlWithoutSavePath, false);
}

TEST(t6_ctploader_run, unparsable_inline_yaml_reports_failure_without_throwing)
{
	// 非法 YAML 遵守原有失败协议，且解析失败不得部分改写现有配置。
	expect_rejected_without_mutation("[unterminated", false);
}

TEST(t6_ctploader_config, invalid_inline_yaml_roots_and_sections_leave_global_state_unchanged)
{
	expect_rejected_without_mutation("- item\n", false); // root sequence
	expect_rejected_without_mutation("config:\n  mask: 7\n", false); // missing ctp
	expect_rejected_without_mutation("ctp: scalar\nconfig:\n  mask: 7\n", false); // wrong ctp type
	expect_rejected_without_mutation("ctp: {}\nconfig: scalar\n", false); // wrong config type
	expect_rejected_without_mutation("ctp: {}\n", false); // missing config
}

TEST(t6_ctploader_config, invalid_json_roots_and_sections_leave_global_state_unchanged)
{
	{
		TempCfgFile f("t6_ctp_loader_bad_root.json", "[]");
		expect_rejected_without_mutation(f.c_str(), true);
	}
	{
		TempCfgFile f("t6_ctp_loader_missing_ctp.json", "{\"config\":{\"mask\":7}}");
		expect_rejected_without_mutation(f.c_str(), true);
	}
	{
		TempCfgFile f("t6_ctp_loader_wrong_config.json", "{\"ctp\":{},\"config\":[]}");
		expect_rejected_without_mutation(f.c_str(), true);
	}
}

TEST(t6_ctploader_config, invalid_ini_sections_and_parse_leave_global_state_unchanged)
{
	{
		TempCfgFile f("t6_ctp_loader_missing_ctp.ini", "[config]\nmask=7\n");
		expect_rejected_without_mutation(f.c_str(), true);
	}
	{
		TempCfgFile f("t6_ctp_loader_missing_config.ini", "[ctp]\nfront=tcp://127.0.0.1:1\n");
		expect_rejected_without_mutation(f.c_str(), true);
	}
	{
		TempCfgFile f("t6_ctp_loader_bad_ini.ini", "not-an-ini-section\n");
		expect_rejected_without_mutation(f.c_str(), true);
	}
}

TEST(t6_ctploader_run, missing_config_files_are_rejected_for_both_layouts)
{
	// 不存在的 JSON/INI 文件返回 0，且不得触及配置全局状态。
	expect_rejected_without_mutation("/tmp/t6_ctploader_no_such_cfg.json", true);
	expect_rejected_without_mutation("/tmp/t6_ctploader_no_such_file.ini", true);
}

TEST(t6_ctploader_run, json_file_with_ctp_and_config_without_path_leaves_state_unchanged)
{
	TempCfgFile f("t6_ctp_loader_cfg.json",
		"{\"ctp\":{\"front\":\"tcp://127.0.0.1:1\",\"broker\":\"9999\",\"user\":\"88888\","
		"\"pass\":\"secret\",\"authcode\":\"ac\"},"
		"\"config\":{\"mask\":7}}");
	// 结构正确但缺 path；在全局提交前拒绝。
	expect_rejected_without_mutation(f.c_str(), true);
}

TEST(t6_ctploader_run, ini_with_complete_ctp_but_no_savepath_is_rejected)
{
	TempCfgFile f("t6_ctp_loader_full.ini",
		"[ctp]\n"
		"front=tcp://127.0.0.1:1\n"
		"broker=9999\n"
		"user=88888\n"
		"pass=secret\n"
		"appid=app-t6\n"
		"authcode=ac-t6\n"
		"[config]\n"
		"mask=7\n"
		"onlyincfg=true\n"
		"incremental=true\n"
		"qryfees=false\n"
		"feefilter=rb,ao\n"
		"commfile=commodities.json\n"
		"contfile=contracts.json\n"
		"feesfile=fees.json\n");
	// Configuration parsing remains offline; missing path is rejected before global commit.
	expect_rejected_without_mutation(f.c_str(), true);
}

TEST(t6_ctploader_run, ini_with_empty_front_is_rejected_although_savepath_present)
{
	// 反向组合：有 path、front 为空 → 守卫的另一侧，证明必填检查真的在看 FRONT_ADDR 而不只看 SAVEPATH
	TempCfgFile f("t6_ctp_loader_no_front.ini",
		"[ctp]\n"
		"front=\n"
		"broker=9999\n"
		"user=88888\n"
		"pass=secret\n"
		"[config]\n"
		"path=/tmp/t6_ctp_loader_out\n");
	// Required front is empty despite path being present; reject without committing config.
	expect_rejected_without_mutation(f.c_str(), true);
}

/* ------------------------------------------------------------------ */
/* TraderSpi：可直接调用的纯函数                                         */
/* ------------------------------------------------------------------ */

TEST(t6_ctp_loader_names, extract_product_id_stops_at_first_non_letter)
{
	// L85-96：逐字符取字母段，遇到数字即止
	EXPECT_EQ("ag", extractProductID("ag2512"));
	EXPECT_EQ("IF", extractProductID("IF2509"));
	EXPECT_EQ("m", extractProductID("m"));
	EXPECT_EQ("rb", extractProductID("rb2510"));
	EXPECT_EQ("", extractProductID("2512"));
}

TEST(t6_ctp_loader_names, extract_product_name_trims_trailing_month_digits)
{
	// L98-109：从尾部回退掉数字得到品种名；注意 `idx > 0` 让纯数字代码至少留下 1 个字符
	EXPECT_EQ("Silver", extractProductName("Silver2512"));
	EXPECT_EQ("abc", extractProductName("abc"));
	EXPECT_EQ("ab", extractProductName("ab1"));
	EXPECT_EQ("a1x", extractProductName("a1x"));	// 尾部不是数字时原样返回
	EXPECT_EQ("2", extractProductName("2512"));	// 全数字时因 idx>0 提前停止
}

TEST(t6_ctp_loader_names, extract_product_name_handles_empty_null_and_signed_bytes)
{
	EXPECT_EQ("", extractProductName(""));
	EXPECT_EQ("", extractProductName(NULL));
	const char signed_byte_name[] = { static_cast<char>(0xFF), '1', '\0' };
	EXPECT_EQ(std::string(1, static_cast<char>(0xFF)), extractProductName(signed_byte_name));
}

/* ------------------------------------------------------------------ */
/* TraderSpi：回调守卫（一律 bIsLast=false 或带非零 ErrorID）             */
/* ------------------------------------------------------------------ */

TEST(t6_ctp_loader_spi, front_disconnected_is_reported_on_stderr)
{
	CTraderSpi spi;
	CoutCapture cap;

	// L847-851：断连只打日志，不碰 pUserApi
	spi.OnFrontDisconnected(-5);
	const std::string out = cap.text();
	EXPECT_NE(std::string::npos, out.find("OnFrontDisconnected")) << out;
	EXPECT_NE(std::string::npos, out.find("-5")) << out;
}

TEST(t6_ctp_loader_spi, rsp_error_prints_detail_only_for_real_errors)
{
	CThostFtdcRspInfoField err = make_rsp(32, "invalid instruction");
	CThostFtdcRspInfoField ok = make_rsp(0, NULL);

	{
		CTraderSpi spi;
		CoutCapture cap;
		spi.OnRspError(&err, 1, true);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("OnRspError")) << out;
		// IsErrorRspInfo（L859-866）只对 ErrorID != 0 打印明细
		EXPECT_NE(std::string::npos, out.find("ErrorID=32")) << out;
	}
	{
		CTraderSpi spi;
		CoutCapture cap;
		spi.OnRspError(&ok, 2, true);
		EXPECT_EQ(std::string::npos, cap.text().find("ErrorID=")) << cap.text();
	}
	{
		// IsErrorRspInfo 有 (pRspInfo) 短路防护：空指针不应崩
		CTraderSpi spi;
		CoutCapture cap;
		spi.OnRspError(NULL, 3, true);
		EXPECT_NE(std::string::npos, cap.text().find("OnRspError")) << cap.text();
		EXPECT_EQ(std::string::npos, cap.text().find("ErrorID=")) << cap.text();
	}
}

TEST(t6_ctp_loader_spi, authenticate_responses_never_reach_login_when_guarded)
{
	CThostFtdcRspInfoField err = make_rsp(200, "auth failed");
	CThostFtdcRspInfoField ok = make_rsp(0, NULL);

	{
		CTraderSpi spi;
		CoutCapture cap;
		// L138-145：bIsLast=true 但错误应答 → 不得调 ReqUserLogin（它会解引用空的 pUserApi）
		spi.OnRspAuthenticate(NULL, &err, 7, true);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("OnRspAuthenticate")) << out;
		EXPECT_NE(std::string::npos, out.find("ErrorID=200")) << out;
		EXPECT_EQ(std::string::npos, out.find("Requesting")) << out;
	}
	{
		CTraderSpi spi;
		CoutCapture cap;
		// bIsLast=false → 连 IsErrorRspInfo 都不该被调用（无 ErrorID 打印、无后续请求）
		spi.OnRspAuthenticate(NULL, &ok, 8, false);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("OnRspAuthenticate")) << out;
		EXPECT_EQ(std::string::npos, out.find("Requesting")) << out;
	}
}

TEST(t6_ctp_loader_spi, login_failure_response_is_not_treated_as_success)
{
	CThostFtdcRspInfoField err = make_rsp(64, "not authorized");
	CTraderSpi spi;
	CoutCapture cap;

	// L158-177：错误应答必须被守卫挡住，否则会写 FRONT_ID 并解引用 pUserApi(NULL)
	spi.OnRspUserLogin(NULL, &err, 9, true);
	const std::string out = cap.text();
	EXPECT_NE(std::string::npos, out.find("OnRspUserLogin")) << out;
	EXPECT_NE(std::string::npos, out.find("ErrorID=64")) << out;
	EXPECT_EQ(std::string::npos, out.find("Requesting")) << out;
}

TEST(t6_ctp_loader_spi, instrument_responses_skip_body_and_never_reach_exit)
{
	CTraderSpi spi;
	{
		// bIsLast 必须为 false：L542-548 在 bIsLast && !QRYFEES 时直接 exit(0) 终结宿主进程
		CThostFtdcRspInfoField err = make_rsp(110, "no data");
		CThostFtdcInstrumentField inst;
		std::memset(&inst, 0, sizeof(inst));
		snprintf(inst.InstrumentID, sizeof(inst.InstrumentID), "ag2512");
		snprintf(inst.ExchangeID, sizeof(inst.ExchangeID), "SHFE");

		CoutCapture cap;
		spi.OnRspQryInstrument(&inst, &err, 10, false);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("ErrorID=110")) << out;
		// 错误应答不得进入合约映射主体
		EXPECT_EQ(std::string::npos, out.find("already exists")) << out;
		EXPECT_EQ(std::string::npos, out.find("Name confirmed automatically")) << out;
	}
	{
		CThostFtdcRspInfoField ok = make_rsp(0, NULL);
		CoutCapture cap;
		// 非错误应答 + pInstrument==NULL → L345/L348 两层防护命中，主体一行都不执行
		spi.OnRspQryInstrument(NULL, &ok, 11, false);
		EXPECT_TRUE(cap.text().empty()) << cap.text();
	}
}

TEST(t6_ctp_loader_spi, fee_and_margin_error_responses_are_skipped_before_deref)
{
	CThostFtdcRspInfoField err = make_rsp(404, "fee not found");
	CTraderSpi spi;

	{
		CoutCapture cap;
		// L550-556：pInstrumentCommissionRate 的解引用只在 !IsErrorRspInfo 分支里
		spi.OnRspQryInstrumentCommissionRate(NULL, &err, 12, true);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("ErrorID=404")) << out;
	}
	{
		CoutCapture cap;
		// L578-582：同上（保证金率）；_queries 为空且 QRYFEES=false ⇒ 不会走 exit(0)
		spi.OnRspQryInstrumentMarginRate(NULL, &err, 13, true);
		const std::string out = cap.text();
		EXPECT_NE(std::string::npos, out.find("ErrorID=404")) << out;
	}
}

/* ------------------------------------------------------------------ */
/* 明确不进入的路径（整条 DISABLED_，理由见注释）                          */
/* ------------------------------------------------------------------ */

// pUserApi 未装载就是 NULL；OnFrontConnected → ReqAuth → pUserApi->ReqAuthenticate 空指针崩溃。
TEST(DISABLED_t6_ctploader_spi_crash, on_front_connected_dereferences_null_user_api)
{
	CTraderSpi spi;
	CoutCapture cap;
	spi.OnFrontConnected();
	(void)cap;
}

// bIsLast=true 且 QRYFEES=false 时 OnRspQryInstrument 以 DumpToJson()+exit(0) 收尾（L542-548），
// 作为独立工具体检是「设计如此」，被任何宿主进程调用会把进程直接杀掉。
TEST(DISABLED_t6_ctploader_spi_exit, instrument_last_response_calls_exit_zero)
{
	CThostFtdcRspInfoField ok = make_rsp(0, NULL);
	CTraderSpi spi;
	CoutCapture cap;
	spi.OnRspQryInstrument(NULL, &ok, 14, true);
	(void)cap;
}

// 五个必填项齐全后 run() 会调用厂商 CreateFtdcTraderApi + RegisterFront + Init(+Join)，
// 起厂商线程并连前置；按门禁规则（不连柜台、不起线程）整条禁用。
TEST(DISABLED_t6_ctploader_run_net, complete_config_reaches_vendor_api_init)
{
	CoutCapture cap;
	const char* full =
		"ctp:\n"
		"  front: tcp://127.0.0.1:1\n"
		"  broker: \"9999\"\n"
		"  user: \"88888\"\n"
		"  pass: \"secret\"\n"
		"config:\n"
		"  path: /tmp/t6_ctp_loader_out\n"
		"  mask: 7\n";
	(void)run(full, true, false);
	(void)cap;
}
