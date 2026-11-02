#include "../../../WTSTools/WTSLogger.h"
#include <cstdio>
#include <cstdlib>
WTSLogLevel WTSLogger::m_logLevel = LL_INFO;
bool WTSLogger::m_bStopped = false;
bool WTSLogger::m_bInited = false;
SpdLoggerPtr WTSLogger::m_rootLogger;
thread_local char WTSLogger::m_buffer[MAX_LOG_BUF_SIZE] = {};
void WTSLogger::print_message(const char* message) {
 const char* path=std::getenv("WT_CTA_LOG_MARKER");
 if (path) if (FILE* f=std::fopen(path,"a")) { std::fputs(message,f); std::fputc('\n',f); std::fclose(f); }
}
void WTSLogger::info_imp(SpdLoggerPtr, const char*) {}
void WTSLogger::error_imp(SpdLoggerPtr, const char*) {}
