// L01 (lane Share/WTSUtils): CodeHelper.hpp coverage and regression tests.
#include <gtest/gtest.h>

#include "Share/CodeHelper.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

void setField(char* dst, std::size_t cap, const char* src)
{
	(void)std::snprintf(dst, cap, "%s", src);
}

const char* exchgOf(const CodeHelper::CodeInfo& i) { return i._exchg; }

} // namespace

// ---------------------------------------------------------------- CodeInfo
TEST(l01_codehelper, codeinfo_default_all_zero)
{
	CodeHelper::CodeInfo info;
	EXPECT_EQ(info._exright, 0);
	EXPECT_EQ(std::strlen(info._code), 0u);
	EXPECT_EQ(std::strlen(info._exchg), 0u);
	EXPECT_EQ(std::strlen(info._product), 0u);
	EXPECT_FALSE(info.hasRule());
	EXPECT_FALSE(info.isExright());
	EXPECT_FALSE(info.isForwardAdj());
	EXPECT_FALSE(info.isBackwardAdj());
}

TEST(l01_codehelper, codeinfo_exright_flags)
{
	CodeHelper::CodeInfo info;

	info._exright = 0;
	EXPECT_FALSE(info.isExright());
	EXPECT_FALSE(info.isForwardAdj());
	EXPECT_FALSE(info.isBackwardAdj());

	info._exright = 1;
	EXPECT_TRUE(info.isExright());
	EXPECT_TRUE(info.isForwardAdj());
	EXPECT_FALSE(info.isBackwardAdj());

	info._exright = 2;
	EXPECT_TRUE(info.isExright());
	EXPECT_FALSE(info.isForwardAdj());
	EXPECT_TRUE(info.isBackwardAdj());

	// 其它值只算“复权”，既不是前复权也不是后复权
	info._exright = 3;
	EXPECT_TRUE(info.isExright());
	EXPECT_FALSE(info.isForwardAdj());
	EXPECT_FALSE(info.isBackwardAdj());
}

TEST(l01_codehelper, codeinfo_std_comm_id_and_clear)
{
	CodeHelper::CodeInfo info;
	setField(info._exchg, sizeof(info._exchg), "SHFE");
	setField(info._product, sizeof(info._product), "ag");
	EXPECT_STREQ(info.stdCommID(), "SHFE.ag");
	// 第二次调用走 strlen(_fullpid) != 0 的分支，直接返回缓存
	EXPECT_STREQ(info.stdCommID(), "SHFE.ag");

	info.clear();
	EXPECT_EQ(std::strlen(info._fullpid), 0u);
	EXPECT_EQ(std::strlen(info._exchg), 0u);

	// clear 后可以重新拼装
	setField(info._exchg, sizeof(info._exchg), "CFFEX");
	setField(info._product, sizeof(info._product), "IF");
	EXPECT_STREQ(info.stdCommID(), "CFFEX.IF");
}

TEST(l01_codehelper, codeinfo_has_rule)
{
	CodeHelper::CodeInfo info;
	EXPECT_FALSE(info.hasRule());
	setField(info._ruletag, sizeof(info._ruletag), "hot");
	EXPECT_TRUE(info.hasRule());
	info.clear();
	EXPECT_FALSE(info.hasRule());
}

TEST(l01_codehelper, codeinfo_accessor_helpers)
{
	CodeHelper::CodeInfo info;
	setField(info._code, sizeof(info._code), "ag2012");
	EXPECT_STREQ(info._code, "ag2012");
	EXPECT_STREQ(exchgOf(info), "");
}

// ------------------------------------------------- isStdChnFutOptCode 状态机
TEST(l01_codehelper, is_std_chn_fut_opt_code_true)
{
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.C.4300"));
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("DCE.m2009.P.3000"));
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("GFEX.si2505.C.10000"));
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("CFFEX.HO2409.C.1"));
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("CZCE.ZC010.P.11600"));
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("ZCE.ZC010.P.11600"));
}

TEST(l01_codehelper, is_std_chn_fut_opt_code_false)
{
	// 空串：state 仍为 0
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode(""));
	// 交易所段必须是大写字母开头
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("cffex.IO2008.C.4300"));
	// 缺少前面的大写段
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode(".IO2008.C.4300"));
	// 只有两段
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008"));
	// 非郑商所格式仍不接受 3 位月份
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO208.C.4300"));
	// 行权方向不是 C/P
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.X.4300"));
	// 行权价带非数字
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.C.430A"));
	// 末尾多出字母
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.C.4300x"));
	// 第二段以数字开头
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.1O2008.C.4300"));
	// 普通期货代码
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("SHFE.ag.2012"));
}

