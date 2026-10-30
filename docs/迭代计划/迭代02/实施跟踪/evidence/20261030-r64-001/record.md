# BUG-R64-001 isolated acceptance record

Repository: `/Users/yunjinqi/Documents/new_projects/wondertrader`
Isolated build directory: `/tmp/wt-r64-build`

## Original full build

Command: `/opt/homebrew/bin/cmake --build /tmp/wt-r64-build --target WTSTools CoverageTests -j4`

Observed output reached `[360/361] Linking CXX executable build_x64/Debug/bin/TestUnits/CoverageTests`, after which the process session completed without an error message. The original full-build stdout and explicit subprocess exit code were not captured to a file, so no formal exit code is claimed for that first run. (The changed `WTSTools/WTSHotMgr.cpp` object and `r56_hotmgr_test.cpp` object both compiled during it.)

## Captured rebuilds

The two subsequent invocations of the same build command completed with `ninja: no work to do` and `BUILD_EXIT_CODE=0`:

- `/tmp/wt-r64-evidence/build-1-recorded.log`
- `/tmp/wt-r64-evidence/build-2.log`

The first attempted log wrapper used zsh's read-only `status` variable after the build completed; that wrapper itself exited 1. The build output showed no work and no compile failure. Preserved at `/tmp/wt-r64-evidence/build-1-wrapper-error.log`; the corrected captured build immediately afterward exited 0.

## Focused test

Command:

`LLVM_PROFILE_FILE=/tmp/wt-r64-evidence/r56-%p.profraw /tmp/wt-r64-build/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_filter='R56HotMgr.*' --gtest_output=xml:/tmp/wt-r64-evidence/r56.xml`

Exit code: 0; 9 active tests passed; one pre-existing disabled test was not run. Full stdout: `/tmp/wt-r64-evidence/r56.stdout.log`; XML: `/tmp/wt-r64-evidence/r56.xml`; profile: `/tmp/wt-r64-evidence/r56-*.profraw`.

This is local macOS arm64 coverage only.
