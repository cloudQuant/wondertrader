/*!
 * \file WTSLogger.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 日志模块实现
 *
 * 本文件实现了WonderTrader的日志系统，基于spdlog实现
 * 支持各种日志级别、日志输出目标的配置与处理
 * 实现了日志初始化、格式化输出、多级别日志控制等功能
 * 
 * 主要功能包括：
 * 1. 支持多种日志级别：调试、信息、警告、错误和致命错误
 * 2. 支持多种输出目标：控制台、文件（日常文件和基本文件）
 * 3. 支持按分类输出日志，可以为不同模块设置不同的日志配置
 * 4. 支持动态日志创建和格式化，可根据需要动态创建不同模式的日志器
 * 5. 支持自定义日志处理器，可以扩展日志处理方式
 * 6. 支持异步日志输出，提高性能
 * 7. 支持线程安全的日志操作
 */
#include <stdio.h>
#include <iostream>
#include <sys/timeb.h>
#ifdef _MSC_VER
#include <time.h>
#else
#include <sys/time.h>
#endif

#include "WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/ILogHandler.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"

#include <boost/filesystem.hpp>

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/async.h>

const char* DYN_PATTERN = "dyn_pattern";

ILogHandler*		WTSLogger::m_logHandler	= NULL;
WTSLogLevel			WTSLogger::m_logLevel	= LL_NONE;
bool				WTSLogger::m_bStopped = false;
bool				WTSLogger::m_bInited = false;
bool				WTSLogger::m_bTpInited = false;
SpdLoggerPtr		WTSLogger::m_rootLogger = NULL;
WTSLogger::LogPatterns*	WTSLogger::m_mapPatterns = NULL;
thread_local char	WTSLogger::m_buffer[];
std::set<std::string>	WTSLogger::m_setDynLoggers;

/**
 * @brief 将字符串格式的日志级别转换为spdlog日志级别枚举值
 * @param slvl 日志级别字符串，可以是"debug", "info", "warn", "error", "fatal"
 * @return spdlog::level::level_enum 返回spdlog对应的日志级别枚举值
 */
inline spdlog::level::level_enum str_to_level(const char* slvl)
{
	if(wt_stricmp(slvl, "debug") == 0)
	{
		return spdlog::level::debug;
	}
	else if (wt_stricmp(slvl, "info") == 0)
	{
		return spdlog::level::info;
	}
	else if (wt_stricmp(slvl, "warn") == 0)
	{
		return spdlog::level::warn;
	}
	else if (wt_stricmp(slvl, "error") == 0)
	{
		return spdlog::level::err;
	}
	else if (wt_stricmp(slvl, "fatal") == 0)
	{
		return spdlog::level::critical;
	}
	else
	{
		return spdlog::level::off;
	}
}

/**
 * @brief 将字符串格式的日志级别转换为WTSLogLevel枚举值
 * @param slvl 日志级别字符串，可以是"debug", "info", "warn", "error", "fatal"
 * @return WTSLogLevel 返回WonderTrader系统内部对应的日志级别枚举值
 */
inline WTSLogLevel str_to_ll(const char* slvl)
{
	if (wt_stricmp(slvl, "debug") == 0)
	{
		return LL_DEBUG;
	}
	else if (wt_stricmp(slvl, "info") == 0)
	{
		return LL_INFO;
	}
	else if (wt_stricmp(slvl, "warn") == 0)
	{
		return LL_WARN;
	}
	else if (wt_stricmp(slvl, "error") == 0)
	{
		return LL_ERROR;
	}
	else if (wt_stricmp(slvl, "fatal") == 0)
	{
		return LL_FATAL;
	}
	else
	{
		return LL_NONE;
	}
}

/**
 * @brief 检查并创建文件路径中的目录
 * @param filename 文件路径，如果路径中的目录不存在则创建
 */
inline void checkDirs(const char* filename)
{
	std::string s = StrUtil::standardisePath(filename, false);
	std::size_t pos = s.find_last_of('/');

	if (pos == std::string::npos)
		return;

	pos++;

	if (!StdFile::exists(s.substr(0, pos).c_str()))
		boost::filesystem::create_directories(s.substr(0, pos).c_str());
}

