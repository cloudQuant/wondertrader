/*!
 * \file r125_selfthft_factories_test.cpp
 * \brief lane R125: WtUftStraFact / WtHftStraFact 的 C 导出、枚举与拒删分支
 *
 * 覆盖依据（本轮读源码确认）：
 * - WtUftStraFact.cpp L32-47 createStrategyFact/deleteStrategyFact；
 *   L76-91 getName/enumStrategy；L101-131 create/delete（L125-126 非本厂拒删）。
 * - WtHftStraFact.cpp L27-42 createStrategyFact/deleteStrategyFact；
 *   L69-82 getName/enumStrategy；L92-122 create/delete（L116-117 非本厂拒删）。
 * UFT/HFT 的 C 导出在测试目标里按目录被编译期改名为 *_wtuft / *_wthft（见
 * src/TestUnits/coverage/CMakeLists.txt 的 set_source_files_properties），本文件直接
 * 声明别名符号调用，不 dlopen、不加载插件。
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Includes/UftStrategyDefs.h"
#include "Includes/HftStrategyDefs.h"
#include "WtUftStraFact/WtUftStraFact.h"
#include "WtHftStraFact/WtHftStraFact.h"

#include "r125_selfthft_fake.h"

USING_NS_WTP;

extern "C" IUftStrategyFact* createStrategyFact_wtuft();
extern "C" void deleteStrategyFact_wtuft(IUftStrategyFact* fact);
extern "C" IHftStrategyFact* createStrategyFact_wthft();
extern "C" void deleteStrategyFact_wthft(IHftStrategyFact* fact);

namespace {

std::vector<std::string>& enum_logs()
{
	static std::vector<std::string> logs;
	return logs;
}

void collect_enum(const char* fact, const char* stra, bool isLast)
{
	enum_logs().push_back(std::string(fact == NULL ? "" : fact) + "/" +
		(stra == NULL ? "" : stra) + (isLast ? ":last" : ""));
}

} // namespace

TEST(R125SelfFactories, UftCreateDeleteEnumAndReject)
{
	IUftStrategyFact* api = createStrategyFact_wtuft();
	ASSERT_NE(nullptr, api);
	EXPECT_STREQ("WtUftStraFact", api->getName());
	EXPECT_EQ(nullptr, api->createStrategy("NoSuchUft", "x"));

	enum_logs().clear();
	api->enumStrategy(collect_enum);
	ASSERT_EQ(1u, enum_logs().size());
	EXPECT_EQ("WtUftStraFact/SimpleUft:last", enum_logs()[0]);

	UftStrategy* stra = api->createStrategy("SimpleUft", "r125_uft_api");
	ASSERT_NE(nullptr, stra);
	EXPECT_STREQ("UftDemoStrategy", stra->getName());
	EXPECT_STREQ("WtUftStraFact", stra->getFactName());
	EXPECT_TRUE(api->deleteStrategy(stra));

	r125_sh::ForeignUftStrategy foreign;
	EXPECT_FALSE(api->deleteStrategy(&foreign));
	EXPECT_TRUE(api->deleteStrategy(NULL));

	deleteStrategyFact_wtuft(api);
	deleteStrategyFact_wtuft(NULL);
}

TEST(R125SelfFactories, HftCreateDeleteEnumAndReject)
{
	IHftStrategyFact* api = createStrategyFact_wthft();
	ASSERT_NE(nullptr, api);
	EXPECT_STREQ("WtHftStraFact", api->getName());
	EXPECT_EQ(nullptr, api->createStrategy("NoSuchHft", "x"));

	enum_logs().clear();
	api->enumStrategy(collect_enum);
	ASSERT_EQ(1u, enum_logs().size());
	EXPECT_EQ("WtHftStraFact/SimpleHft:last", enum_logs()[0]);

	HftStrategy* stra = api->createStrategy("SimpleHft", "r125_hft_api");
	ASSERT_NE(nullptr, stra);
	EXPECT_STREQ("HftDemoStrategy", stra->getName());
	EXPECT_STREQ("WtHftStraFact", stra->getFactName());
	EXPECT_TRUE(api->deleteStrategy(stra));

	r125_sh::ForeignHftStrategy foreign;
	EXPECT_FALSE(api->deleteStrategy(&foreign));
	EXPECT_TRUE(api->deleteStrategy(NULL));

	deleteStrategyFact_wthft(api);
	deleteStrategyFact_wthft(NULL);
}
