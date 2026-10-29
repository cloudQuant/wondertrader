// R55（第十一波补测）：CodeHelper.hpp 状态机/期权/规则标签边界 + charconv.hpp 编码转换边界。
// 只读覆盖，不改生产代码。新发现的缺陷见 docs/迭代计划/迭代01/bugs/R55.md。
// 既有 l01_share_wtsutils 用例已覆盖三段/两段/复权/HOT 主路径，本文件只补其未走到的分支，
// 因此用例名与 l01/r16 均不重复（统一 R55_ 前缀）。
#include <gtest/gtest.h>

#include "Share/CodeHelper.hpp"
#include "Share/charconv.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 只用于 extractStdCode 的 IHotMgr* 非空分支：仅 getRuleTag 返回非空，
// 其余纯虚接口给出最小合法实现，不触碰任何真实主力管理器。
class R55RuleHotMgr : public IHotMgr
{
public:
	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char* stdCode) override
	{
		return (std::strcmp(stdCode, "HOT") == 0) ? "R55RULE" : "";
	}
	double getRuleFactor(const char*, const char*, uint32_t) override { return 0.0; }
};

}	// namespace

//////////////////////////////////////////////////////////////////////////
// CodeHelper::isStdChnFutOptCode 的失败状态（CodeHelper.hpp:136-228）
TEST(R55_codehelper, is_std_chn_fut_opt_code_rejects_each_bad_state)
{
	// state 4~6：月份四位数字中出现字母（L183-189）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2A08.C.4300"));
	// state 7：月份后不是 '.'（L190-196）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008X.C.4300"));
	// state 9：C/P 后不是 '.'（L204-210）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.CX.4300"));
	// state 10：行权价首位不是数字（L211-217）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.C.A300"));
	// state 11：行权价后续位不是数字（L218-224）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.C.4300X"));
	// state 2：品种段首字符不合法（L166-172）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX..C.4300"));
	// state 1：交易所后既非大写也非 '.'（L156-165）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode("CFFEX1.IO2008.C.4300"));
	// 空串结束时 state=0，不等于 11（L227）
	EXPECT_FALSE(CodeHelper::isStdChnFutOptCode(""));
	// 单字符行权价仍应通过完整状态机（L227）
	EXPECT_TRUE(CodeHelper::isStdChnFutOptCode("CFFEX.IO2008.C.0"));
}

// extractStdChnFutOptCode 的 CZCE 分支（CodeHelper.hpp:547-587，571-575）
TEST(R55_codehelper, extract_std_chn_fut_opt_code_czce_product_appends_side)
{
	CodeHelper::CodeInfo info = CodeHelper::extractStdChnFutOptCode("CZCE.SR105.C.5000");
	EXPECT_STREQ(info._exchg, "CZCE");
	// SHFE/INE/CZCE 分支：三段直接相连（L553-556）
	EXPECT_STREQ(info._code, "SR105C5000");
	// CZCE 分支：品种取 ay[1] 的数字前部分再拼 ay[2]（C/P），L571-575
	EXPECT_STREQ(info._product, "SRC");
	EXPECT_EQ(info._exright, 0);
	EXPECT_FALSE(info.hasRule());
}

// extractStdCode 的 IHotMgr 非空分支（CodeHelper.hpp:592-661，648-655）
TEST(R55_codehelper, extract_std_code_uses_rule_tag_from_hot_mgr)
{
	R55RuleHotMgr mgr;

	CodeHelper::CodeInfo info = CodeHelper::extractStdCode("SHFE.ag.HOT", &mgr);
	EXPECT_STREQ(info._exchg, "SHFE");
	EXPECT_STREQ(info._product, "ag");
	// 规则标签非空：合约取品种，ruletag 单独保存（L652-655）
	EXPECT_STREQ(info._code, "ag");
	EXPECT_STREQ(info._ruletag, "R55RULE");
	EXPECT_TRUE(info.hasRule());

	// 规则标签为空：仍按普通扩展段处理（L648-650）
	CodeHelper::CodeInfo plain = CodeHelper::extractStdCode("SHFE.ag.XYZ", &mgr);
	EXPECT_STREQ(plain._code, "XYZ");
	EXPECT_EQ(std::strlen(plain._ruletag), 0u);
	EXPECT_FALSE(plain.hasRule());
}

// isMonthlyCode 的 6 位数字/字母混合分支（CodeHelper.hpp:374-436）
TEST(R55_codehelper, is_monthly_code_six_digits_and_letter_cutoffs)
{
	// 6 位数字尾巴后接字母：state 走到 >=6 再由字母置 7（L421-431）
	EXPECT_TRUE(CodeHelper::isMonthlyCode("BTC202412"));
	// 6 位数字到头：state==6，不是 7（L433-435）
	EXPECT_FALSE(CodeHelper::isMonthlyCode("123456"));
	// 5 位数字后接字母：state 停在 5，走 state<6 的“非数字即失败”（L414-419）
	EXPECT_FALSE(CodeHelper::isMonthlyCode("a12345"));
	// 尾部非数字直接失败（L385-390）
	EXPECT_FALSE(CodeHelper::isMonthlyCode("BTC2024AB"));
}

