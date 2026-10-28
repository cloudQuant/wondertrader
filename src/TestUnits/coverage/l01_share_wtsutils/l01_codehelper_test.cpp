// L01 (lane Share/WTSUtils): CodeHelper.hpp coverage.
// 只读覆盖，不修改生产代码。疑似缺陷见 docs/迭代计划/迭代01/bugs/l01_share_wtsutils.md
#include <gtest/gtest.h>

#include "Share/CodeHelper.hpp"

#include <cstdio>
#include <cstring>
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
	// 月份只有 3 位
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
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("CFFEX.IO2008.C.4300"), std::string("CFFEX.IO2008"));
	// 两段代码原样返回（注意：与文件头注释“SSE.600000 -> SSE.STK”不一致，见 bug 清单）
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
	// 首位 > '5' => 20xx
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("au912", "SHFE"), std::string("SHFE.au.1912"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("cu01", "SHFE"), std::string("SHFE.cu.2001"));
	// 首位 <= '5' => 20xx
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("au512", "SHFE"), std::string("SHFE.au.2512"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("rb105", "SHFE"), std::string("SHFE.rb.2105"));
}

TEST(l01_codehelper, raw_month_code_to_std_code_as_comm)
{
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("ag1912", "SHFE", true), std::string("SHFE.ag1912"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("BTC-USDT", "OKEX", true), std::string("OKEX.BTC-USDT"));
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
	EXPECT_FALSE(CodeHelper::isMonthlyCode("IF211"));
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

// ------------------------------------------- stdCodeToStdHot/2ndCode
TEST(l01_codehelper, std_code_to_std_hot_code)
{
	const std::string hot = CodeHelper::stdCodeToStdHotCode("DCE.m.2109");
	// c_str() 在内部第一个 '\0' 处截断，可见文本正确
	EXPECT_STREQ(hot.c_str(), "DCE.m.HOT");
	// BUG-L01-002：resize 多留了 1 字节，string 末尾带一个多余的 '\0'
	EXPECT_EQ(hot.size(), 10u);
}

TEST(l01_codehelper, std_code_to_std_2nd_code)
{
	const std::string snd = CodeHelper::stdCodeToStd2ndCode("DCE.m.2109");
	EXPECT_STREQ(snd.c_str(), "DCE.m.2ND");
	EXPECT_EQ(snd.size(), 10u);
}

TEST(l01_codehelper, std_code_to_std_hot_code_without_suffix_sep)
{
	// 只有一个分隔符时，取第一个 '.' 之前的部分
	const std::string hot = CodeHelper::stdCodeToStdHotCode("SSE.STK");
	EXPECT_STREQ(hot.c_str(), "SSE.HOT");
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

TEST(l01_codehelper, extract_std_code_fut_opt_goes_to_opt_branch)
{
	// 期权代码走 extractStdChnFutOptCode 分支
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("CFFEX.IO2008.C.4300", nullptr);
	EXPECT_STREQ(info._exchg, "CFFEX");
	EXPECT_STREQ(info._code, "IO2008-C-4300");
	EXPECT_STREQ(info._product, "IO");
}

TEST(l01_codehelper, extract_std_code_exright_on_monthly_keeps_month)
{
	// 复权标记剥离后仍是 4 位数字 => 仍按月合约拼接
	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("CFFEX.IF.2112-", nullptr);
	EXPECT_STREQ(info._code, "IF2112");
	EXPECT_TRUE(info.isForwardAdj());
}