// ------------------------------------------------------ isStdMonthlyFutCode
TEST(l01_codehelper, is_std_monthly_fut_code)
{
	EXPECT_TRUE(CodeHelper::isStdMonthlyFutCode("CFFEX.IO.2007"));
	EXPECT_TRUE(CodeHelper::isStdMonthlyFutCode("SHFE.ag.1912"));
	EXPECT_TRUE(CodeHelper::isStdMonthlyFutCode("CFFEX.IF.2112"));
	// 月份不足 4 位
	EXPECT_FALSE(CodeHelper::isStdMonthlyFutCode("CFFEX.IF.210"));
	// 三段以上
	EXPECT_FALSE(CodeHelper::isStdMonthlyFutCode("CFFEX.IF.2112.C"));
	EXPECT_FALSE(CodeHelper::isStdMonthlyFutCode(""));
}

// ---------------------------------------------------- stdCodeToStdCommID2
TEST(l01_codehelper, std_code_to_std_comm_id)
{
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("SHFE.ag.2012"), std::string("SHFE.ag"));
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("BINANCE.DC.BTCUSDT"), std::string("BINANCE.DC"));
	// 期权代码的品种 ID 格式尚待合同明确；此处保留普通两段/三段代码对照。
	// SSE.STK 是两段品种标识，作为两段输入原值返回的对照。
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("SSE.STK"), std::string("SSE.STK"));
}

// ----------------------------------------------- rawMonthCodeToRawCommID
TEST(l01_codehelper, raw_month_code_to_raw_comm_id)
{
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("ag1912"), std::string("ag"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("IF2112"), std::string("IF"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("sr0105"), std::string("sr"));
	// 数字开头 => 空前缀
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("1912"), std::string(""));
	// '_'(0x5F) 落在 'A'..'z' 区间内
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("IF_o1"), std::string("IF_o"));
}

// ----------------------------------------------- rawMonthCodeToStdCode
TEST(l01_codehelper, raw_month_code_to_std_code_4_digits)
{
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("ag1912", "SHFE"), std::string("SHFE.ag.1912"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("IF2112", "CFFEX"), std::string("CFFEX.IF.2112"));
}

TEST(l01_codehelper, raw_month_code_to_std_code_3_digits)
{
	// 三位尾码首位 > '5' 时选 '1' 前缀，否则选 '2' 前缀。
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("au912", "SHFE"), std::string("SHFE.au.1912"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("cu001", "SHFE"), std::string("SHFE.cu.2001"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("au512", "SHFE"), std::string("SHFE.au.2512"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("rb105", "SHFE"), std::string("SHFE.rb.2105"));
}

TEST(l01_codehelper, raw_month_code_to_std_code_as_comm)
{
	// isComm=true 接收品种代码；不传入月合约或永续合约代码。
	const std::string shfe = CodeHelper::rawMonthCodeToStdCode("ag", "SHFE", true);
	const std::string cffex = CodeHelper::rawMonthCodeToStdCode("IF", "CFFEX", true);
	EXPECT_EQ(std::string("SHFE.ag"), shfe);
	EXPECT_EQ(7u, shfe.size());
	EXPECT_EQ(std::string("CFFEX.IF"), cffex);
	EXPECT_EQ(8u, cffex.size());

	std::map<std::string, int> commoditiesByStdCode;
	commoditiesByStdCode[shfe] = 1;
	commoditiesByStdCode[cffex] = 2;
	EXPECT_NE(commoditiesByStdCode.find("SHFE.ag"), commoditiesByStdCode.end());
	EXPECT_NE(commoditiesByStdCode.find("CFFEX.IF"), commoditiesByStdCode.end());
}

TEST(l01_codehelper, raw_month_code_to_std_code_dynamic_fields_near_buffer_limit)
{
	const std::string exchange(31, 'E');

	const std::string commAt64(32, 'P');
	const std::string commAt65(33, 'Q');
	const std::string expectedCommAt64 = exchange + "." + commAt64;
	const std::string expectedCommAt65 = exchange + "." + commAt65;
	EXPECT_EQ(64u, expectedCommAt64.size());
	EXPECT_EQ(expectedCommAt64, CodeHelper::rawMonthCodeToStdCode(commAt64.c_str(), exchange.c_str(), true));
	EXPECT_EQ(65u, expectedCommAt65.size());
	EXPECT_EQ(expectedCommAt65, CodeHelper::rawMonthCodeToStdCode(commAt65.c_str(), exchange.c_str(), true));

	const std::string product4(27, 'a');
	const std::string raw4 = product4 + "1912";
	const std::string expectedMonthlyAt64 = exchange + "." + product4 + ".1912";
	EXPECT_EQ(64u, expectedMonthlyAt64.size());
	EXPECT_EQ(expectedMonthlyAt64, CodeHelper::rawMonthCodeToStdCode(raw4.c_str(), exchange.c_str()));

	const std::string product3(28, 'b');
	const std::string raw3 = product3 + "123";
	const std::string expectedMonthlyAt65 = exchange + "." + product3 + ".2123";
	EXPECT_EQ(65u, expectedMonthlyAt65.size());
	EXPECT_EQ(expectedMonthlyAt65, CodeHelper::rawMonthCodeToStdCode(raw3.c_str(), exchange.c_str()));
}

TEST(l01_codehelper, raw_month_code_to_std_code_null_and_short_inputs_return_empty)
{
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode(nullptr, "SHFE").empty());
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag1912", nullptr).empty());
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("", "SHFE").empty());
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag", "SHFE").empty());
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag12", "SHFE").empty());
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("", "SHFE", true).empty());
	EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag", "", true).empty());
}

