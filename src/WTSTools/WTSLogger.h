/*!
 * \file WTSLogger.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 日志模块定义
 *
 * 本文件定义了WonderTrader的日志系统，基于spdlog实现
 * 支持多种日志级别，多种日志输出方式，包括控制台、文件、每日滚动文件等
 * 可以通过配置文件定制不同分类的日志输出格式和方式
 */
#pragma once
#include "../Includes/WTSTypes.h"
#include "../Includes/WTSCollection.hpp"
#include "../Share/fmtlib.h"

#include <memory>
#include <sstream>
#include <thread>
#include <set>

//By Wesley @ 2022.01.05
//spdlog升级到1.9.2
//同时使用外部的fmt 8.1.0
#include <spdlog/spdlog.h>

typedef std::shared_ptr<spdlog::logger> SpdLoggerPtr;

NS_WTP_BEGIN
class ILogHandler;
class WTSVariant;
NS_WTP_END

USING_NS_WTP;

#define MAX_LOG_BUF_SIZE 2048


/**
 * @brief WonderTrader的日志系统类
 *
 * 基于spdlog实现的日志系统，提供了多种日志级别的输出方式
 * 包括基本的调试、信息、警告、错误和致命错误等多级别日志
 * 支持日志分类、动态日志和定制日志格式
 * 该类的所有方法都是静态的，不需要创建类的实例
 */
class WTSLogger
{
private:
	/**
	 * @brief 调试级别日志实现函数
	 * @param logger 日志器指针
	 * @param message 日志消息
	 */
	static void debug_imp(SpdLoggerPtr logger, const char* message);

	/**
	 * @brief 信息级别日志实现函数
	 * @param logger 日志器指针
	 * @param message 日志消息
	 */
	static void info_imp(SpdLoggerPtr logger, const char* message);

	/**
	 * @brief 警告级别日志实现函数
	 * @param logger 日志器指针
	 * @param message 日志消息
	 */
	static void warn_imp(SpdLoggerPtr logger, const char* message);

	/**
	 * @brief 错误级别日志实现函数
	 * @param logger 日志器指针
	 * @param message 日志消息
	 */
	static void error_imp(SpdLoggerPtr logger, const char* message);

	/**
	 * @brief 致命错误级别日志实现函数
	 * @param logger 日志器指针
	 * @param message 日志消息
	 */
	static void fatal_imp(SpdLoggerPtr logger, const char* message);

	/**
	 * @brief 初始化指定分类的日志器
	 * @param catName 日志分类名称
	 * @param cfgLogger 日志器配置
	 */
	static void initLogger(const char* catName, WTSVariant* cfgLogger);

	/**
	 * @brief 获取指定名称的日志器
	 * @param logger 日志器名称
	 * @param pattern 日志模式，默认为空字符串
	 * @return SpdLoggerPtr 日志器指针，如果不存在且模式为空则返回null
	 */
	static SpdLoggerPtr getLogger(const char* logger, const char* pattern = "");

	/**
	 * @brief 打印消息到控制台
	 * @param buffer 消息内容
	 */
	static void print_message(const char* buffer);

public:
	/**
	 * @brief 直接输出日志消息
	 * 
	 * 使用默认的日志器输出指定级别的日志消息
	 * 
	 * @param ll 日志级别
	 * @param message 日志消息
	 */
	static void log_raw(WTSLogLevel ll, const char* message);

	/**
	 * @brief 按分类输出日志消息
	 * 
	 * 使用指定分类的日志器输出指定级别的日志消息
	 * 
	 * @param catName 分类名称
	 * @param ll 日志级别
	 * @param message 日志消息
	 */
	static void log_raw_by_cat(const char* catName, WTSLogLevel ll, const char* message);