/**
 * @brief 输出当前时间标签
 * @param bWithSpace 是否在时间标签后追加空格，默认为true
 */
inline void print_timetag(bool bWithSpace = true)
{
	uint64_t now = TimeUtils::getLocalTimeNow();
	time_t t = now / 1000;

	tm * tNow = localtime(&t);
	fmt::print("[{}.{:02d}.{:02d} {:02d}:{:02d}:{:02d}]", tNow->tm_year + 1900, tNow->tm_mon + 1, tNow->tm_mday, tNow->tm_hour, tNow->tm_min, tNow->tm_sec);
	if (bWithSpace)
		fmt::print(" ");
}

/**
 * @brief 直接打印日志消息到控制台
 * @param buffer 要打印的消息内容
 */
void WTSLogger::print_message(const char* buffer)
{
	print_timetag(true);
	fmt::print(buffer);
	fmt::print("\r\n");
}

/**
 * @brief 初始化指定分类的日志器
 * @param catName 分类名称，用于识别和管理日志器
 * @param cfgLogger 日志器的配置信息，包含异步设置、日志级别、输出目标等
 */
void WTSLogger::initLogger(const char* catName, WTSVariant* cfgLogger)
{
	bool bAsync = cfgLogger->getBoolean("async");
	const char* level = cfgLogger->getCString("level");

	WTSVariant* cfgSinks = cfgLogger->get("sinks");
	std::vector<spdlog::sink_ptr> sinks;
	for (uint32_t idx = 0; idx < cfgSinks->size(); idx++)
	{
		WTSVariant* cfgSink = cfgSinks->get(idx);
		const char* type = cfgSink->getCString("type");
		if (strcmp(type, "daily_file_sink") == 0)
		{
			std::string filename = cfgSink->getString("filename");
			StrUtil::replace(filename, "%s", catName);
			checkDirs(filename.c_str());
			auto sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(filename, 0, 0);
			sink->set_pattern(cfgSink->getCString("pattern"));
			sinks.emplace_back(sink);
		}
		else if (strcmp(type, "basic_file_sink") == 0)
		{
			std::string filename = cfgSink->getString("filename");
			StrUtil::replace(filename, "%s", catName);
			checkDirs(filename.c_str());
			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, cfgSink->getBoolean("truncate"));
			sink->set_pattern(cfgSink->getCString("pattern"));
			sinks.emplace_back(sink);
		}
		else if (strcmp(type, "console_sink") == 0)
		{
			auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			sink->set_pattern(cfgSink->getCString("pattern"));
			sinks.emplace_back(sink);
		}
		else if (strcmp(type, "ostream_sink") == 0)
		{
			auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cout, true);
			sink->set_pattern(cfgSink->getCString("pattern"));
			sinks.emplace_back(sink);
		}
	}

	if (!bAsync)
	{
		auto logger = std::make_shared<spdlog::logger>(catName, sinks.begin(), sinks.end());
		logger->set_level(str_to_level(cfgLogger->getCString("level")));
		spdlog::register_logger(logger);
	}
	else
	{
		if(!m_bTpInited)
		{
			spdlog::init_thread_pool(8192, 2);
			m_bTpInited = true;
		}

		auto logger = std::make_shared<spdlog::async_logger>(catName, sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
		logger->set_level(str_to_level(cfgLogger->getCString("level")));
		spdlog::register_logger(logger);
	}

	if(strcmp(catName, "root")==0)
	{
		m_logLevel = str_to_ll(cfgLogger->getCString("level"));
	}
}

/**
 * @brief 初始化日志系统
 * @param propFile 配置文件路径或配置内容字符串，默认为"logcfg.json"
 * @param isFile 是否为文件路径，为false时propFile被视为配置内容字符串
 * @param handler 自定义日志处理器，默认为NULL
 */
