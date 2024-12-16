/*
* $Id: mdump.h 5561 2009-12-25 07:23:59Z wangmeng $
*
* this file is part of easyMule
* Copyright (C)2002-2008 VeryCD Dev Team ( strEmail.Format("%s@%s", "emuledev", "verycd.com") / http: * www.easymule.org )
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either
* version 2 of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/


#pragma once
#include <Windows.h>
// 声明 _EXCEPTION_POINTERS 结构体，用于存储异常信息。
struct _EXCEPTION_POINTERS;

/*
用于在 Windows 应用程序中捕获未处理的异常,
并生成一个内存转储文件（minidump),
以便开发人员分析和调试崩溃的原因。
 */
class CMiniDumper
{
public:

	// 启用异常捕获功能，并设置应用程序名称和转储文件路径。
	static void Enable(LPCTSTR pszAppName, bool bShowErrors, LPCTSTR pszDumpPath = "");

private:
    // 存储应用程序名称。
	static TCHAR m_szAppName[MAX_PATH];
    // 存储转储文件的保存路径。
	static TCHAR m_szDumpPath[MAX_PATH];
	// 加载 DBGHELP.DLL 动态链接库，并获取 MiniDumpWriteDump 函数的地址。
	static HMODULE GetDebugHelperDll(FARPROC* ppfnMiniDumpWriteDump, bool bShowErrors);
	// 顶层异常过滤器，用于捕获未处理的异常并生成转储文件。
    static LONG WINAPI TopLevelFilter(struct _EXCEPTION_POINTERS* pExceptionInfo);
};
// 全局实例，用于启用异常捕获功能。
extern CMiniDumper theCrashDumper;