	/**
	 * @brief 使用动态分类模式输出日志消息
	 * 
	 * 使用指定模式和分类的日志器输出指定级别的日志消息
	 * 
	 * @param patttern 日志模式
	 * @param catName 分类名称
	 * @param ll 日志级别
	 * @param message 日志消息
	 */
	static void log_dyn_raw(const char* patttern, const char* catName, WTSLogLevel ll, const char* message);


//////////////////////////////////////////////////////////////////////////
/**
 * @brief fmt格式化日志接口
 *
 * 以下方法使用fmt格式化库提供格式化日志输出能力
 * 支持多种数据类型的格式化并输出为日志
 */
public:
	/**
	 * @brief 输出调试级别日志
	 * 
	 * 使用fmt格式化库将参数格式化后输出调试级别日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void debug(const char* format, const Args& ...args)
	{
		if (m_logLevel > LL_DEBUG || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		if (!m_bInited)
		{
			print_message(m_buffer);
			return;
		}

		debug_imp(m_rootLogger, m_buffer);
	}

	/**
	 * @brief 输出信息级别日志
	 * 
	 * 使用fmt格式化库将参数格式化后输出信息级别日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void info(const char* format, const Args& ...args)
	{
		if (m_logLevel > LL_INFO || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		if (!m_bInited)
		{
			print_message(m_buffer);
			return;
		}

		info_imp(m_rootLogger, m_buffer);
	}

	/**
	 * @brief 输出警告级别日志
	 * 
	 * 使用fmt格式化库将参数格式化后输出警告级别日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void warn(const char* format, const Args& ...args)
	{
		if (m_logLevel > LL_WARN || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		if (!m_bInited)
		{
			print_message(m_buffer);
			return;
		}

		warn_imp(m_rootLogger, m_buffer);
	}

	/**
	 * @brief 输出错误级别日志
	 * 
	 * 使用fmt格式化库将参数格式化后输出错误级别日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void error(const char* format, const Args& ...args)
	{
		if (m_logLevel > LL_ERROR || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		if (!m_bInited)
		{
			print_message(m_buffer);
			return;
		}

		error_imp(m_rootLogger, m_buffer);
	}

	/**
	 * @brief 输出致命错误级别日志
	 * 
	 * 使用fmt格式化库将参数格式化后输出致命错误级别日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void fatal(const char* format, const Args& ...args)
	{
		if (m_logLevel > LL_FATAL || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		if (!m_bInited)
		{
			print_message(m_buffer);
			return;
		}

		fatal_imp(m_rootLogger, m_buffer);
	}

	/**
	 * @brief 使用指定级别输出格式化日志
	 * 
	 * 将格式化参数按指定级别输出日志，使用默认日志器
	 * 
	 * @tparam Args 参数类型列表
	 * @param ll 日志级别
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void log(WTSLogLevel ll, const char* format, const Args& ...args)
	{
		if (m_logLevel > ll || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		log_raw(ll, m_buffer);
	}

	/**
	 * @brief 使用指定分类和级别输出格式化日志
	 * 
	 * 根据分类名称和级别格式化输出日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param catName 日志分类名称
	 * @param ll 日志级别
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void log_by_cat(const char* catName, WTSLogLevel ll, const char* format, const Args& ...args)
	{
		if (m_logLevel > ll || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		log_raw_by_cat(catName, ll, m_buffer);
	}

	/**
	 * @brief 使用指定分类和级别输出格式化日志，并添加分类前缀
	 * 
	 * 根据分类名称和级别格式化输出日志，日志内容前会添加[catName]前缀
	 * 
	 * @tparam Args 参数类型列表
	 * @param catName 日志分类名称
	 * @param ll 日志级别
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void log_by_cat_prefix(const char* catName, WTSLogLevel ll, const char* format, const Args& ...args)
	{
		if (m_logLevel > ll || m_bStopped)
			return;

		m_buffer[0] = '[';
		strcpy(m_buffer + 1, catName);
		auto offset = strlen(catName);
		m_buffer[offset + 1] = ']';
		char* s = m_buffer + offset + 2;
		log_raw_by_cat(catName, ll, m_buffer);
	}

	/**
	 * @brief 使用动态日志模式和分类输出格式化日志
	 * 
	 * 根据指定的日志模式、分类名称和级别格式化输出日志
	 * 
	 * @tparam Args 参数类型列表
	 * @param patttern 日志模式
	 * @param catName 日志分类名称
	 * @param ll 日志级别
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void log_dyn(const char* patttern, const char* catName, WTSLogLevel ll, const char* format, const Args& ...args)
	{
		if (m_logLevel > ll || m_bStopped)
			return;

		fmtutil::format_to(m_buffer, format, args...);

		log_dyn_raw(patttern, catName, ll, m_buffer);
	}

	/**
	 * @brief 使用动态日志模式和分类输出格式化日志，并添加分类前缀
	 * 
	 * 根据指定的日志模式、分类名称和级别格式化输出日志，日志内容前会添加[catName]前缀
	 * 
	 * @tparam Args 参数类型列表
	 * @param patttern 日志模式
	 * @param catName 日志分类名称
	 * @param ll 日志级别
	 * @param format 格式化字符串
	 * @param args 参数列表
	 */
	template<typename... Args>
	static void log_dyn_prefix(const char* patttern, const char* catName, WTSLogLevel ll, const char* format, const Args& ...args)
	{
		if (m_logLevel > ll || m_bStopped)
			return;

		m_buffer[0] = '[';
		strcpy(m_buffer+1, catName);
		auto offset = strlen(catName);
		m_buffer[offset + 1] = ']';
		char* s = m_buffer + offset + 2;
		fmtutil::format_to(s, format, args...);
		log_dyn_raw(patttern, catName, ll, m_buffer);
	}

public:
	/**
	 * @brief 初始化日志系统
	 * 
	 * 根据配置文件初始化日志系统，设置日志级别、输出目标等
	 * 
	 * @param propFile 配置文件路径，默认为"logcfg.json"
	 * @param isFile 是否为文件路径，为false时propFile为配置内容字符串
	 * @param handler 自定义日志处理器，默认为NULL
	 */
	static void init(const char* propFile = "logcfg.json", bool isFile = true, ILogHandler* handler = NULL);

	/**
	 * @brief 注册自定义日志处理器
	 * 
	 * 允许外部注册自定义日志处理器来处理日志消息
	 * 
	 * @param handler 自定义日志处理器，默认为NULL
	 */
	static void registerHandler(ILogHandler* handler = NULL);

	/**
	 * @brief 停止日志系统
	 * 
	 * 停止所有日志输出并释放相关资源
	 */
	static void stop();

	/**
	 * @brief 释放所有动态创建的日志器
	 * 
	 * 遍历并释放所有通过动态模式创建的日志器资源
	 */
	static void freeAllDynLoggers();

private:
	static bool					m_bInited;
	static bool					m_bTpInited;
	static bool					m_bStopped;
	static ILogHandler*			m_logHandler;
	static WTSLogLevel			m_logLevel;

	static SpdLoggerPtr			m_rootLogger;

	typedef WTSHashMap<std::string>	LogPatterns;
	static LogPatterns*				m_mapPatterns;
	static std::set<std::string>	m_setDynLoggers;

	static thread_local char	m_buffer[MAX_LOG_BUF_SIZE];
};