// ----------------------------------------------- rawFlatCodeToStdCode
TEST(l01_codehelper, raw_flat_code_to_std_code)
{
	EXPECT_EQ(CodeHelper::rawFlatCodeToStdCode("BTCUSDT", "BINANCE", "DC"), std::string("BINANCE.DC.BTCUSDT"));
	// code == pid => 不重复拼接
	EXPECT_EQ(CodeHelper::rawFlatCodeToStdCode("STK", "SSE", "STK"), std::string("SSE.STK"));
	// pid 为空 => 原样
	EXPECT_EQ(CodeHelper::rawFlatCodeToStdCode("600000", "SSE", ""), std::string("SSE.600000"));
}

// ---------------------------------------------------- isMonthlyCode
TEST(l01_codehelper, is_monthly_code_true)
{
	EXPECT_TRUE(CodeHelper::isMonthlyCode("ag1912"));
	EXPECT_TRUE(CodeHelper::isMonthlyCode("IF2112"));
	EXPECT_TRUE(CodeHelper::isMonthlyCode("au912"));
	// 3 位数字尾巴同样被当成月合约（边界较松，见 bug 清单）
	EXPECT_TRUE(CodeHelper::isMonthlyCode("IF211"));
	EXPECT_TRUE(CodeHelper::isMonthlyCode("IO2008"));
	EXPECT_TRUE(CodeHelper::isMonthlyCode("BTC2024"));
}

TEST(l01_codehelper, is_monthly_code_false)
{
	EXPECT_FALSE(CodeHelper::isMonthlyCode("BTCUSDT"));
	EXPECT_FALSE(CodeHelper::isMonthlyCode("600000"));
	EXPECT_FALSE(CodeHelper::isMonthlyCode("a12"));
	EXPECT_FALSE(CodeHelper::isMonthlyCode("STK"));
	EXPECT_FALSE(CodeHelper::isMonthlyCode(""));
	// 合约主体近似月码但末尾含非数字，不满足月码尾缀。
	EXPECT_FALSE(CodeHelper::isMonthlyCode("IF21A"));
}

// ------------------------------------------- rawFutOptCodeToStdCode
TEST(l01_codehelper, raw_fut_opt_code_to_std_code_matched)
{
	// 命中正则：中金所/大商所格式
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("IO2013-C-4000", "CFFEX"), std::string("CFFEX.IO2013.C.4000"));
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("m2009-C-3000", "DCE"), std::string("DCE.m2009.C.3000"));
}

