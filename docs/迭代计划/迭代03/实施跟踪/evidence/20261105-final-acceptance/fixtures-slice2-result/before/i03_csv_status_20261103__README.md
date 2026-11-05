# CSV export status channel probe

This standalone macOS fixture builds the current helper and a saved pre-change helper source in independent targets, and compares normal bar/tick CSV bytes produced by the new API and legacy wrappers with those produced by the saved source. It also tests argument and directory errors, read and close hooks, output open and close errors, RLIMIT_FSIZE-induced output failure, and a batch containing a valid and a malformed file, both with and without a logger where applicable. The hooks simulate a failure after the actual close. The library and test translation units use ASan/UBSan; the reused static dependencies and system libraries are not fully instrumented. A separate shared-library target without test hooks checks exported symbols.

Pre-change snapshots for this run: `/tmp/i03-csv-status-baseline-20261103/WtDtHelper.cpp` SHA-256 `676a86c22aa1a8764c1ca99d431ccc8b62de42e56575ee4c6cdf135b487d2116`; `WtDtHelper.h` SHA-256 `7a99af88ac350f96aca795786414827af955d5e40d846ab12572447c52fbbdcf`. The source was already modified before this task. The fixture compiles the saved source against the current compatible header. The CMake file references the same locally installed dependencies and previously built static libraries as `l05_csv_failure_signal`; it is not a clean full-project build.

```
cmake -S src/TestUnits/fixtures/i03_csv_status_20261103 -B /tmp/i03-csv-status-build-20261103 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -DBASELINE_SOURCE=/tmp/i03-csv-status-baseline-20261103/WtDtHelper.cpp -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-csv-status-build-20261103 -j2
mkdir /tmp/i03-csv-status-golden-20261103
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 LLVM_PROFILE_FILE=/tmp/i03-csv-status-baseline-%p.profraw /tmp/i03-csv-status-build-20261103/csv_baseline /tmp/i03-csv-status-golden-20261103
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 LLVM_PROFILE_FILE=/tmp/i03-csv-status-%p.profraw CSV_BASELINE_DIR=/tmp/i03-csv-status-golden-20261103 /tmp/i03-csv-status-build-20261103/csv_status
nm -gU /tmp/i03-csv-status-build-20261103/libcsv_status_exports.dylib | rg 'dump_(bars|ticks)'
```

This does not establish transactional publication, real kernel/storage failure behavior, concurrent directory mutation, Windows compatibility, caller migration, or complete regression coverage. No symlinks or live trading were used.
