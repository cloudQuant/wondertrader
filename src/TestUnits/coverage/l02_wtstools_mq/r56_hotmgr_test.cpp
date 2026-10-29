// Lane R56 - src/WTSTools/WTSHotMgr.cpp 主力/次主力/自定义换月规则
// 覆盖：loadCustomRules/loadHots/loadSeconds 的文件加载与规则解析（复权因子累计）、
//       getRuleTag/getRuleFactor 的日期窗口分支、getRawCode/getPrevRawCode 的
//       切换日前后/首尾边界、isHot 的 dt=0 与按日回查、splitHotSecions 的分段与
//       越界收口、次主力接口转发、release 后的默认返回。
// 夹具全部为临时目录中的 JSON，不含任何墙钟依赖（dt=0 的日期路径不纳入正常用例）。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Includes/IHotMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "r56_tmp.h"

namespace {

const char* kHots = R"({
	"SHFE": {
		"rb": [
			{"date": 20240102, "from": "",      "to": "rb2401", "oldclose": 0.0,   "newclose": 100.0},
			{"date": 20240110, "from": "rb2401", "to": "rb2405", "oldclose": 100.0, "newclose": 50.0},
			{"date": 20240120, "from": "rb2405", "to": "rb2410", "oldclose": 200.0, "newclose": 100.0}
		]
	}
})";

}	// namespace

class R56HotMgr : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_dir = r56tmp::unique_dir("r56_hot");
		m_file = r56tmp::write_file(m_dir, "hots.json", kHots);
	}

	void TearDown() override
	{
		r56tmp::clean_dir(m_dir);
	}

	WTSHotMgr	m_mgr;
	std::filesystem::path	m_dir;
	std::string	m_file;
};

// 调用方仅在 hot 配置项存在时才调用 loadHots；未配置是正常可选路径。
TEST_F(R56HotMgr, OptionalHotRulesOmittedLeavesManagerUninitialized)
{
	EXPECT_FALSE(m_mgr.isInitialized());

	// 未配置时没有规则，查询保持默认结果。
	EXPECT_STREQ("", m_mgr.getRuleTag("SHFE.rb.HOT"));
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240110));
	EXPECT_STREQ("", m_mgr.getRawCode("SHFE", "rb", 20240110));
	EXPECT_STREQ("", m_mgr.getPrevRawCode("SHFE", "rb", 20240115));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "rb2410", 20240110));
	HotSections sections;
	EXPECT_FALSE(m_mgr.splitHotSecions("SHFE", "rb", 20240101, 20240131, sections));
	EXPECT_TRUE(sections.empty());
}

// 显式配置了路径时，缺失文件属于加载失败；不应报告成功或进入 initialized 状态。
TEST_F(R56HotMgr, ExplicitMissingHotRulesFileFailsWithoutInitializingManager)
{
	const std::string missing = (m_dir / "no_such_hots.json").string();
	EXPECT_FALSE(m_mgr.loadCustomRules("HOT", missing.c_str()));
	EXPECT_FALSE(m_mgr.loadSeconds(missing.c_str()));
	EXPECT_FALSE(m_mgr.loadHots(missing.c_str()));
	EXPECT_FALSE(m_mgr.isInitialized());
}

// WTSHotMgr.cpp:232-269,34-59,148-159
TEST_F(R56HotMgr, LoadParsesFactorsAndRuleTag)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));
	ASSERT_TRUE(m_mgr.isInitialized());

	// 标签表只登记最后主力合约的完整代码
	const std::vector<std::string>& tags = m_mgr.getHotTags();
	ASSERT_EQ(1u, tags.size());
	EXPECT_EQ("HOT", tags[0]);

	// 规则后缀识别：带 +/- 后缀、无交易所前缀、未知标签
	EXPECT_STREQ("HOT", m_mgr.getRuleTag("SHFE.rb.HOT"));
	EXPECT_STREQ("HOT", m_mgr.getRuleTag("SHFE.rb.HOT+"));
	EXPECT_STREQ("HOT", m_mgr.getRuleTag("SHFE.rb.HOT-"));
	EXPECT_STREQ("HOT", m_mgr.getRuleTag("HOT"));
	EXPECT_STREQ("", m_mgr.getRuleTag("SHFE.rb"));
	EXPECT_STREQ("", m_mgr.getRuleTag("SHFE.rb.NOPE"));

	// 复权因子按换月顺序累乘：1.0 -> 2.0 -> 4.0
	EXPECT_DOUBLE_EQ(4.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 0));
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240101));
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240102));
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240109));
	EXPECT_DOUBLE_EQ(2.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240110));
	EXPECT_DOUBLE_EQ(2.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240115));
	EXPECT_DOUBLE_EQ(4.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20240120));
	EXPECT_DOUBLE_EQ(4.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20241231));
	// 未知标签/未知品种返回 1.0
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("2ND", "SHFE.rb", 20240115));
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("HOT", "DCE.i", 20240115));
}