TEST(l01_codehelper, raw_fut_opt_code_to_std_code_unmatched)
{
	// 未命中正则：郑商所/上期所格式 ZC010P11600
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("ZC010P11600", "CZCE"), std::string("CZCE.ZC010.P.11600"));
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("au2012C3400", "SHFE"), std::string("SHFE.au2012.C.3400"));
}

TEST(l01_codehelper, try_raw_fut_opt_code_to_std_code_accepts_supported_formats)
{
	struct Example
	{
		const char* raw;
		const char* exchange;
		const char* expected;
	};
	const Example examples[] = {
		{"IO2301-P-4000", "CFFEX", "CFFEX.IO2301.P.4000"},
		{"m2409-C-3000", "DCE", "DCE.m2409.C.3000"},
		{"si2309-C-11000", "GFEX", "GFEX.si2309.C.11000"},
		{"TA2308P4000", "CZCE", "CZCE.TA2308.P.4000"},
		{"ZC010P11600", "ZCE", "ZCE.ZC010.P.11600"},
		{"au2012C3400", "SHFE", "SHFE.au2012.C.3400"},
		{"sc2308C400", "INE", "INE.sc2308.C.400"},
		// The parser validates the code shape; exchange-specific shape rules belong to callers.
		{"IO2301C4000", "CFFEX", "CFFEX.IO2301.C.4000"},
		{"TA2308P4000", "VENDOR1", "VENDOR1.TA2308.P.4000"},
		{"IO2301-P-4000", "GLOBAL_X", "GLOBAL_X.IO2301.P.4000"},
	};

	for (const Example& example : examples)
	{
		std::string actual = "stale";
		ASSERT_TRUE(CodeHelper::tryRawFutOptCodeToStdCode(example.raw, example.exchange, actual))
			<< example.raw << " @ " << example.exchange;
		EXPECT_EQ(example.expected, actual);
		EXPECT_EQ(example.expected, CodeHelper::rawFutOptCodeToStdCode(example.raw, example.exchange));
	}
}

TEST(l01_codehelper, try_raw_fut_opt_code_to_std_code_rejects_malformed_input)
{
	const char nonAsciiProduct[] = {'T', 'A', static_cast<char>(0xC3), '2', '3', '0', '8', 'P', '4', '0', '0', '0', '\0'};
	const char nonAsciiStrike[] = {'T', 'A', '2', '3', '0', '8', 'P', '4', static_cast<char>(0x80), '\0'};
	const char nonAsciiExchange[] = {'X', static_cast<char>(0xFF), '\0'};
	char unterminatedCode[MAX_INSTRUMENT_LENGTH];
	char unterminatedExchange[MAX_INSTRUMENT_LENGTH];
	std::memset(unterminatedCode, 'A', sizeof(unterminatedCode));
	std::memset(unterminatedExchange, 'X', sizeof(unterminatedExchange));
	const struct InvalidExample
	{
		const char* raw;
		const char* exchange;
	} examples[] = {
		{"", "CZCE"},
		{"123456789", "CZCE"},
		{"P1", "CZCE"},
		{"IO2301-X-4000", "CFFEX"},
		{"TA2308P40x0", "CZCE"},
		// The CTPLoader InstrumentID fixture is synthetic and does not establish
		// SR510CS5600 as a valid raw option code or legal numeric strike format.
		{"SR510CS5600", "CZCE"},
		{nonAsciiProduct, "CZCE"},
		{nonAsciiStrike, "CZCE"},
		{nullptr, "CZCE"},
		{"TA2308P4000", ""},
		{"TA2308P4000", "CFF.EX"},
		{"TA2308P4000", nonAsciiExchange},
		{unterminatedCode, "CZCE"},
		{"TA2308P4000", unterminatedExchange},
	};

	std::string actual;
	actual = "stale";
	EXPECT_FALSE(CodeHelper::tryRawFutOptCodeToStdCode(nullptr, "CZCE", actual));
	EXPECT_TRUE(actual.empty());
	EXPECT_TRUE(CodeHelper::rawFutOptCodeToStdCode(nullptr, "CZCE").empty());
	actual = "stale";
	EXPECT_FALSE(CodeHelper::tryRawFutOptCodeToStdCode("TA2308P4000", nullptr, actual));
	EXPECT_TRUE(actual.empty());
	EXPECT_TRUE(CodeHelper::rawFutOptCodeToStdCode("TA2308P4000", nullptr).empty());

	for (std::size_t index = 0; index < sizeof(examples) / sizeof(examples[0]); ++index)
	{
		const InvalidExample& example = examples[index];
		SCOPED_TRACE(std::string("invalid input example index ") + std::to_string(index));
		actual = "stale";
		EXPECT_FALSE(CodeHelper::tryRawFutOptCodeToStdCode(example.raw, example.exchange, actual));
		EXPECT_TRUE(actual.empty());
		EXPECT_TRUE(CodeHelper::rawFutOptCodeToStdCode(example.raw, example.exchange).empty());
	}
}

