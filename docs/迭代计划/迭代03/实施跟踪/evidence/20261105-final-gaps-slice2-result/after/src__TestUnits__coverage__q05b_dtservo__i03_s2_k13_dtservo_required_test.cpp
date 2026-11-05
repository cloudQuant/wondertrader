// BUG-R104-002 / I03-NEW-003（迭代03 最终缺口，slice2）——WtDtServo 运行器的 parser required
// 选项（决策 K13=B）：缺省 true，初始化失败使 initParsers 失败（服务不启动）；required:false
// 失败只告警，候选隔离，其余 parser 照常登记。只用缺失模块，不连接行情源。
#include <gtest/gtest.h>

#include <string>

#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtDtServo/WtDataManager.h>
#define private public
#include <WtDtServo/WtDtRunner.h>
#undef private

namespace i03s2k13servo {

WTSVariant* parsers_with(int required /* -1: key absent */)
{
	WTSVariant* parsers = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", "i03s2_servo_parser");
	item->append("module", "i03s2_no_such_parser_module");
	if (required >= 0)
		item->append("required", required != 0);
	parsers->append(item, false);
	return parsers;
}

} // namespace i03s2k13servo

using namespace i03s2k13servo;

TEST(I03S2K13DtServoRequired, RequiredParserFailureStopsTheServiceByDefault)
{
	for (int required : { -1, 1 })
	{
		WtDtRunner runner;
		WTSVariant* parsers = parsers_with(required);
		EXPECT_FALSE(runner.initParsers(parsers)) << "required=" << required;
		parsers->release();
		EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_servo_parser").get());
	}
}

TEST(I03S2K13DtServoRequired, OptionalParserFailureDegradesAndContinues)
{
	WtDtRunner runner;
	WTSVariant* parsers = parsers_with(0);
	EXPECT_TRUE(runner.initParsers(parsers)) << "an optional parser failure must not stop the data service";
	parsers->release();
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_servo_parser").get()) << "a failed parser is never registered";
	EXPECT_EQ(1u, runner._quarantined_parsers.size()) << "the failed candidate stays quarantined";
}
