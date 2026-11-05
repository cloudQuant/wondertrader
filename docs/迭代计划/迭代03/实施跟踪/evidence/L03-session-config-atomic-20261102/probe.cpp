#include <fstream>
#include <iostream>
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSSessionInfo.hpp"
void WTSLogger::log_raw(WTSLogLevel, const char*) {}
static void file(const char* f, const char* v) { std::ofstream o(f); o << v; }
int main(int argc, char**) {
 const char* p="/tmp/wt-i03-session-atomic-20261102/build/input.json";
 WTSBaseDataMgr m;
 file(p,R"({"OLD":{"name":"old","offset":0,"sections":[{"from":900,"to":1500}]}})");
 std::cout << "baseline=" << m.loadSessions(p) << '\n';
 auto old=m.getSession("OLD"); old->retain();
 file(p,R"({"OLD":{"name":"overwrite","offset":300,"sections":[{"from":2100,"to":2300}]},"BROKEN":{"name":"broken","offset":0}})");
 bool ok=m.loadSessions(p); auto now=m.getSession("OLD");
 std::cout << "partial_result=" << ok << " existing_replaced=" << (now!=old) << " offset=" << (now?now->getOffsetMins():-1) << '\n';
 file(p,R"({"N":{"name":"null","offset":0,"sections":[null,{"from":900,"to":1500}]}})");
 bool nullResult=m.loadSessions(p);
 std::cout << "null_result=" << nullResult << " count=" << (m.getSession("N")?m.getSession("N")->getSectionCount():0) << '\n';
 file(p,R"({"N":{"name":"first","offset":0,"sections":[{"from":900,"to":1500}]},"N":{"name":"second","offset":300,"sections":[{"from":2100,"to":2300}]}})");
 std::cout << "duplicate_result=" << m.loadSessions(p) << " offset=" << m.getSession("N")->getOffsetMins() << '\n';
 bool bad=false;
 if (argc > 1) {
 file(p,R"({"BAD":{"name":"bad","offset":2147483648,"sections":[{"from":2460,"to":1500}]}})");
 bad=m.loadSessions(p); auto bads=m.getSession("BAD");
 std::cout << "out_of_domain_result=" << bad << " offset=" << (bads?bads->getOffsetMins():0) << " section_count=" << (bads?bads->getSectionCount():0) << '\n';
 }
 file(p,R"({"NIGHT":{"name":"night","offset":2880,"sections":[{"from":2100,"to":2400},{"from":0,"to":230}]}})");
 bool retry=m.loadSessions(p); auto night=m.getSession("NIGHT"); uint32_t date=0;
 std::cout << "retry=" << retry << " night_count=" << (night?night->getSectionCount():0) << " checked_date=" << (night && night->tryGetOffsetDate(20240228,2100,date)) << " date=" << date << '\n';
 bool failed = ok || now != old || nullResult || bad || !retry || !night || night->getSectionCount() != 2 || date != 20240301;
 old->release();
 return failed ? 1 : 0;
}
bool WTSLogger::m_bInited = false;
bool WTSLogger::m_bStopped = false;
WTSLogLevel WTSLogger::m_logLevel = LL_NONE;
SpdLoggerPtr WTSLogger::m_rootLogger;
thread_local char WTSLogger::m_buffer[MAX_LOG_BUF_SIZE];
void WTSLogger::print_message(const char*) {}
void WTSLogger::error_imp(SpdLoggerPtr, const char*) {}