// ------------------------------------------- stdCodeToStdHot/2ndCode
TEST(l01_codehelper, std_code_to_std_hot_code)
{
	const std::string hot = CodeHelper::stdCodeToStdHotCode("DCE.m.2109");
	// c_str() 在内部第一个 '\0' 处截断，可见文本正确
	EXPECT_STREQ(hot.c_str(), "DCE.m.HOT");
	EXPECT_EQ(std::string("DCE.m.HOT"), hot);
	EXPECT_EQ(9u, hot.size());
}

TEST(l01_codehelper, std_code_to_std_2nd_code)
{
	const std::string snd = CodeHelper::stdCodeToStd2ndCode("DCE.m.2109");
	EXPECT_STREQ(snd.c_str(), "DCE.m.2ND");
	EXPECT_EQ(std::string("DCE.m.2ND"), snd);
	EXPECT_EQ(9u, snd.size());
}

TEST(l01_codehelper, std_code_to_std_hot_code_without_suffix_sep)
{
	// 只有一个分隔符时，取第一个 '.' 之前的部分
	const std::string hot = CodeHelper::stdCodeToStdHotCode("SSE.STK");
	EXPECT_STREQ(hot.c_str(), "SSE.HOT");
	EXPECT_EQ(std::string("SSE.HOT"), hot);
	EXPECT_EQ(7u, hot.size());
}

// ------------------------------------------- stdFutOptCodeToRawCode
TEST(l01_codehelper, std_fut_opt_code_to_raw_code)
{
	// CFFEX / DCE 用 '-' 连接
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("CFFEX.IO2008.C.4300"), std::string("IO2008-C-4300"));
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("DCE.m2009.C.3000"), std::string("m2009-C-3000"));
	// 其它交易所直接去掉分隔符
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("SHFE.au2012.C.3400"), std::string("au2012C3400"));
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("CZCE.SR105.C.5000"), std::string("SR105C5000"));
	// 没有 '.' 时 find 返回 npos，substr(0) 原样返回
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("IO2008"), std::string("IO2008"));
}

// -------------------------------------------------- indexCodeMonth
TEST(l01_codehelper, index_code_month)
{
	EXPECT_EQ(CodeHelper::indexCodeMonth("IO2008"), 2);
	EXPECT_EQ(CodeHelper::indexCodeMonth("1IF"), 0);
	EXPECT_EQ(CodeHelper::indexCodeMonth("si2505"), 2);
	EXPECT_EQ(CodeHelper::indexCodeMonth("ag"), -1);
	EXPECT_EQ(CodeHelper::indexCodeMonth(""), -1);
}

// ----------------------------------------- extractStdChnFutOptCode
TEST(l01_codehelper, extract_std_chn_fut_opt_code_cffex)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdChnFutOptCode("CFFEX.IO2008.C.4300");
	EXPECT_STREQ(info._exchg, "CFFEX");
	EXPECT_STREQ(info._code, "IO2008-C-4300");
	EXPECT_STREQ(info._product, "IO");
	EXPECT_EQ(info._exright, 0);
	EXPECT_FALSE(info.hasRule());
}

TEST(l01_codehelper, extract_std_chn_fut_opt_code_other_exchg)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdChnFutOptCode("GFEX.si2505.C.10000");
	EXPECT_STREQ(info._exchg, "GFEX");
	EXPECT_STREQ(info._code, "si2505-C-10000");
	// 非 SHFE/INE/CZCE/CFFEX 分支会追加 "_o"
	EXPECT_STREQ(info._product, "si_o");
}

TEST(l01_codehelper, extract_std_chn_fut_opt_code_shfe)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdChnFutOptCode("SHFE.ao2022.C.3400");
	EXPECT_STREQ(info._exchg, "SHFE");
	// SHFE/INE/CZCE 分支把三段直接拼起来
	EXPECT_STREQ(info._code, "ao2022C3400");
	EXPECT_STREQ(info._product, "ao_o");
}