// WTSHotMgr.cpp:132-146,321-356
TEST_F(R56HotMgr, RawCodeOnSwitchBoundaries)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));

	// 切换日当天即生效；切换日之前取上一段主力
	EXPECT_STREQ("rb2401", m_mgr.getRawCode("SHFE", "rb", 20240102));
	EXPECT_STREQ("rb2401", m_mgr.getRawCode("SHFE", "rb", 20240105));
	EXPECT_STREQ("rb2405", m_mgr.getRawCode("SHFE", "rb", 20240110));
	EXPECT_STREQ("rb2405", m_mgr.getRawCode("SHFE", "rb", 20240115));
	EXPECT_STREQ("rb2410", m_mgr.getRawCode("SHFE", "rb", 20240120));
	// 超出最后切换日取最后一根
	EXPECT_STREQ("rb2410", m_mgr.getRawCode("SHFE", "rb", 20241231));
	// 未知品种
	EXPECT_STREQ("", m_mgr.getRawCode("DCE", "i", 20240115));

	// 上一主力：切换日取前一段，首段之前为空
	EXPECT_STREQ("", m_mgr.getPrevRawCode("SHFE", "rb", 20240102));
	EXPECT_STREQ("", m_mgr.getPrevRawCode("SHFE", "rb", 20240105));
	EXPECT_STREQ("rb2401", m_mgr.getPrevRawCode("SHFE", "rb", 20240110));
	EXPECT_STREQ("rb2401", m_mgr.getPrevRawCode("SHFE", "rb", 20240115));
	EXPECT_STREQ("rb2405", m_mgr.getPrevRawCode("SHFE", "rb", 20240120));
	EXPECT_STREQ("rb2405", m_mgr.getPrevRawCode("SHFE", "rb", 20241231));
	EXPECT_STREQ("", m_mgr.getPrevRawCode("DCE", "i", 20240115));
	// 未知规则标签：tag -> product map 未命中
	EXPECT_STREQ("", m_mgr.getCustomRawCode("NOPE", "SHFE.rb", 20240115));
	EXPECT_STREQ("", m_mgr.getPrevCustomRawCode("NOPE", "SHFE.rb", 20240115));
}

// WTSHotMgr.cpp:140-146,358-411
TEST_F(R56HotMgr, IsHotCurrentAndHistorical)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));

	// dt=0 只查标签表里登记的最后主力代码
	EXPECT_TRUE(m_mgr.isHot("SHFE", "rb2410", 0));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "rb2401", 0));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "nope", 0));

	// 按交易日回查：切换日当天新主力生效
	EXPECT_TRUE(m_mgr.isHot("SHFE", "rb2405", 20240110));
	EXPECT_TRUE(m_mgr.isHot("SHFE", "rb2405", 20240115));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "rb2401", 20240115));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "rb2405", 20240105));
	// 超出最后切换日
	EXPECT_TRUE(m_mgr.isHot("SHFE", "rb2410", 20241231));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "rb2405", 20241231));
	// 未知标签（tag 未登记）走 curHotCodes.empty() 早退，返回 false
	EXPECT_FALSE(m_mgr.isCustomHot("NOPE", "SHFE.rb2405", 20240115));
	// 注意：已知标签 + 未知品种的 false 语义目前被产品破坏（isCustomHot 用
	// `return "";` 返回 bool，空串指针恒为 true），断言保留在
	// DISABLED_UnknownProductIsNotHot 中，见 docs/迭代计划/迭代01/bugs/R64.md BUG-R64-001。
}

// 产品缺陷占位：WTSHotMgr.cpp isCustomHot 的 prodMap/dtMap 落空分支写的是
// `return "";`（bool 返回值下空串指针恒为 true），未知品种会被误判为主力。
// 见 docs/迭代计划/迭代01/bugs/R64.md BUG-R64-001；默认不执行。
// WTSHotMgr.cpp:378-385
TEST_F(R56HotMgr, DISABLED_UnknownProductIsNotHot)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));
	EXPECT_FALSE(m_mgr.isHot("DCE", "i2405", 20240115));
}

