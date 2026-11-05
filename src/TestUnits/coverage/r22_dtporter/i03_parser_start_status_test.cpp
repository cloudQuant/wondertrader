#include <gtest/gtest.h>
#include <atomic>
#include <functional>
#include <stdexcept>
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
#include "WtDtPorter/WtDtPorter.h"

extern WtDtRunner& getRunner();
namespace {
class ParserForStart final : public IParserApi {
public:
 bool init(WTSVariant*) override { return true; }
 bool connect() override { ++calls; if (callback) callback(); return result; }
 void registerSpi(IParserSpi*) override {}
 void subscribe(const CodeSet&) override {}
 bool result = false;
 int calls = 0;
 std::function<void()> callback;
};
void PORTER_FLAG failDynamicConnect(WtUInt32 event, const char*)
{
 if (event == EVENT_PARSER_CONNECT) throw std::runtime_error("test connect failure");
}

TEST(I03DtPorterStart, failed_config_load_does_not_report_empty_snapshot_success)
{
 WtDtRunner runner;
 runner.initialize("/this/path/does/not/exist/i03-config.json", "/this/path/does/not/exist/i03-log.json", "", true, true);
 runner.start(true,true);
 EXPECT_FALSE(runner.initialParserCommandsSubmitted());
}

TEST(I03DtPorterStart, failed_config_keeps_registered_parser_without_connecting)
{
 WtDtRunner runner;
 auto parser=std::make_shared<ParserAdapter>(&runner._bd_mgr, &runner._data_mgr, nullptr);
 auto* api=new ParserForStart(); api->result=true;
 ASSERT_TRUE(parser->initExt("retained",api));
 ASSERT_TRUE(runner._parsers.addAdapter("retained",parser));
 runner.initialize("/this/path/does/not/exist/i03-config.json", "/this/path/does/not/exist/i03-log.json", "", true, true);
 runner.start(true,true);
 EXPECT_FALSE(runner.initialParserCommandsSubmitted());
 EXPECT_EQ(api->calls,0);
 EXPECT_EQ(runner._parsers.getAdapter("retained"),parser);
 runner._parsers.release();
}

TEST(I03DtPorterStart, valid_empty_parser_config_reports_empty_snapshot_success)
{
 WtDtRunner runner;
 runner.initialize("{\"basefiles\":{},\"writer\":{\"module\":\"no_i03_writer\"},\"allday\":true,\"parsers\":[]}",
                   "/this/path/does/not/exist/i03-log.json", "", false, true);
 runner.start(true,true);
 EXPECT_TRUE(runner.initialParserCommandsSubmitted());
}

TEST(I03DtPorterStart, config_load_failure_can_retry_before_start)
{
 WtDtRunner runner;
 runner.initialize("/this/path/does/not/exist/i03-config.json", "/this/path/does/not/exist/i03-log.json", "", true, true);
 runner.start(true,true);
 EXPECT_FALSE(runner.initialParserCommandsSubmitted());
 runner.initialize("{\"basefiles\":{},\"writer\":{\"module\":\"no_i03_writer\"},\"allday\":true,\"parsers\":[]}",
                   "/this/path/does/not/exist/i03-log.json", "", false, true);
 runner.start(true,true);
 EXPECT_TRUE(runner.initialParserCommandsSubmitted());
}

TEST(I03DtPorterStart, partial_submission_does_not_release_owners)
{
 WtDtRunner runner;
 auto first=std::make_shared<ParserAdapter>(&runner._bd_mgr, &runner._data_mgr, nullptr);
 auto second=std::make_shared<ParserAdapter>(&runner._bd_mgr, &runner._data_mgr, nullptr);
 auto* fail=new ParserForStart(); auto* success=new ParserForStart();
 success->result=true; bool callbackStatus=true;
 fail->callback=[&] { callbackStatus=runner.initialParserCommandsSubmitted(); };
 ASSERT_TRUE(first->initExt("fail",fail)); ASSERT_TRUE(second->initExt("success",success));
 ASSERT_TRUE(runner._parsers.addAdapter("fail",first)); ASSERT_TRUE(runner._parsers.addAdapter("success",second));
 EXPECT_FALSE(runner.initialParserCommandsSubmitted());
 runner.start(true,true);
 EXPECT_FALSE(callbackStatus); EXPECT_FALSE(runner.initialParserCommandsSubmitted());
 EXPECT_EQ(fail->calls,1); EXPECT_EQ(success->calls,1);
 EXPECT_EQ(runner._parsers.getAdapter("fail"),first); EXPECT_EQ(runner._parsers.getAdapter("success"),second);
 runner.start(true,true); EXPECT_FALSE(runner.initialParserCommandsSubmitted());
 EXPECT_EQ(fail->calls,1); EXPECT_EQ(success->calls,1);
 runner._parsers.release();
}
TEST(I03DtPorterStart, capi_reports_initial_snapshot_not_later_connections)
{
 WtDtRunner& runner=getRunner();
 EXPECT_FALSE(initial_parser_connect_commands_submitted());
 runner.start(true,true);
 EXPECT_TRUE(initial_parser_connect_commands_submitted());
 runner.registerParserPorter(failDynamicConnect, nullptr);
 EXPECT_FALSE(runner.createExtParser("later-failure"));
 EXPECT_TRUE(initial_parser_connect_commands_submitted());
 runner.registerParserPorter(nullptr, nullptr);
}
}