// ----------------------------------------------- extractStdCode
TEST(l01_codehelper, extract_std_code_monthly_future)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("SHFE.ag.2012", nullptr);
	EXPECT_STREQ(info._exchg, "SHFE");
	EXPECT_STREQ(info._product, "ag");
	EXPECT_STREQ(info._code, "ag2012");
	EXPECT_EQ(info._exright, 0);

	info = CodeHelper::extractStdCode("CFFEX.IF.2112", nullptr);
	EXPECT_STREQ(info._exchg, "CFFEX");
	EXPECT_STREQ(info._product, "IF");
	EXPECT_STREQ(info._code, "IF2112");
}

TEST(l01_codehelper, extract_std_code_exright_suffix)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("SSE.STK.600000-", nullptr);
	EXPECT_STREQ(info._exchg, "SSE");
	EXPECT_STREQ(info._product, "STK");
	EXPECT_STREQ(info._code, "600000");
	EXPECT_TRUE(info.isExright());
	EXPECT_TRUE(info.isForwardAdj());

	info = CodeHelper::extractStdCode("SSE.STK.600000+", nullptr);
	EXPECT_STREQ(info._code, "600000");
	EXPECT_TRUE(info.isBackwardAdj());
}

TEST(l01_codehelper, extract_std_code_plain_and_two_segment)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("BINANCE.DC.BTCUSDT", nullptr);
	EXPECT_STREQ(info._exchg, "BINANCE");
	EXPECT_STREQ(info._product, "DC");
	EXPECT_STREQ(info._code, "BTCUSDT");

	// 两段代码：品种与合约一致
	info = CodeHelper::extractStdCode("OKEX.BTC-USDT", nullptr);
	EXPECT_STREQ(info._exchg, "OKEX");
	EXPECT_STREQ(info._product, "BTC-USDT");
	EXPECT_STREQ(info._code, "BTC-USDT");
}

TEST(l01_codehelper, extract_std_code_rejects_malformed_segments)
{
	const char* malformed[] = {
		"", "nosuchcode", ".SSE", "SSE.", "SSE..600000", "SSE.STK.",
		"SSE.STK.600000.", "SSE.STK.600000.EXTRA", "SSE.STK.+"
	};

	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("SSE.STK.600000", nullptr);
	ASSERT_STREQ(info._code, "600000");

	for (const char* code : malformed)
	{
		info = CodeHelper::extractStdCode(code, nullptr);
		EXPECT_EQ(info._exright, 0) << code;
		EXPECT_STREQ(info._code, "") << code;
		EXPECT_STREQ(info._exchg, "") << code;
		EXPECT_STREQ(info._product, "") << code;
		EXPECT_STREQ(info._ruletag, "") << code;
		EXPECT_STREQ(info._fullpid, "") << code;
	}

	info = CodeHelper::extractStdCode(nullptr, nullptr);
	EXPECT_EQ(info._exright, 0);
	EXPECT_STREQ(info._code, "");
	EXPECT_STREQ(info._exchg, "");
	EXPECT_STREQ(info._product, "");
}

TEST(l01_codehelper, extract_std_code_rejects_fields_that_exceed_fixed_capacity)
{
	const std::string tooLong(MAX_INSTRUMENT_LENGTH, 'a');
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode((tooLong + ".STK.600000").c_str(), nullptr);
	EXPECT_STREQ(info._code, "");
	EXPECT_STREQ(info._exchg, "");

	info = CodeHelper::extractStdCode(("SSE." + tooLong + ".600000").c_str(), nullptr);
	EXPECT_STREQ(info._product, "");

	info = CodeHelper::extractStdCode(("SSE.STK." + tooLong).c_str(), nullptr);
	EXPECT_STREQ(info._code, "");

	// A valid monthly code may still overflow the combined output field.
	const std::string longProduct(29, 'a');
	info = CodeHelper::extractStdCode(("X." + longProduct + ".2025").c_str(), nullptr);
	EXPECT_STREQ(info._code, "");

	// Each option segment and stdCommID fit, but the normalized option code does not.
	const std::string longOption = "X." + std::string(25, 'a') + "1234.C.1234";
	info = CodeHelper::extractStdCode(longOption.c_str(), nullptr);
	EXPECT_STREQ(info._code, "");
	EXPECT_STREQ(info._product, "");
}

