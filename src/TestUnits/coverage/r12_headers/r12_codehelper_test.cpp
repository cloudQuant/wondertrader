// Lane R12 - src/Share/CodeHelper.hpp 合约码互转
// 覆盖：extractStdCode 三段/两段式与复权后缀、extractStdChnFutOptCode 各交易所分支、
//       rawMonthCodeToStdCode 的 4 位/3 位月份补齐、rawFlatCodeToStdCode 的 pid 夹取分支、
//       stdCodeToStdHotCode/2nd、stdCodeToStdCommID2、isMonthlyCode/isStdChnFutOptCode 状态机、
//       indexCodeMonth、rawMonthCodeToRawCommID、CodeInfo::stdCommID/hasRule/isExright
// 说明：合约键统一三段式（SHFE.rb.2405），避免两段式把整段当 product 的假红陷阱。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Share/CodeHelper.hpp"

USING_NS_WTP;

namespace {
typedef CodeHelper::CodeInfo CodeInfo;
// CodeInfo 内含 thread_local 静态缓冲，取回后立刻拷成 std::string 再断言
std::string str(const char* s) { return s == nullptr ? std::string("<null>") : std::string(s); }
}

TEST(r12_codehelper, extract_std_monthly_code_three_segment)
{
	CodeInfo ci = CodeHelper::extractStdCode("SHFE.rb.2405", NULL);
	EXPECT_STREQ(ci._exchg, "SHFE");
	EXPECT_STREQ(ci._product, "rb");
	// 最后一段是 4 位数字 → 分月合约，code 要拼上品种前缀
	EXPECT_STREQ(ci._code, "rb2405");
	EXPECT_EQ(str(ci.stdCommID()), "SHFE.rb");
	EXPECT_FALSE(ci.isExright());
	EXPECT_FALSE(ci.isForwardAdj());
	EXPECT_FALSE(ci.isBackwardAdj());
	EXPECT_FALSE(ci.hasRule());

	// 中金所同理
	CodeInfo ci2 = CodeHelper::extractStdCode("CFFEX.IF.2112", NULL);
	EXPECT_STREQ(ci2._exchg, "CFFEX");
	EXPECT_STREQ(ci2._product, "IF");
	EXPECT_STREQ(ci2._code, "IF2112");

	// 末段 4 位但含字母（郑商所 3 位年+1 位月的变体在这里按非分月处理）
	CodeInfo ci3 = CodeHelper::extractStdCode("CZCE.SR.1009", NULL);
	EXPECT_STREQ(ci3._code, "SR1009");
}

TEST(r12_codehelper, extract_std_code_stock_and_exright)
{
	CodeInfo stock = CodeHelper::extractStdCode("SSE.STK.600000", NULL);
	EXPECT_STREQ(stock._exchg, "SSE");
	EXPECT_STREQ(stock._product, "STK");
	// 末段 6 位数字 → 不是分月合约，原样作为 code
	EXPECT_STREQ(stock._code, "600000");
	EXPECT_FALSE(stock.isExright());

	// 前复权后缀 '-'
	CodeInfo qfq = CodeHelper::extractStdCode("SSE.STK.600000-", NULL);
	EXPECT_TRUE(qfq.isExright());
	EXPECT_TRUE(qfq.isForwardAdj());
	EXPECT_FALSE(qfq.isBackwardAdj());
	EXPECT_STREQ(qfq._code, "600000");

	// 后复权后缀 '+'
	CodeInfo hfq = CodeHelper::extractStdCode("SSE.STK.600000+", NULL);
	EXPECT_TRUE(hfq.isBackwardAdj());
	EXPECT_FALSE(hfq.isForwardAdj());
	EXPECT_STREQ(hfq._code, "600000");

	// 两段式：product 与 code 一致
	CodeInfo two = CodeHelper::extractStdCode("OKEX.BTC-USDT", NULL);
	EXPECT_STREQ(two._exchg, "OKEX");
	EXPECT_STREQ(two._product, "BTC-USDT");
	EXPECT_STREQ(two._code, "BTC-USDT");
	EXPECT_FALSE(two.isExright());
}

TEST(r12_codehelper, extract_std_code_hot_and_2nd_suffix)
{
	// 无 hotMgr 时 ruleTag 恒空，HOT/2ND 结尾被原样当作 code
	CodeInfo hot = CodeHelper::extractStdCode("DCE.m.HOT", NULL);
	EXPECT_STREQ(hot._exchg, "DCE");
	EXPECT_STREQ(hot._product, "m");
	EXPECT_STREQ(hot._code, "HOT");
	EXPECT_FALSE(hot.hasRule());
	// 4 位数字结尾仍走分月分支，不会被当成规则标记
	CodeInfo m2 = CodeHelper::extractStdCode("DCE.m.2409", NULL);
	EXPECT_STREQ(m2._code, "m2409");
}

