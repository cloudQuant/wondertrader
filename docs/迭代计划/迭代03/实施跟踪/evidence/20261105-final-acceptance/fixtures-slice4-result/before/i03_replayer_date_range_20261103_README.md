# HFT trading-date range checked mapping

The standalone fixture exercises `get_hft_trading_date_range` using its pre-existing friend test accessor. It compiles the current `HisDataReplayer.cpp` and `WTSBaseDataMgr.cpp` directly; other static dependencies come from the existing `/tmp/ctpm-i03-build/build_x64/Debug/libs`. The test and these two current translation units use ASan/UBSan; reused libraries and system dependencies are not all instrumented.

Normal ordinary, night, and 7×24 date mappings, missing commodity/session/calendar, invalid endpoints, out-of-range mapping, all four subscription categories, output preservation, and mixed subscriptions are covered. In particular, a valid commodity now requires its configured calendar even when the old mapping could return a natural date without consulting it. A failure log contains the subscription code and the numeric `DateMappingStatus` from `DateMappingV1.h`. No subscriptions returns false and preserves both output arguments.

The pre-change already-dirty replayer source is saved as `/tmp/i03-replayer-date-range-baseline-20261103/HisDataReplayer.cpp` with SHA-256 `21899ae8eec556d9bc7a2ce1eb89a78170db6c2ada05ed24e174074c55547e6f`; the actual source and evidence hashes are in `evidence/source.sha256`. The ASan/UBSan run recorded in `evidence/test.log` passed 20/20 with no skips.

```
cmake -S src/TestUnits/fixtures/i03_replayer_date_range_20261103 -B /tmp/i03-replayer-date-range-build-20261103 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-replayer-date-range-build-20261103 -j2
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 LLVM_PROFILE_FILE=/tmp/i03-replayer-date-range-%p.profraw /tmp/i03-replayer-date-range-build-20261103/i03_replayer_date_range --gtest_color=no
```

This tests range selection, not end-to-end replay, provider data, other date-mapping consumers, Windows, or the full regression suite; it does not close the broader date-mapping issue. No symbolic links or live provider actions were used.