// WTSHotMgr.cpp:413-462
TEST_F(R56HotMgr, SplitSectionsCoversWindowAndEnd)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));

	HotSections sections;
	ASSERT_TRUE(m_mgr.splitHotSecions("SHFE", "rb", 20240105, 20240125, sections));
	ASSERT_EQ(3u, sections.size());
	EXPECT_EQ("rb2401", sections[0]._code);
	EXPECT_EQ(20240105u, sections[0]._s_date);
	EXPECT_EQ(20240109u, sections[0]._e_date);
	EXPECT_DOUBLE_EQ(1.0, sections[0]._factor);
	EXPECT_EQ("rb2405", sections[1]._code);
	EXPECT_EQ(20240110u, sections[1]._s_date);
	EXPECT_EQ(20240119u, sections[1]._e_date);
	EXPECT_DOUBLE_EQ(2.0, sections[1]._factor);
	EXPECT_EQ("rb2410", sections[2]._code);
	EXPECT_EQ(20240120u, sections[2]._s_date);
	EXPECT_EQ(20240125u, sections[2]._e_date);
	EXPECT_DOUBLE_EQ(4.0, sections[2]._factor);

	// 窗口结束落在两次切换之间：命中 curDate > eDt 分支，首段 from 为空被跳过
	HotSections early;
	ASSERT_TRUE(m_mgr.splitHotSecions("SHFE", "rb", 20240101, 20240115, early));
	ASSERT_EQ(2u, early.size());
	EXPECT_EQ("rb2401", early[0]._code);
	EXPECT_EQ(20240102u, early[0]._s_date);
	EXPECT_EQ(20240109u, early[0]._e_date);
	EXPECT_EQ("rb2405", early[1]._code);
	EXPECT_EQ(20240110u, early[1]._s_date);
	EXPECT_EQ(20240115u, early[1]._e_date);
	EXPECT_DOUBLE_EQ(2.0, early[1]._factor);

	// 未知品种
	HotSections none;
	EXPECT_FALSE(m_mgr.splitHotSecions("DCE", "i", 20240101, 20240131, none));
	EXPECT_TRUE(none.empty());
}

// WTSHotMgr.cpp:171-206,211-230
TEST_F(R56HotMgr, SecondRulesAndReloadSameTag)
{
	ASSERT_TRUE(m_mgr.loadSeconds(m_file.c_str()));
	EXPECT_STREQ("2ND", m_mgr.getRuleTag("SHFE.rb.2ND"));
	EXPECT_DOUBLE_EQ(4.0, m_mgr.getRuleFactor("2ND", "SHFE.rb", 20240125));
	EXPECT_STREQ("rb2405", m_mgr.getSecondRawCode("SHFE", "rb", 20240115));
	EXPECT_STREQ("rb2401", m_mgr.getPrevSecondRawCode("SHFE", "rb", 20240115));
	EXPECT_TRUE(m_mgr.isSecond("SHFE", "rb2410", 0));
	EXPECT_TRUE(m_mgr.isSecond("SHFE", "rb2405", 20240110));
	EXPECT_FALSE(m_mgr.isSecond("SHFE", "rb2401", 20240110));

	HotSections sections;
	ASSERT_TRUE(m_mgr.splitSecondSecions("SHFE", "rb", 20240105, 20240125, sections));
	EXPECT_EQ(3u, sections.size());

	// 同一 tag 二次加载：复用已有 product map，查询结果保持不变
	ASSERT_TRUE(m_mgr.loadCustomRules("2ND", m_file.c_str()));
	EXPECT_STREQ("rb2410", m_mgr.getSecondRawCode("SHFE", "rb", 20241231));
	EXPECT_STREQ("2ND", m_mgr.getRuleTag("SHFE.rb.2ND"));

	// 非法 JSON 文件：加载失败，但已加载的规则仍然可用
	const std::string bad = r56tmp::write_file(m_dir, "bad.json", "this is not json");
	EXPECT_FALSE(m_mgr.loadCustomRules("BAD", bad.c_str()));
	EXPECT_STREQ("rb2410", m_mgr.getSecondRawCode("SHFE", "rb", 20241231));
}

// WTSHotMgr.cpp:465-484
TEST_F(R56HotMgr, ReleaseResetsCustomRules)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));
	ASSERT_STREQ("rb2410", m_mgr.getRawCode("SHFE", "rb", 20241231));

	m_mgr.release();
	EXPECT_STREQ("", m_mgr.getRawCode("SHFE", "rb", 20241231));
	EXPECT_STREQ("", m_mgr.getPrevRawCode("SHFE", "rb", 20241231));
	EXPECT_FALSE(m_mgr.isHot("SHFE", "rb2410", 0));
	EXPECT_DOUBLE_EQ(1.0, m_mgr.getRuleFactor("HOT", "SHFE.rb", 20241231));
	HotSections sections;
	EXPECT_FALSE(m_mgr.splitHotSecions("SHFE", "rb", 20240101, 20240131, sections));
	m_mgr.release();	// 二次 release 必须安全
}

// 产品缺陷占位：切换日之前查询会在 begin() 上执行 cit--（std::map 迭代器下越界）。
// 见 docs/迭代计划/迭代01/bugs/R56.md BUG-R56-001；默认不执行，避免进程崩溃。
// WTSHotMgr.cpp:291-295,337-341
TEST_F(R56HotMgr, DISABLED_RawCodeBeforeFirstSwitchDate)
{
	ASSERT_TRUE(m_mgr.loadHots(m_file.c_str()));
	EXPECT_STREQ("", m_mgr.getRawCode("SHFE", "rb", 20240101));
	EXPECT_STREQ("", m_mgr.getPrevRawCode("SHFE", "rb", 20240101));
}
