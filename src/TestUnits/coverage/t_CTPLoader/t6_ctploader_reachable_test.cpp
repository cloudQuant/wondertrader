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
 *  - std::cerr 重定向到内存是本文件唯一的可观察通道：胶水层所有回报都走 std::cerr，
 *    所以「有没有打印某一行」就是可证伪的断言。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "CTPLoader/TraderSpi.h"

USING_NS_WTP;

/// 生产入口：src/CTPLoader/CTPLoader.cpp L71（extern "C" 声明）/ L76（定义）
extern "C" int run(const char* cfgfile, bool bAsync, bool isFile);

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
	CoutCapture cap;
	// isFile=false → load_from_content("", yaml) → 空文档得到 null 根 → L87-88 return 0
	EXPECT_EQ(0, run("", false, false));
	// 一旦防护失效往下走，厂商装载/查询日志必然出现在 cerr 上
	EXPECT_TRUE(cap.text().empty()) << cap.text();
}

TEST(t6_ctploader_run, inline_yaml_without_savepath_stops_at_required_field_guard)
{
	CoutCapture cap;
	// 覆盖 L84-122 内联配置分支全部字段读取（含 L113-120 的 module 缺省名兜底）
	// SAVEPATH 为空 → L201-204 返回 0，绝不创建厂商 API
	EXPECT_EQ(0, run(kInlineYamlWithoutSavePath, false, false));
	EXPECT_TRUE(cap.text().empty()) << cap.text();
}

TEST(t6_ctploader_run, unparsable_inline_yaml_reports_failure_without_throwing)
{
	// WTSCfgLoader 文档约定解析失败返回 NULL；run() 对 NULL 根节点以 0 报告失败。
	// 非法 YAML 应遵守同一失败协议，异常不得穿过 C ABI 入口。
	int result = -1;
	EXPECT_NO_THROW(result = run("[unterminated", false, false));
	EXPECT_EQ(0, result);
}

TEST(t6_ctploader_run, missing_config_files_are_rejected_for_both_layouts)
{
	CoutCapture cap;
	// JSON 分支：文件不存在 → load_from_file 返回 NULL → L159-160 return 0
	EXPECT_EQ(0, run("/tmp/t6_ctploader_no_such_cfg.json", false, true));
	// ini 分支：IniHelper::load 吞掉「文件不存在」，全部取默认空串
	// → MODULE_NAME 走 ./thosttraderapi_se.so 默认 + L196-199 traders/ 拼接 → 守卫返回 0
	EXPECT_EQ(0, run("/tmp/t6_ctploader_no_such_file.ini", false, true));
	EXPECT_TRUE(cap.text().empty()) << cap.text();
}

TEST(t6_ctploader_run, json_file_with_ctp_and_config_stops_at_savepath_guard)
{
	TempCfgFile f("t6_ctp_loader_cfg.json",
		"{\"ctp\":{\"front\":\"tcp://127.0.0.1:1\",\"broker\":\"9999\",\"user\":\"88888\","
		"\"pass\":\"secret\",\"authcode\":\"ac\"},"
		"\"config\":{\"mask\":7,\"commfile\":\"commodities.json\",\"contfile\":\"contracts.json\","
		"\"feesfile\":\"fees.json\",\"onlyincfg\":false,\"incremental\":false,\"qryfees\":false}}");
	CoutCapture cap;

	// 覆盖 L156-194（JSON 文件分支，含 L184-192 module 缺省名兜底）
	// config 段没有 path → 必须在必填项守卫返回 0
	EXPECT_EQ(0, run(f.c_str(), false, true));
	EXPECT_TRUE(cap.text().empty()) << cap.text();
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
	CoutCapture cap;

	// 覆盖 L124-155 全部 ini 读取（含 onlyincfg/incremental 的 true 侧）
	// 缺 path → 仍必须在守卫处返回 0，不得进入厂商装载
	EXPECT_EQ(0, run(f.c_str(), false, true));
	EXPECT_TRUE(cap.text().empty()) << cap.text();
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
	CoutCapture cap;

	EXPECT_EQ(0, run(f.c_str(), false, true));
	EXPECT_TRUE(cap.text().empty()) << cap.text();
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

// 配置里没有 ctp 段时 root->get("ctp") 返回 NULL，下一行直接 getCString（L90-91 / L162-163）。
TEST(DISABLED_t6_ctploader_run_crash, inline_config_without_ctp_section_dereferences_null)
{
	CoutCapture cap;
	run("other:\n  a: 1\n", false, false);
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

// extractProductName("") 里 `auto idx = strlen(cname) - 1` 得到 size_t 的 -1，
// 随后 cname[idx] 越界读（L98-109）→ 崩溃路径，只记录不执行。
TEST(DISABLED_t6_ctp_loader_names_crash, extract_product_name_on_empty_string_reads_out_of_bounds)
{
	volatile size_t n = extractProductName("").size();
	(void)n;
	SUCCEED();
}
