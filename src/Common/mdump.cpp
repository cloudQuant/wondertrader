/*
* $Id: mdump.cpp 5561 2009-12-25 07:23:59Z wangmeng $
*
* this file is part of eMule
* Copyright (C)2002-2006 Merkur ( strEmail.Format("%s@%s", "devteam", "emule-project.net") / http://www.emule-project.net )
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

/*
这段代码实现了一个异常捕获和内存转储功能，主要用于在 Windows 应用程序中捕获未处理的异常，并生成一个内存转储文件，以便开发人员分析和调试崩溃的原因。以下是关键点：

异常捕获：通过 SetUnhandledExceptionFilter 设置顶层异常过滤器。

内存转储：使用 MiniDumpWriteDump 函数生成内存转储文件。

错误报告：调用外部程序 CrashReporter.exe 发送错误报告。

路径处理：生成转储文件的完整路径，并替换文件名中的特殊字符。
 */
#include "mdump.h"
#include <dbghelp.h>   // 用于使用 MiniDumpWriteDump 函数
#include <ShellAPI.h> // 用于调用外部程序（如 CrashReporter.exe）
#include <tchar.h>   // 用于处理宽字符和多字节字符。
#include <stdio.h>  // 用于格式化字符串。

#define ARRSIZE(x)	(sizeof(x)/sizeof(x[0]))  // 计算数组 x 的大小。


