#pragma once

using DWORD = unsigned long;
using BOOL = int;
using LPCSTR = const char*;
using HMODULE = void*;

constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
constexpr int MAX_PATH = 260;

BOOL GetModuleHandleExA(DWORD flags, LPCSTR address, HMODULE* module);
DWORD GetModuleFileNameA(HMODULE module, char* path, DWORD size);