//////////////////////////////////////////////////////////////////////////
// UTF8toChar / ChartoUTF8 的非 ASCII iconv 路径（charconv.hpp:85-131、257-305）
TEST(R55_charconv, utf8_and_gb2312_roundtrip_for_fixed_chinese_input)
{
	// “中” UTF-8 = E4 B8 AD，GB2312 = D6 D0（固定输入，用系统 iconv 对拍过）
	const char* utf8 = "\xE4\xB8\xAD";
	const char* gb = "\xD6\xD0";

	UTF8toChar local(utf8);
	EXPECT_STREQ(gb, local.c_str());

	ChartoUTF8 back(gb);
	EXPECT_STREQ(utf8, back.c_str());

	// std::string 构造函数应走同一转换路径
	const std::string utf8Std(utf8);
	UTF8toChar localFromStd(utf8Std);
	EXPECT_STREQ(gb, localFromStd.c_str());

	const std::string gbStd(gb);
	ChartoUTF8 backFromStd(gbStd);
	EXPECT_STREQ(utf8, backFromStd.c_str());
}

// 空串短路分支（charconv.hpp:85-98、257-270）
TEST(R55_charconv, utf8_helpers_handle_empty_inputs)
{
	UTF8toChar empty("");
	EXPECT_STREQ("", empty.c_str());

	ChartoUTF8 emptyBack("");
	EXPECT_STREQ("", emptyBack.c_str());
}

// BLOCKED by BUG-R55-001: null-input objects leave needFree uninitialized, which
// their destructors may read. Keep these assertions disabled until that state is safe.
TEST(R55_charconv, DISABLED_null_inputs_keep_null_pointers)
{
	UTF8toChar nullIn(static_cast<const char*>(nullptr));
	EXPECT_EQ(nullptr, nullIn.c_str());

	ChartoUTF8 nullBack(static_cast<const char*>(nullptr));
	EXPECT_EQ(nullptr, nullBack.c_str());
}

// ASCII 短路分支保持原指针，并覆盖两个转换运算符（charconv.hpp:141-157、315-331）
TEST(R55_charconv, ascii_helpers_keep_original_pointer_and_convert_operators)
{
	const char* raw = "r55-ascii";
	UTF8toChar u8(raw);
	EXPECT_EQ(raw, u8.c_str());
	const char* u8Cast = u8;
	EXPECT_EQ(raw, u8Cast);

	const char* raw2 = "r55-ascii-2";
	ChartoUTF8 local(raw2);
	EXPECT_EQ(raw2, local.c_str());
	const char* localCast = local;
	EXPECT_EQ(raw2, localCast);
}

// 合法 2 字节 UTF-8 应被接受（BUG-R55-002）。
TEST(R55_charconv, two_byte_utf8_is_accepted)
{
	std::vector<unsigned char> utf8_2{ 0xC2, 0xA2 };	// U+00A2
	EXPECT_TRUE(EncodingHelper::isUtf8(utf8_2.data(), utf8_2.size()));
}

// EncodingHelper 的 GBK 边界（charconv.hpp:560-669）
TEST(R55_charconv, encoding_helper_two_byte_and_gbk_boundaries)
{
	// 后续字节不是 10xxxxxx（L654-660）
	std::vector<unsigned char> utf8_2_bad{ 0xC2, 0x41 };
	EXPECT_FALSE(EncodingHelper::isUtf8(utf8_2_bad.data(), utf8_2_bad.size()));

	// C1 BF 是 overlong 前导，preNUm==2 后直接判非 UTF-8
	std::vector<unsigned char> utf8_overlong{ 0xC1, 0xBF };
	EXPECT_FALSE(EncodingHelper::isUtf8(utf8_overlong.data(), utf8_overlong.size()));

	// GBK 合法双字节的上下边界：首字节 0x81/0xFE，次字节 0x40/0xFE（L570-578）
	std::vector<unsigned char> gbk_low{ 0x81, 0x40 };
	EXPECT_TRUE(EncodingHelper::isGBK(gbk_low.data(), gbk_low.size()));

	std::vector<unsigned char> gbk_high{ 0xFE, 0xFE };
	EXPECT_TRUE(EncodingHelper::isGBK(gbk_high.data(), gbk_high.size()));

	// 纯 0x7F 仍是 ASCII 字节，扫描结束返回 false（L563-567）
	std::vector<unsigned char> ascii{ 0x7F };
	EXPECT_FALSE(EncodingHelper::isGBK(ascii.data(), ascii.size()));
}