TEST(l01_codehelper, extract_std_code_std_comm_id_capacity_boundaries)
{
	const std::string exchange = "EXCH";
	const std::size_t maxPidProductLen = MAX_INSTRUMENT_LENGTH - exchange.size() - 2;
	const std::string fitsProduct(maxPidProductLen, 'p');
	const std::string tooLongProduct(maxPidProductLen + 1, 'p');
	const std::string expectedFullPid = exchange + "." + fitsProduct;

	// Both input shapes may fill all but the terminating NUL in _fullpid.
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode((exchange + "." + fitsProduct).c_str(), nullptr);
	ASSERT_STREQ(info.stdCommID(), expectedFullPid.c_str());
	EXPECT_EQ(std::strlen(info._fullpid), static_cast<std::size_t>(MAX_INSTRUMENT_LENGTH - 1));

	info = CodeHelper::extractStdCode((exchange + "." + fitsProduct + ".contract").c_str(), nullptr);
	ASSERT_STREQ(info.stdCommID(), expectedFullPid.c_str());
	EXPECT_EQ(std::strlen(info._fullpid), static_cast<std::size_t>(MAX_INSTRUMENT_LENGTH - 1));

	// A full PID whose characters would occupy the array's NUL byte is rejected.
	info = CodeHelper::extractStdCode((exchange + "." + tooLongProduct).c_str(), nullptr);
	EXPECT_STREQ(info._exchg, "");
	EXPECT_STREQ(info._product, "");
	EXPECT_STREQ(info._code, "");
	EXPECT_STREQ(info._fullpid, "");
	EXPECT_STREQ(info.stdCommID(), ".");
	EXPECT_LT(std::strlen(info._fullpid), static_cast<std::size_t>(MAX_INSTRUMENT_LENGTH));

	info = CodeHelper::extractStdCode((exchange + "." + tooLongProduct + ".contract").c_str(), nullptr);
	EXPECT_STREQ(info._exchg, "");
	EXPECT_STREQ(info._product, "");
	EXPECT_STREQ(info._code, "");
	EXPECT_STREQ(info._fullpid, "");
	EXPECT_STREQ(info.stdCommID(), ".");
	EXPECT_LT(std::strlen(info._fullpid), static_cast<std::size_t>(MAX_INSTRUMENT_LENGTH));
}

TEST(l01_codehelper, extract_std_code_fut_opt_goes_to_opt_branch)
{
	// 期权代码走 extractStdChnFutOptCode 分支
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("CFFEX.IO2008.C.4300", nullptr);
	EXPECT_STREQ(info._exchg, "CFFEX");
	EXPECT_STREQ(info._code, "IO2008-C-4300");
	EXPECT_STREQ(info._product, "IO");
}

TEST(l01_codehelper, extract_std_code_zce_legacy_three_digit_option_round_trip)
{
	const struct Example
	{
		const char* standardCode;
		const char* exchange;
		const char* expectedCode;
		const char* expectedProduct;
	} examples[] = {
		{"CZCE.ZC010.P.11600", "CZCE", "ZC010P11600", "ZCP"},
		{"ZCE.ZC010.P.11600", "ZCE", "ZC010P11600", "ZCP"},
		{"CZCE.TA2308.P.4000", "CZCE", "TA2308P4000", "TAP"},
		{"ZCE.TA2308.P.4000", "ZCE", "TA2308P4000", "TAP"},
	};

	for (const Example& example : examples)
	{
		CodeHelper::CodeInfo info = CodeHelper::extractStdCode(example.standardCode, nullptr);
		EXPECT_STREQ(info._exchg, example.exchange);
		EXPECT_STREQ(info._code, example.expectedCode);
		EXPECT_STREQ(info._product, example.expectedProduct);
		EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode(info._code, info._exchg), example.standardCode);
	}
}

TEST(l01_codehelper, extract_std_code_exright_on_monthly_keeps_month)
{
	// 复权标记剥离后仍是 4 位数字 => 仍按月合约拼接
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("CFFEX.IF.2112-", nullptr);
	EXPECT_STREQ(info._code, "IF2112");
	EXPECT_TRUE(info.isForwardAdj());
}
