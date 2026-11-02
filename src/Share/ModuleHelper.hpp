/**
 * @file ModuleHelper.hpp
 * @brief 模块路径辅助工具
 * @details 提供跨平台的模块路径获取功能，支持Windows和Unix/Linux平台
 * @author Wesley
 * @date 2020/03/30
 */

#pragma once
#include "../Share/StrUtil.hpp"

#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#define WT_MODULE_HELPER_UNDEFINE_NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define WT_MODULE_HELPER_UNDEFINE_WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef WT_MODULE_HELPER_UNDEFINE_WIN32_LEAN_AND_MEAN
#undef WT_MODULE_HELPER_UNDEFINE_WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#ifdef WT_MODULE_HELPER_UNDEFINE_NOMINMAX
#undef WT_MODULE_HELPER_UNDEFINE_NOMINMAX
#undef NOMINMAX
#endif

// Writable TU-local storage cannot be folded with another module's anchor.
// Looking up this address identifies the DLL (or executable) containing the TU
// without defining a DllMain in a public header.
static char wt_module_helper_path_anchor;
#else
#include <dlfcn.h>
/**
 * @brief Linux/Unix平台下的模块地址锚点
 * @details 内部链接的空函数，仅用于获取当前模块的内存地址作为dladdr函数的参数。
 *          每个包含本头文件的翻译单元都有自己的锚点，避免多翻译单元链接冲突。
 */
static void wt_module_helper_path_anchor() {}
/**
 * @brief 获取当前动态库的完整路径
 * @return const std::string& 返回当前动态库的完整路径
 * @details 在Linux/Unix平台下使用dladdr函数获取当前动态库的完整路径
 *          首次调用时计算路径并缓存，后续调用直接返回缓存值
 */
static const std::string& getInstPath()
{
	// Cache failures as well as successes and let C++ initialize the result
	// once, so concurrent first callers never write the same string.
	static const std::string moduleName = []() -> std::string {
		Dl_info dl_info{};
		if (dladdr((void *)wt_module_helper_path_anchor, &dl_info) == 0 || dl_info.dli_fname == nullptr)
			return {};
		return dl_info.dli_fname;
	}();

	return moduleName;
}
#endif

/**
 * @brief 获取当前模块所在的目录路径
 * @return const char* 返回当前模块所在的目录路径，以斜杠结尾
 * @details 该函数在不同平台下使用相应的API获取当前模块的路径
 *          Windows平台下使用GetModuleFileName函数
 *          Linux/Unix平台下使用getInstPath函数
 *          然后提取路径中的目录部分，去除文件名
 *          首次调用时计算路径并缓存，后续调用直接返回缓存值
 */
[[maybe_unused]] static const char* getBinDir()
{
#ifdef _MSC_VER
	// Initialize once, including on failure, so concurrent callers do not race
	// over a mutable path.  An empty string reports lookup/truncation failure;
	// never pass a null module handle to GetModuleFileNameA (which means the exe).
	static const std::string g_bin_dir = []() -> std::string {
		HMODULE module = NULL;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&wt_module_helper_path_anchor), &module) || module == NULL)
			return {};

		std::vector<char> path(MAX_PATH);
		// Accommodate long Windows paths and multibyte ANSI encodings, while
		// refusing to turn a persistently truncated result into a directory.
		constexpr std::size_t maxPathBytes = 131072;
		for (;;)
		{
			DWORD length = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0)
				return {};
			if (length < path.size())
			{
				std::string modulePath = StrUtil::standardisePath(std::string(path.data(), length), false);
				const std::size_t pos = modulePath.find_last_of('/');
				return pos == std::string::npos ? std::string() : modulePath.substr(0, pos + 1);
			}
			if (path.size() >= maxPathBytes)
				return {};
			path.resize(path.size() > maxPathBytes / 2 ? maxPathBytes : path.size() * 2);
		}
	}();
	return g_bin_dir.c_str();
#else
	static const std::string g_bin_dir = []() -> std::string {
		const std::string& path = getInstPath();
		const std::size_t pos = path.find_last_of('/');
		return pos == std::string::npos ? std::string() : path.substr(0, pos + 1);
	}();

	return g_bin_dir.c_str();
#endif
}