TEST(r12_codehelper, extract_std_chn_futopt_per_exchange_branches)
{
	// CFFEX：code 用短横连接，product 取月份前缀
	CodeInfo cffex = CodeHelper::extractStdChnFutOptCode("CFFEX.IO2013.C.4000");
	EXPECT_STREQ(cffex._exchg, "CFFEX");
	EXPECT_STREQ(cffex._code, "IO2013-C-4000");
	EXPECT_STREQ(cffex._product, "IO");
	// SHFE：code 直接拼接，product 带 _o 后缀
	CodeInfo shfe = CodeHelper::extractStdChnFutOptCode("SHFE.ag2405.C.4000");
	EXPECT_STREQ(shfe._code, "ag2405C4000");
	EXPECT_STREQ(shfe._product, "ag_o");
	// CZCE：product 拼上 C/P
	CodeInfo czce = CodeHelper::extractStdChnFutOptCode("CZCE.SR1009.C.5200");
	EXPECT_STREQ(czce._code, "SR1009C5200");
	EXPECT_STREQ(czce._product, "SRC");
	// INE 与 SHFE 同分支
	CodeInfo ine = CodeHelper::extractStdChnFutOptCode("INE.sc2405.P.400");
	EXPECT_STREQ(ine._code, "sc2405P400");
	EXPECT_STREQ(ine._product, "sc_o");
}

TEST(r12_codehelper, is_std_chn_futopt_state_machine)
{
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2007.C.4000"));
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("SHFE.ag2405.P.4000"));
	// 交易所段必须全大写
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("cffex.IO2007.C.4000"));
	// 品种+月份段必须紧跟 4 位数字
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IF.2112"));
	// C/P 位不允许小写
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2007.c.4000"));
	// 行权价必须是数字
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2007.C.abcd"));
	// 普通期货代码不是期权码
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("SSE.STK.600000"));
}

TEST(r12_codehelper, is_monthly_code_reverse_state_machine)
{
	// 末 3 位数字 + 前面是字母 → 分月
	EXPECT_TRUE(CodeHelper::isMonthlyCode("rb2405"));
	EXPECT_TRUE(CodeHelper::isMonthlyCode("IF2112"));
	// 全数字（股票代码）走不满 state 7
	EXPECT_FALSE(CodeHelper::isMonthlyCode("600000"));
	// 末位不是数字
	EXPECT_FALSE(CodeHelper::isMonthlyCode("BTCUSDT"));
	// 空串：循环体一次都不进，state 0 != 7
	EXPECT_FALSE(CodeHelper::isMonthlyCode(""));
	// 带横杠的加密合约尾部不满足数字前置条件
	EXPECT_FALSE(CodeHelper::isMonthlyCode("BTC-USDT"));
}

TEST(r12_codehelper, index_code_month_and_raw_comm_id)
{
	EXPECT_EQ(CodeHelper::indexCodeMonth("rb2405"), 2);
	EXPECT_EQ(CodeHelper::indexCodeMonth("IO2013"), 2);
	// 首位即数字
	EXPECT_EQ(CodeHelper::indexCodeMonth("600000"), 0);
	// 无数字
	EXPECT_EQ(CodeHelper::indexCodeMonth("abc"), -1);
	// 空串直接 -1
	EXPECT_EQ(CodeHelper::indexCodeMonth(""), -1);

	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("ag1912"), std::string("ag"));
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("rb2405"), std::string("rb"));
	// 全数字代码取到空品种
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("600000"), std::string(""));
	// 纯字母原样返回
	EXPECT_EQ(CodeHelper::rawMonthCodeToRawCommID("BTCUSDT"), std::string("BTCUSDT"));
}

TEST(r12_codehelper, raw_month_code_to_std_code)
{
	// 4 位月份：exchg.pid.month
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("ag1912", "SHFE").c_str(), std::string("SHFE.ag.1912"));
	// 3 位月份：首位 <=5 补 '2'（20xx）
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("rb109", "SHFE").c_str(), std::string("SHFE.rb.2109"));
	// 3 位月份：首位 >5 补 '1'（19xx）
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("rb905", "SHFE").c_str(), std::string("SHFE.rb.1905"));
	// isComm=true：直接把原始码当整体，不再拆品种段
	EXPECT_EQ(CodeHelper::rawMonthCodeToStdCode("ag1912", "SHFE", true).c_str(), std::string("SHFE.ag1912"));
}