// 定义一个函数指针类型，指向 MiniDumpWriteDump 函数。
typedef BOOL(WINAPI *MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
	CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
	CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

CMiniDumper theCrashDumper;
TCHAR CMiniDumper::m_szAppName[MAX_PATH] = { 0 };
TCHAR CMiniDumper::m_szDumpPath[MAX_PATH] = { 0 };

void CMiniDumper::Enable(LPCTSTR pszAppName, bool bShowErrors, LPCTSTR pszDumpPath/* = ""*/)
{
	// if this assert fires then you have two instances of CMiniDumper which is not allowed
	_tcsncpy(m_szAppName, pszAppName, ARRSIZE(m_szAppName));
	_tcsncpy(m_szDumpPath, pszDumpPath, ARRSIZE(m_szDumpPath));

	MINIDUMPWRITEDUMP pfnMiniDumpWriteDump = NULL;
	HMODULE hDbgHelpDll = GetDebugHelperDll((FARPROC*)&pfnMiniDumpWriteDump, bShowErrors);
	if (hDbgHelpDll)
	{
		if (pfnMiniDumpWriteDump)
			SetUnhandledExceptionFilter(TopLevelFilter);
		FreeLibrary(hDbgHelpDll);
		hDbgHelpDll = NULL;
		pfnMiniDumpWriteDump = NULL;
	}
}

HMODULE CMiniDumper::GetDebugHelperDll(FARPROC* ppfnMiniDumpWriteDump, bool bShowErrors)
{
	*ppfnMiniDumpWriteDump = NULL;
	HMODULE hDll = LoadLibrary(_T("DBGHELP.DLL"));
	if (hDll == NULL)
	{
		if (bShowErrors) {
			// Do *NOT* localize that string (in fact, do not use MFC to load it)!
			MessageBox(NULL, _T("DBGHELP.DLL not found. Please install a DBGHELP.DLL."), m_szAppName, MB_ICONSTOP | MB_OK);
		}
	}
	else
	{
		*ppfnMiniDumpWriteDump = GetProcAddress(hDll, "MiniDumpWriteDump");
		if (*ppfnMiniDumpWriteDump == NULL)
		{
			if (bShowErrors) {
				// Do *NOT* localize that string (in fact, do not use MFC to load it)!
				MessageBox(NULL, _T("DBGHELP.DLL found is too old. Please upgrade to a newer version of DBGHELP.DLL."), m_szAppName, MB_ICONSTOP | MB_OK);
			}
		}
	}
	return hDll;
}

LONG CMiniDumper::TopLevelFilter(struct _EXCEPTION_POINTERS* pExceptionInfo)
{
	LONG lRetValue = EXCEPTION_CONTINUE_SEARCH;
	TCHAR szResult[_MAX_PATH + 1024] = { 0 };
	MINIDUMPWRITEDUMP pfnMiniDumpWriteDump = NULL;
	HMODULE hDll = GetDebugHelperDll((FARPROC*)&pfnMiniDumpWriteDump, true);
	HINSTANCE	hInstCrashReporter = NULL;	//ADDED by fengwen on 2006/11/15 : 使用新的发送错误报告机制。

	if (hDll)
	{
		if (pfnMiniDumpWriteDump)
		{
			//MessageBox(NULL,"test","test",MB_OK);
			// Ask user if they want to save a dump file
			// Do *NOT* localize that string (in fact, do not use MFC to load it)!
			//COMMENTED by fengwen on 2006/11/15	<begin> : 使用新的发送错误报告机制。
			//if (MessageBox(NULL, _T("eMule crashed :-(\r\n\r\nA diagnostic file can be created which will help the author to resolve this problem. This file will be saved on your Disk (and not sent).\r\n\r\nDo you want to create this file now?"), m_szAppName, MB_ICONSTOP | MB_YESNO) == IDYES)
			//COMMENTED by fengwen on 2006/11/15	<end> : 使用新的发送错误报告机制。
			{
				// Create full path for DUMP file
				TCHAR szDumpPath[_MAX_PATH] = { 0 };
				if(_tcsclen(m_szDumpPath) == 0)
				{
					GetModuleFileName(NULL, szDumpPath, ARRSIZE(szDumpPath));
					LPTSTR pszFileName = _tcsrchr(szDumpPath, _T('\\'));
					if (pszFileName) {
						pszFileName++;
						*pszFileName = _T('\0');
					}
				}
				else
				{
					_tcsncpy(szDumpPath, m_szDumpPath, _tcsclen(m_szDumpPath));
					szDumpPath[_tcsclen(m_szDumpPath)] = _T('\0');
				}

				// Replace spaces and dots in file name.
				TCHAR szBaseName[_MAX_PATH] = { 0 };
				_tcsncat(szBaseName, m_szAppName, ARRSIZE(szBaseName) - 1);
				LPTSTR psz = szBaseName;
				while (*psz != _T('\0')) {
					if (*psz == _T('.'))
						*psz = _T('-');
					else if (*psz == _T(' '))
						*psz = _T('_');
					psz++;
				}
				_tcsncat(szDumpPath, szBaseName, ARRSIZE(szDumpPath) - 1);
				SYSTEMTIME curTime;
				GetLocalTime(&curTime);
				char buf[64];
				sprintf(buf, "%4.4d%2.2d%2.2d%2.2d%2.2d%2.2d", curTime.wYear, curTime.wMonth, curTime.wDay, curTime.wHour, curTime.wMinute, curTime.wSecond);
				strcat(szDumpPath, buf);

				_tcsncat(szDumpPath, _T(".dmp"), ARRSIZE(szDumpPath) - 1);

				HANDLE hFile = CreateFile(szDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				if (hFile != INVALID_HANDLE_VALUE)
				{
					_MINIDUMP_EXCEPTION_INFORMATION ExInfo = { 0 };
					ExInfo.ThreadId = GetCurrentThreadId();
					ExInfo.ExceptionPointers = pExceptionInfo;
					ExInfo.ClientPointers = NULL;

					BOOL bOK = (*pfnMiniDumpWriteDump)(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &ExInfo, NULL, NULL);
					if (bOK)
					{
						// Do *NOT* localize that string (in fact, do not use MFC to load it)!
						_sntprintf(szResult, ARRSIZE(szResult), _T("Saved dump file to \"%s\".\r\n\r\nPlease send this file together with a detailed bug report to bastet.wang@gmail.com !\r\n\r\nThank you for helping to improve Tsts."), szDumpPath);
						lRetValue = EXCEPTION_EXECUTE_HANDLER;

						//ADDED by fengwen on 2006/11/15	<begin> : 使用新的发送错误报告机制。
						hInstCrashReporter = ShellExecute(NULL, _T("open"), _T("CrashReporter.exe"), szDumpPath, NULL, SW_SHOW);
						if (hInstCrashReporter <= (HINSTANCE)32)
							lRetValue = EXCEPTION_CONTINUE_SEARCH;
						//ADDED by fengwen on 2006/11/15	<end> : 使用新的发送错误报告机制。
					}
					else
					{
						// Do *NOT* localize that string (in fact, do not use MFC to load it)!
						_sntprintf(szResult, ARRSIZE(szResult), _T("Failed to save dump file to \"%s\".\r\n\r\nError: %u"), szDumpPath, GetLastError());
					}
					CloseHandle(hFile);
				}
				else
				{
					// Do *NOT* localize that string (in fact, do not use MFC to load it)!
					_sntprintf(szResult, ARRSIZE(szResult), _T("Failed to create dump file \"%s\".\r\n\r\nError: %u"), szDumpPath, GetLastError());
				}
			}
		}
		FreeLibrary(hDll);
		hDll = NULL;
		pfnMiniDumpWriteDump = NULL;
	}

	//COMMENTED by fengwen on 2006/11/15	<begin> : 使用新的发送错误报告机制。
	//if (szResult[0] != _T('\0'))
	//	MessageBox(NULL, szResult, m_szAppName, MB_ICONINFORMATION | MB_OK);
	//COMMENTED by fengwen on 2006/11/15	<end> : 使用新的发送错误报告机制。

#ifndef _DEBUG
	if (EXCEPTION_EXECUTE_HANDLER == lRetValue)		//ADDED by fengwen on 2006/11/15 : 由此filter处理了异常,才去中止进程。
	{
		// Exit the process only in release builds, so that in debug builds the exceptio is passed to a possible
		// installed debugger
		ExitProcess(0);
	}
	else
		return lRetValue;

#else

	return lRetValue;
#endif
}