void WTSLogger::init(const char* propFile /* = "logcfg.json" */, bool isFile /* = true */, ILogHandler* handler /* = NULL */)
{
	if (m_bInited)
		return;

	if (isFile && !StdFile::exists(propFile))
		return;

	WTSVariant* cfg = isFile ? WTSCfgLoader::load_from_file(propFile) : WTSCfgLoader::load_from_content(propFile, false);
	if (cfg == NULL)
		return;

	auto keys = cfg->memberNames();
	for (std::string& key : keys)
	{
		WTSVariant* cfgItem = cfg->get(key.c_str());
		if (key == DYN_PATTERN)
		{
			auto pkeys = cfgItem->memberNames();
			for(std::string& pkey : pkeys)
			{
				WTSVariant* cfgPattern = cfgItem->get(pkey.c_str());
				if (m_mapPatterns == NULL)
					m_mapPatterns = LogPatterns::create();

				m_mapPatterns->add(pkey.c_str(), cfgPattern, true);
			}
			continue;
		}

		initLogger(key.c_str(), cfgItem);
	}

	m_rootLogger = getLogger("root");
	if(m_rootLogger == NULL)
	{
		throw std::runtime_error("root logger can not be null, please check the config file");
	}
	spdlog::set_default_logger(m_rootLogger);
	spdlog::flush_every(std::chrono::seconds(2));

	m_logHandler = handler;

	m_bInited = true;
}

/**
 * @brief 注册自定义日志处理器
 * @param handler 日志处理器实例，默认为NULL
 */
void WTSLogger::registerHandler(ILogHandler* handler /* = NULL */)
{
	m_logHandler = handler;
}

/**
 * @brief 停止日志系统
 * 
 * 释放日志系统相关资源，包括关闭所有日志器和释放模式映射
 */
void WTSLogger::stop()
{
	m_bStopped = true;
	if (m_mapPatterns)
		m_mapPatterns->release();
	spdlog::shutdown();
}

/**
 * @brief 调试级别日志输出的实现函数
 * @param logger 日志器指针，用于实际输出日志
 * @param message 要输出的日志消息
 */
void WTSLogger::debug_imp(SpdLoggerPtr logger, const char* message)
{
	if (logger)
		logger->debug(message);

	if (logger != m_rootLogger)
		m_rootLogger->debug(message);

	if (m_logHandler)
		m_logHandler->handleLogAppend(LL_DEBUG, message);
}

/**
 * @brief 信息级别日志输出的实现函数
 * @param logger 日志器指针，用于实际输出日志
 * @param message 要输出的日志消息
 */
void WTSLogger::info_imp(SpdLoggerPtr logger, const char* message)
{
	if (logger)
		logger->info(message);

	if (logger != m_rootLogger)
		m_rootLogger->info(message);

	if (m_logHandler)
		m_logHandler->handleLogAppend(LL_INFO, message);
}

/**
 * @brief 警告级别日志输出的实现函数
 * @param logger 日志器指针，用于实际输出日志
 * @param message 要输出的日志消息
 */
void WTSLogger::warn_imp(SpdLoggerPtr logger, const char* message)
{
	if (logger)
		logger->warn(message);

	if (logger != m_rootLogger)
		m_rootLogger->warn(message);

	if (m_logHandler)
		m_logHandler->handleLogAppend(LL_WARN, message);
}

/**
 * @brief 错误级别日志输出的实现函数
 * @param logger 日志器指针，用于实际输出日志
 * @param message 要输出的日志消息
 */
void WTSLogger::error_imp(SpdLoggerPtr logger, const char* message)
{
	if (logger)
		logger->error(message);

	if (logger != m_rootLogger)
		m_rootLogger->error(message);

	if (m_logHandler)
		m_logHandler->handleLogAppend(LL_ERROR, message);
}

/**
 * @brief 致命错误级别日志输出的实现函数
 * @param logger 日志器指针，用于实际输出日志
 * @param message 要输出的日志消息
 */
void WTSLogger::fatal_imp(SpdLoggerPtr logger, const char* message)
{
	if (logger)
		logger->critical(message);

	if (logger != m_rootLogger)
		m_rootLogger->critical(message);

	if (m_logHandler)
		m_logHandler->handleLogAppend(LL_FATAL, message);
}

/**
 * @brief 直接输出指定级别的原始日志消息
 * @param ll 日志级别，决定使用哪个日志函数来输出
 * @param message 要输出的日志消息
 */
