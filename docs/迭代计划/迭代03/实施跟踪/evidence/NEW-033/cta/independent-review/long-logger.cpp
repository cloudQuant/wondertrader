#include "/Users/yunjinqi/Documents/new_projects/wondertrader/src/WTSTools/WTSLogger.h"
#include <cstdio>
#include <cstring>
WTSLogLevel WTSLogger::m_logLevel = LL_INFO;
bool WTSLogger::m_bStopped = false;
bool WTSLogger::m_bInited = false;
SpdLoggerPtr WTSLogger::m_rootLogger;
thread_local char WTSLogger::m_buffer[MAX_LOG_BUF_SIZE] = {};
void WTSLogger::print_message(const char* message) { std::fprintf(stderr, "formatted_length=%zu buffer_capacity=%d\n", std::strlen(message), MAX_LOG_BUF_SIZE); }
void WTSLogger::info_imp(SpdLoggerPtr, const char*) {}
void WTSLogger::error_imp(SpdLoggerPtr, const char*) {}
