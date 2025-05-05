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
#include <wtypes.h>
/**
 * @brief Windows平台下DLL模块句柄
 * @details 在Windows平台下存储当前已加载的DLL模块句柄
 */
static HMODULE	g_dllModule = NULL;

/**
 * @brief Windows平台下DLL入口点函数
 * @param hModule 模块句柄
 * @param ul_reason_for_call 调用原因
 * @param lpReserved 保留参数
 * @return BOOL 返回TRUE表示成功
 * @details 当DLL被加载到进程空间时，保存模块句柄以便后续使用
 */
BOOL APIENTRY DllMain(
	HANDLE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		g_dllModule = (HMODULE)hModule;
		break;
	}
	return TRUE;
}
#else
#include <dlfcn.h>
/**
 * @brief Linux/Unix平台下的辅助函数
 * @details 空函数，仅用于获取当前模块的内存地址作为dladdr函数的参数
 */
void inst_hlp() {}
/**
 * @brief 获取当前动态库的完整路径
 * @return const std::string& 返回当前动态库的完整路径
 * @details 在Linux/Unix平台下使用dladdr函数获取当前动态库的完整路径
 *          首次调用时计算路径并缓存，后续调用直接返回缓存值
 */
static const std::string& getInstPath()
{
	static std::string moduleName;
	if (moduleName.empty())
	{
		Dl_info dl_info;
		dladdr((void *)inst_hlp, &dl_info);
		moduleName = dl_info.dli_fname;
	}

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
static const char* getBinDir()
{
	static std::string g_bin_dir;

	if (g_bin_dir.empty())
	{
#ifdef _MSC_VER
		char strPath[MAX_PATH];
		GetModuleFileName(g_dllModule, strPath, MAX_PATH);

		g_bin_dir = StrUtil::standardisePath(strPath, false);
#else
		g_bin_dir = getInstPath();
#endif
		std::size_t nPos = g_bin_dir.find_last_of('/');
		g_bin_dir = g_bin_dir.substr(0, nPos + 1);
	}

	return g_bin_dir.c_str();
}