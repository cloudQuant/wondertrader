// BUG-R104-002 / I03-NEW-003（迭代03 最终缺口，slice2）——WtDtPorter 内 WtDtRunner 的 parser
// required 选项（决策 K13=B）：缺省 true，初始化失败阻止启动（_startup_blocked）；
// required:false 失败只告警，候选隔离，启动不被阻止。只用缺失模块，不连接行情源。
#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <vector>

#include "WtDtPorter/PorterDefs.h"
#include "WtDtPorter/ExpDumper.h"
#include "WtDtCore/DataManager.h"
#include "WtDtCore/ParserAdapter.h"
#include "WtDtCore/StateMonitor.h"
#include "WtDtCore/UDPCaster.h"
#include "WtDtCore/IndexWorkerMgr.h"
#include "WtDtCore/ShmCaster.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include <boost/asio.hpp>
#define private public
#include "WtDtPorter/WtDtRunner.h"
#undef private
#include "Includes/WTSVariant.hpp"

namespace i03s2k13dtp {

WTSVariant* parsers_with(int required /* -1: key absent */)
{
	WTSVariant* parsers = WTSVariant::createArray();
	WTSVariant* item = WTSVariant::createObject();
	item->append("active", true);
	item->append("id", "i03s2_dtp_parser");
	item->append("module", "i03s2_no_such_parser_module");
	if (required >= 0)
		item->append("required", required != 0);
	parsers->append(item, false);
	return parsers;
}

} // namespace i03s2k13dtp

using namespace i03s2k13dtp;

TEST(I03S2K13DtPorterRequired, RequiredParserFailureBlocksStartupByDefault)
{
	for (int required : { -1, 1 })
	{
		WtDtRunner runner;
		WTSVariant* parsers = parsers_with(required);
		runner.initParsers(parsers);
		parsers->release();
		EXPECT_TRUE(runner._startup_blocked) << "required=" << required;
		EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_dtp_parser").get());
	}
}

TEST(I03S2K13DtPorterRequired, OptionalParserFailureDoesNotBlockStartup)
{
	WtDtRunner runner;
	WTSVariant* parsers = parsers_with(0);
	runner.initParsers(parsers);
	parsers->release();
	EXPECT_FALSE(runner._startup_blocked) << "an optional parser failure must not block the data kit";
	EXPECT_EQ(nullptr, runner._parsers.getAdapter("i03s2_dtp_parser").get()) << "a failed parser is never registered";
	EXPECT_EQ(1u, runner._quarantined_ext_parsers.size()) << "the failed candidate stays quarantined";
}