void WTSLogger::log_raw(WTSLogLevel ll, const char* message)
{
	if (m_logLevel > ll || m_bStopped)
		return;

	if (!m_bInited)
	{
		print_message(message);
		return;
	}

	auto logger = m_rootLogger;

	if (logger)
	{
		switch (ll)
		{
		case LL_DEBUG:
			debug_imp(logger, message); break;
		case LL_INFO:
			info_imp(logger, message); break;
		case LL_WARN:
			warn_imp(logger, message); break;
		case LL_ERROR:
			error_imp(logger, message); break;
		case LL_FATAL:
			fatal_imp(logger, message); break;
		default:
			break;
		}
	}
}

/**
 * @brief 按分类直接输出指定级别的原始日志消息
 * @param catName 分类名称，用于获取对应的日志器
 * @param ll 日志级别，决定使用哪个日志函数来输出
 * @param message 要输出的日志消息
 */
void WTSLogger::log_raw_by_cat(const char* catName, WTSLogLevel ll, const char* message)
{
	if (m_logLevel > ll || m_bStopped)
		return;

	auto logger = getLogger(catName);
	if (logger == NULL)
		logger = m_rootLogger;

	if (!m_bInited)
	{
		print_timetag(true);
		fmt::print(message);
		fmt::print("\n");
		return;
	}

	if (logger)
	{
		switch (ll)
		{
		case LL_DEBUG:
			debug_imp(logger, message);
			break;
		case LL_INFO:
			info_imp(logger, message);
			break;
		case LL_WARN:
			warn_imp(logger, message);
			break;
		case LL_ERROR:
			error_imp(logger, message);
			break;
		case LL_FATAL:
			fatal_imp(logger, message);
			break;
		default:
			break;
		}
	}	
}

/**
 * @brief 使用动态日志模式直接输出指定级别的日志消息
 * @param patttern 日志模式，影响日志输出的格式和目标
 * @param catName 分类名称，用于获取或创建对应的日志器
 * @param ll 日志级别，决定使用哪个日志函数来输出
 * @param message 要输出的日志消息
 */
void WTSLogger::log_dyn_raw(const char* patttern, const char* catName, WTSLogLevel ll, const char* message)
{
	if (m_logLevel > ll || m_bStopped)
		return;

	auto logger = getLogger(catName, patttern);
	if (logger == NULL)
		logger = m_rootLogger;

	if (!m_bInited)
	{
		print_timetag(true);
		fmt::print(m_buffer);
		fmt::print("\n");
		return;
	}

	switch (ll)
	{
	case LL_DEBUG:
		debug_imp(logger, message);
		break;
	case LL_INFO:
		info_imp(logger, message);
		break;
	case LL_WARN:
		warn_imp(logger, message);
		break;
	case LL_ERROR:
		error_imp(logger, message);
		break;
	case LL_FATAL:
		fatal_imp(logger, message);
		break;
	default:
		break;
	}
}


/**
 * @brief 获取指定名称的日志器，如果不存在且指定了模式则动态创建
 * 
 * 尝试获取指定名称的日志器，如果不存在且指定了有效的模式，
 * 则根据模式和名称动态创建一个新的日志器
 * 
 * @param logger 要获取的日志器名称
 * @param pattern 日志模式名称，默认为空字符串
 * @return SpdLoggerPtr 返回找到或创建的日志器指针，如果失败则返回NULL
 */
SpdLoggerPtr WTSLogger::getLogger(const char* logger, const char* pattern /* = "" */)
{
	SpdLoggerPtr ret = spdlog::get(logger);
	if (ret == NULL && strlen(pattern) > 0)
	{
		//当成动态的日志来处理
		if (m_mapPatterns == NULL)
			return SpdLoggerPtr();

		WTSVariant* cfg = (WTSVariant*)m_mapPatterns->get(pattern);
		if (cfg == NULL)
			return SpdLoggerPtr();

		initLogger(logger, cfg);

		m_setDynLoggers.insert(logger);

		return spdlog::get(logger);
	}

	return ret;
}

/**
 * @brief 释放所有动态创建的日志器
 * 
 * 遍历m_setDynLoggers集合中所有记录的动态日志器名称，
 * 通过spdlog::drop方法释放对应的日志器资源
 */
void WTSLogger::freeAllDynLoggers()
{
	for(const std::string& logger : m_setDynLoggers)
	{
		auto loggerPtr = spdlog::get(logger);
		if(!loggerPtr)
			continue;

		spdlog::drop(logger);
	}
}