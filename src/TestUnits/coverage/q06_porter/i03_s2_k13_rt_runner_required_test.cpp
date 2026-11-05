// BUG-R104-002 / I03-NEW-003（迭代03 最终缺口，slice2）——WtRtRunner（WtPorter）的 parser
// required 选项（决策 K13=B）：缺省 true，初始化失败使 initParsers/配置失败；required:false
// 失败只告警，runner 继续；失败候选与之前一样留在可运行管理器之外的隔离表。
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "Includes/IParserApi.h"
#include "Includes/WTSVariant.hpp"
#include "WtCore/ParserAdapter.h"

#define private public
#include "../../../WtPorter/WtRtRunner.h"
#undef private

USING_NS_WTP;

namespace i03s2k13rt {

WTSVariant* parsers_with(int required /* -1: key absent */, bool withValidSecond = false)
{
	WTSVariant* parsers = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", "i03s2_rt_parser");
	item->append("module", "i03s2_no_such_parser_module");
	if (required >= 0)
		item->append("required", required != 0);
	parsers->append(item, false);
	if (withValidSecond)
	{
		WTSVariant* inactive = WTSVariant::createObject();
		inactive->append("active", false);
		inactive->append("id", "i03s2_rt_inactive");
		parsers->append(inactive, false);
	}
	return parsers;
}

} // namespace i03s2k13rt

using namespace i03s2k13rt;

TEST(I03S2K13RtRunnerRequired, RequiredParserFailureFailsByDefault)
{
	for (int required : { -1, 1 })
	{
		WtRtRunner runner;
		runner._engine = &runner._cta_engine;
		WTSVariant* parsers = parsers_with(required);
		EXPECT_FALSE(runner.initParsers(parsers)) << "required=" << required;
		parsers->release();
		EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_rt_parser").get());
		EXPECT_EQ(1u, runner._quarantined_parser_owners.size());
		runner.release();
	}
}

TEST(I03S2K13RtRunnerRequired, OptionalParserFailureDegradesAndContinues)
{
	WtRtRunner runner;
	runner._engine = &runner._cta_engine;
	WTSVariant* parsers = parsers_with(0, true);
	EXPECT_TRUE(runner.initParsers(parsers)) << "an optional parser failure must not fail the runner";
	parsers->release();
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_rt_parser").get()) << "a failed parser is never runnable";
	EXPECT_EQ(1u, runner._quarantined_parser_owners.size()) << "the failed candidate stays quarantined";
	runner.release();
	EXPECT_TRUE(runner._quarantined_parser_owners.empty()) << "a candidate without an API is cleared by release";
}