TEST(r12_codehelper, raw_flat_code_to_std_code_branches)
{
	// 常规三段
	EXPECT_EQ(CodeHelper::rawFlatCodeToStdCode("600000", "SSE", "STK").c_str(), std::string("SSE.STK.600000"));
	// code 与 pid 相同 → 只保留一段
	EXPECT_EQ(CodeHelper::rawFlatCodeToStdCode("STK", "SSE", "STK").c_str(), std::string("SSE.STK"));
	// pid 为空 → 同样只保留一段
	EXPECT_EQ(CodeHelper::rawFlatCodeToStdCode("600000", "SSE", "").c_str(), std::string("SSE.600000"));
}

TEST(r12_codehelper, std_code_to_hot_and_2nd_wrappers)
{
	// 结果串尾带一个 NUL 填充（resize 计算），所以只能按 c_str 比较
	EXPECT_STREQ(CodeHelper::stdCodeToStdHotCode("DCE.m.2409").c_str(), "DCE.m.HOT");
	EXPECT_STREQ(CodeHelper::stdCodeToStd2ndCode("DCE.m.2409").c_str(), "DCE.m.2ND");
	// 无点号（BTCUSDT）会命中 CodeHelper::find 的反向死循环/越界读，
	// 见 bugs/R12_headers_wtstools.md，已移入下面的 DISABLED_ 用例。
}

// 触发条件：CodeHelper::find(src, '.', true)（CodeHelper.hpp:104-133）用
// `for (std::size_t idx = len - 1; idx >= 0; idx--)` 反向扫描。std::size_t 无符号，
// `idx >= 0` 恒真：一旦串里没有该符号，idx 从 0 减到 SIZE_MAX 并继续读 src[SIZE_MAX]，
// 直接越界读/崩溃。因此 stdCodeToStdHotCode / stdCodeToStd2ndCode / stdCodeToStdCommID2
// 对任何不含点号的入参都会崩（rawFutOptCodeToStdCode 的非 CFFEX 分支是同一个写法）。
TEST(DISABLED_r12_codehelper, dotless_input_runs_off_the_end_of_the_string)
{
	EXPECT_STREQ(CodeHelper::stdCodeToStdHotCode("BTCUSDT").c_str(), "");
	EXPECT_STREQ(CodeHelper::stdCodeToStd2ndCode("BTCUSDT").c_str(), "");
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("600000"), std::string("600000"));
}

TEST(r12_codehelper, std_code_to_std_comm_id_and_opt_raw)
{
	// 三段码取前两段
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("SHFE.ag.1912"), std::string("SHFE.ag"));
	// 两段码原样返回（合约即品种）
	EXPECT_EQ(CodeHelper::stdCodeToStdCommID2("SSE.600000"), std::string("SSE.600000"));
	// 单段无点号入参会越界读，见 DISABLED_ 用例

	// 中金所/大商所格式：短横换成点
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("IO2013-C-4000", "CFFEX").c_str(), std::string("CFFEX.IO2013.C.4000"));
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("m2409-C-3000", "DCE").c_str(), std::string("DCE.m2409.C.3000"));
	// 郑商所/上期所格式：从尾部数字段反推
	EXPECT_EQ(CodeHelper::rawFutOptCodeToStdCode("ZC010P11600", "ZCE").c_str(), std::string("ZCE.ZC010.P.11600"));

	// 标准期权码转原始：CFFEX/DCE 用短横，其它直接去点
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("CFFEX.IO2013.C.4000").c_str(), std::string("IO2013-C-4000"));
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("DCE.m2409.C.3000").c_str(), std::string("m2409-C-3000"));
	EXPECT_EQ(CodeHelper::stdFutOptCodeToRawCode("ZCE.SR1009.C.5200").c_str(), std::string("SR1009C5200"));
}

TEST(r12_codehelper, code_info_helpers_clear_and_stdcommid)
{
	CodeInfo ci = CodeHelper::extractStdCode("CFFEX.IF.2112", NULL);
	EXPECT_STREQ(ci.stdCommID(), "CFFEX.IF");
	ci.clear();
	EXPECT_EQ(strlen(ci._code), (std::size_t)0);
	EXPECT_EQ(strlen(ci._exchg), (std::size_t)0);
	EXPECT_EQ(strlen(ci._product), (std::size_t)0);
	EXPECT_EQ(strlen(ci._ruletag), (std::size_t)0);
	EXPECT_FALSE(ci.hasRule());
	EXPECT_FALSE(ci.isExright());
	// clear 后 stdCommID 仍能拼出（此时两段都为空 → "."）
	EXPECT_STREQ(ci.stdCommID(), ".");
}
