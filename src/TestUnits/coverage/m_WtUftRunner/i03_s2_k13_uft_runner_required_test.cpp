// BUG-R104-002 / I03-NEW-003（迭代03 最终缺口，slice2）——WtUftRunner 的 parser required 选项
// （决策 K13=B）：缺省 true，初始化失败使 initParsers 失败；required:false 失败只告警并继续。
// 重复 id 仍是配置错误（与 required 无关）。只用缺失模块，不连接行情源。
#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "Includes/IParserApi.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/EventNotifier.h"
#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSHotMgr.h"

#define private public
#include "WtUftRunner/WtUftRunner.h"
#undef private

using namespace wtp;

namespace i03s2k13uft {

WTSVariant* parsers_with(int required /* -1: key absent */)
{
	WTSVariant* parsers = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", "i03s2_uft_parser");
	item->append("module", "i03s2_no_such_parser_module");
	if (required >= 0)
		item->append("required", required != 0);
	parsers->append(item, false);
	return parsers;
}

} // namespace i03s2k13uft

using namespace i03s2k13uft;

TEST(I03S2K13UftRunnerRequired, RequiredParserFailureFailsByDefault)
{
	for (int required : { -1, 1 })
	{
		WtUftRunner runner;
		WTSVariant* parsers = parsers_with(required);
		EXPECT_FALSE(runner.initParsers(parsers)) << "required=" << required;
		parsers->release();
		EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_uft_parser").get());
		EXPECT_TRUE(runner.shutdown());
	}
}

TEST(I03S2K13UftRunnerRequired, OptionalParserFailureDegradesAndContinues)
{
	WtUftRunner runner;
	WTSVariant* parsers = parsers_with(0);
	EXPECT_TRUE(runner.initParsers(parsers)) << "an optional parser failure must not fail the runner";
	parsers->release();
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_uft_parser").get()) << "a failed parser is never registered";
	EXPECT_TRUE(runner.shutdown());
}
