# CSV historical block type and status fixture

This fixture compiles `WtDtHelper.cpp` as a shared library and calls its actual exported `dump_bars_v2` and `dump_ticks_v2` functions. It creates ordinary local historical bar/tick records and verifies all four defined raw/compressed and old/new payload versions. Each bar version is tested for minute-1, minute-5 and day types. It also checks type-mismatch and short-header input errors, mixed-batch partial failure with a valid output, the BUG-L05-002 (2026-11-04) no-input status for an empty folder or a filter that matches no file (`WT_CSV_DUMP_NO_INPUT`, nothing written), the `strFilter` whole-stem wildcard and case-sensitive matching, and valid zero-record behavior. It does not run hostile malformed-payload or crash tests.

The macOS build links existing `/tmp/wt-i03-r41-drain/libs/libWTSTools.a` and `libWTSUtils.a` plus locally installed dependencies; it is not a fresh full-project build. The fixture and helper library are compiled with UBSan; those reused static dependencies are not. From the repository root, with those dependencies available:

```sh
cmake -S src/TestUnits/fixtures/i03_csv_block_type_20261103 \
  -B /tmp/i03-csv-block-type-build-NEW \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-csv-block-type-build-NEW -j2
UBSAN_OPTIONS=halt_on_error=1 /tmp/i03-csv-block-type-build-NEW/csv_block_check
nm -gU /tmp/i03-csv-block-type-build-NEW/libcsv_block_helper.dylib | rg 'dump_(bars|ticks)(_v2)?$'
```

The earlier `i03_csv_status_20261103` fixture was independently rebuilt and run following its README, with a fresh build and golden output directory and its previously saved `/tmp/i03-csv-status-baseline-20261103/WtDtHelper.cpp`. It passed 14/14, including legacy wrappers, expected output bytes, and controlled I/O failure signaling. These tests do not establish Windows behavior, full regression, or transactional CSV publication. The explicit block-signature and unsupported-version rejection branches are not separately exercised by this bounded fixture.

2026-11-05 (final acceptance, slice2-result): the 2026-11-03 case `EmptyDirectoryAndIgnoredFilter` expected success for an empty folder and an ignored, non-matching filter. Since BUG-L05-002 (2026-11-04) both are `WT_CSV_DUMP_NO_INPUT` and `strFilter` is applied, so the case was replaced by `EmptyDirectoryAndUnmatchedFilterAreNoInput`, which asserts that status, that nothing is written, and which filters accept or reject `kept.dsb`. The runner links the libraries of its `--build` instead of `/tmp/wt-i03-r41-drain/libs`.
