/**
 * @file fmtlib.h
 * @brief 格式化字符串工具函数
 * @details 基于 fmt 库封装的字符串格式化工具，提供便捷的字符串格式化功能
 * @author wondertrader
 */
#pragma once

#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include <spdlog/fmt/bundled/format.h>

/**
 * @brief 格式化字符串工具命名空间
 * @details 提供基于 fmt 库的格式化函数，简化了字符串格式化操作
 */
namespace fmtutil
{
	/**
	 * @brief 格式化字符串并输出到指定缓冲区
	 * @tparam Args 可变参数模板类型
	 * @param buffer 输出缓冲区指针
	 * @param format 格式化字符串模板
	 * @param args 要格式化的参数
	 * @return char* 返回格式化后的字符串末尾指针
	 * @details 使用 fmt 库将参数根据格式化模板格式化到指定缓冲区，并在末尾添加空字符
	 *          调用者需要确保缓冲区足够大，不然可能导致缓冲区溢出
	 */
	template<typename... Args>
	inline char* format_to(char* buffer, const char* format, const Args& ...args)
	{
		char* s = fmt::format_to(buffer, format, args...);
		s[0] = '\0';
		return s;
	}

	/**
	 * @brief 格式化字符串并返回结果
	 * @tparam BUFSIZE 内部缓冲区大小，默认为512字节
	 * @tparam Args 可变参数模板类型
	 * @param format 格式化字符串模板
	 * @param args 要格式化的参数
	 * @return const char* 返回格式化后的字符串指针
	 * @details 使用线程本地静态缓冲区存储格式化后的字符串，并返回其指针
	 *          注意返回的指针指向线程本地静态缓冲区，这意味着：
	 *          1. 返回的字符串只在当前线程中有效
	 *          2. 下一次调用会覆盖同一线程中的上一次结果
	 *          3. 如果格式化后的字符串长度超过BUFSIZE，将导致缓冲区溢出
	 */
	template<int BUFSIZE=512, typename... Args>
	inline const char* format(const char* format, const Args& ...args)
	{
		thread_local static char buffer[BUFSIZE];
		char* s = fmt::format_to(buffer, format, args...);
		s[0] = '\0';
		return buffer;
	}
}
