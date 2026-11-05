#include <fstream>
#include <iostream>
#include <string>
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSSessionInfo.hpp"
static int failures=0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << " FAILED: " #x "\n"; ++failures; } } while (0)
static bool load(WTSBaseDataMgr& m, const std::string& data) {
 const char* p="/tmp/wt-i03-session-atomic-20261102/build/test.json";
 std::ofstream out(p); out << data; out.close(); return m.loadSessions(p);
}
int main() {
 WTSBaseDataMgr m;
 CHECK(load(m,R"({"OLD":{"name":"original","offset":0,"sections":[{"from":900,"to":1500}]}})"));
 auto old=m.getSession("OLD"); CHECK(old); if (!old) return 2; old->retain();
 CHECK(!load(m,R"({"OLD":{"name":"replacement","offset":300,"sections":[{"from":2100,"to":2300}]},"BROKEN":{"name":"missing sections"},"NEW":{"offset":0,"sections":[{"from":900,"to":1500}]}})"));
 CHECK(m.getSession("OLD")==old); CHECK(m.getSession("NEW")==nullptr); CHECK(old->getOffsetMins()==0);
 const char* invalid[]={
  R"({"BAD":{"offset":2147483648,"sections":[{"from":900,"to":1500}]}})",
  R"({"BAD":{"offset":-2147483649,"sections":[{"from":900,"to":1500}]}})",
  R"({"BAD":{"offset":1.5,"sections":[{"from":900,"to":1500}]}})",
  R"({"BAD":{"offset":"nan","sections":[{"from":900,"to":1500}]}})",
  R"({"BAD":{"offset":0,"sections":[{"from":2460,"to":1500}]}})",
  R"({"BAD":{"offset":0,"sections":[{"from":900,"to":2360}]}})",
  R"({"BAD":{"offset":0,"sections":[{"from":900}]}})",
  R"({"BAD":{"offset":0,"auction":{"from":900,"to":9999},"sections":[]}})",
  R"({"BAD":{"offset":0,"auctions":{},"sections":[]}})",
  R"({"BAD":42})"
 };
 for (auto s:invalid) { CHECK(!load(m,s)); CHECK(m.getSession("OLD")==old); CHECK(!m.getSession("BAD")); }
 const char* valid=R"({"OLD":{"name":"new","offset":300,"sections":[{"from":2100,"to":2300}]},"NIGHT":{"name":"night","offset":2880,"auction":{"from":0,"to":0},"sections":[{"from":2100,"to":2400},{"from":0,"to":230}]},"MIN":{"offset":-2147483648,"sections":[{"from":0,"to":2400}]},"MAX":{"offset":2147483647,"sections":[{"from":0,"to":2400}]}})";
 CHECK(load(m,valid)); auto night=m.getSession("NIGHT"); CHECK(night && night->getSectionCount()==2);
 uint32_t date=0; CHECK(night && night->tryGetOffsetDate(20240228,2100,date) && date==20240301);
 CHECK(old->getOffsetMins()==0); CHECK(m.getSession("OLD")!=old); CHECK(m.getSession("MIN") && m.getSession("MIN")->getOffsetMins()==(-2147483647-1)); CHECK(m.getSession("MAX") && m.getSession("MAX")->getOffsetMins()==2147483647);
 CHECK(load(m,valid)); CHECK(m.getSession("NIGHT") && m.getSession("NIGHT")->getOffsetMins()==2880);
 old->release(); std::cout << "failures=" << failures << '\n'; return failures ? 1 : 0;
}
bool WTSLogger::m_bInited = false;
bool WTSLogger::m_bStopped = false;
WTSLogLevel WTSLogger::m_logLevel = LL_NONE;
SpdLoggerPtr WTSLogger::m_rootLogger;
thread_local char WTSLogger::m_buffer[MAX_LOG_BUF_SIZE];
void WTSLogger::print_message(const char*) {}
void WTSLogger::error_imp(SpdLoggerPtr, const char*) {}
