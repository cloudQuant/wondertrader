#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

# This is an incremental focused build. It intentionally does not build the
# CoverageTests aggregate target or the P04 replayer test.
/opt/homebrew/bin/ninja -C build-cov -j 2 \
  WtBtCore/CMakeFiles/WtBtCore.dir/MatchEngine.cpp.o \
  WtDataStorage/CMakeFiles/WtDataStorage.dir/WtRdmDtReader.cpp.o \
  WtDataStorageAD/CMakeFiles/WtDataStorageAD.dir/WtDataReaderAD.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/l06_wtbtcore/l06_match_engine_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/MatchEngine.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r23_storage4/r23_rdm_count_slice_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtRdmDtReader.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtDataReader.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r12_data_def_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/p12_datafactory_datadef/p12_factory_extract_merge_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_adstorage.dir/p05_datastorage2/r4_reader_ad_slices_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_adstorage.dir/__/__/WtDataStorageAD/WtDataReaderAD.cpp.o

(
  cd build-cov
  common_args=(
    /opt/homebrew/lib/libgtest_main.a
    /opt/homebrew/lib/libgtest.a
    -L/opt/homebrew/lib -L/usr/local/lib
    -lboost_filesystem -lboost_thread -lzstd -liconv -lpthread
  )

  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/l06_wtbtcore/l06_match_engine_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/MatchEngine.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o \
    build_x64/Debug/libs/libWtBtCore.a \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-match-batch-v5

  # WtDataReader.cpp.o is required to resolve proc_block_data in this isolated
  # RDM test link. The helper object is deliberately explicit.
  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r23_storage4/r23_rdm_count_slice_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtRdmDtReader.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtDataReader.cpp.o \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-rdm-batch-v5

  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r12_data_def_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/p12_datafactory_datadef/p12_factory_extract_merge_test.cpp.o \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-extrema-batch-v5

  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests_adstorage.dir/p05_datastorage2/r4_reader_ad_slices_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_adstorage.dir/__/__/WtDataStorageAD/WtDataReaderAD.cpp.o \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-adreader-batch-v5
)

build-cov/build_x64/Debug/bin/TestUnits/wt-match-batch-v5 --gtest_color=no
build-cov/build_x64/Debug/bin/TestUnits/wt-rdm-batch-v5 --gtest_color=no
build-cov/build_x64/Debug/bin/TestUnits/wt-extrema-batch-v5 --gtest_color=no \
  --gtest_filter='r12_value_array.*:r12_kline_slice.*:r12_kline_data.*:P12Extract.closed_extrema_ranges_reject_end_equal_size'
build-cov/build_x64/Debug/bin/TestUnits/wt-adreader-batch-v5 --gtest_color=no
