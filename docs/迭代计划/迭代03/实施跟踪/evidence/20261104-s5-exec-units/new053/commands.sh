#!/bin/bash
# NEW-053 根因确证（2026-11-04）。最小复现：Homebrew LLVM 22.1.8。
set -u
CC=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang
$CC -g -fprofile-instr-generate -fcoverage-mapping -shared -o libnew053plugin.dylib plugin.c
$CC -g -shared -o libnew053plugin_noprof.dylib plugin.c
$CC -g -fsanitize=address -fno-omit-frame-pointer -o host_asan host.c
$CC -g -fsanitize=address -fno-omit-frame-pointer -o host_sym host_sym.c
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 LSAN_OPTIONS=exitcode=23:report_objects=1
P=$PWD/run/prof/p-%p.profraw
env LLVM_PROFILE_FILE=$P ./host_asan ./libnew053plugin.dylib 1        # A: 插桩模块+环境变量+dlclose -> exit 23, len+1 字节
env -u LLVM_PROFILE_FILE ./host_asan ./libnew053plugin.dylib 1        # B: 无环境变量 -> exit 0
env LLVM_PROFILE_FILE=$P ./host_asan ./libnew053plugin.dylib 0        # C: 不 dlclose -> exit 0
env LLVM_PROFILE_FILE=$P ./host_asan ./libnew053plugin_noprof.dylib 1 # D: 非插桩模块 -> exit 0
env LLVM_PROFILE_FILE=$PWD/run/prof/a-much-longer-profile-path-name-to-change-the-size-%p.profraw ./host_asan ./libnew053plugin.dylib 1  # E
env LLVM_PROFILE_FILE=$P ./host_sym ./libnew053plugin.dylib           # F: 打印模块基址，随后 atos -l <base> 符号化泄漏栈
# 原用例（只读运行 reverify 的 ASan 覆盖率二进制，独立 cwd）：
BIN=/tmp/wt-i03-reverify-20261103/build-asan/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore
T=P11TraderTest.init_rejects_overlong_ids_and_keeps_empty_id_accepted
# short/long 两种 LLVM_PROFILE_FILE 长度与不设变量各一次，见 wt/<variant>/
