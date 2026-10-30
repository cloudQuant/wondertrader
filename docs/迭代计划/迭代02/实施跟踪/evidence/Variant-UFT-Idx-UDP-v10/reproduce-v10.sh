#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

# Rebuild only the objects used by these local focused links. This is not a
# clean build or an aggregate CoverageTests link.
/opt/homebrew/bin/ninja -C build-cov -j 2 \
  build_x64/Debug/libs/libWTSUtils.a \
  build_x64/Debug/libs/libWtUftCore.a \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/l02_wtstools_mq/l02_variant_collection_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r12_variant_more_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r195_cfg_variant_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_idxfactory.dir/n03b_idxfactory/n03_idxfactory_worker_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_idxfactory.dir/__/__/WtIndexFactory/WtBlkIdxWorker.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_idxfactory.dir/__/__/WtIndexFactory/WtIndexFactory.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/q04_uftcore3/r54_uft_tick_metadata_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/q04_uftcore3/r123_uft_ticker_engine_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/ParserAdapter.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/UftStraContext.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtUftDtMgr.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtUftEngine.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtUftTicker.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtHelper.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_t_ParserUDP.dir/t_ParserUDP/r76_udp_boundary_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_t_ParserUDP.dir/t_ParserUDP/udp_handle_write_lock_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_t_ParserUDP.dir/__/__/ParserUDP/ParserUDP.cpp.o \
  WTSUtils/CMakeFiles/WTSUtils.dir/WTSCfgLoader.cpp.o \
  TestUnits/CMakeFiles/TestUnits.dir/test_wtsvariant.cpp.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest-death-test.cc.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest-filepath.cc.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest-port.cc.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest-printers.cc.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest-test-part.cc.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest-typed-test.cc.o \
  TestUnits/CMakeFiles/TestUnits.dir/gtest/gtest.cc.o

focused_prefix=(/usr/bin/c++ -arch arm64 -fprofile-instr-generate -g -Wl,-undefined,dynamic_lookup)
focused_suffix=(/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
  build_x64/Debug/libs/libWTSTools.a build_x64/Debug/libs/libWTSUtils.a
  -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -liconv -lpthread)

(
  cd build-cov
  "${focused_prefix[@]}" \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/q04_uftcore3/r54_uft_tick_metadata_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/q04_uftcore3/r123_uft_ticker_engine_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/ParserAdapter.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/UftStraContext.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtUftDtMgr.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtUftEngine.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtUftTicker.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/__/__/WtUftCore/WtHelper.cpp.o \
    build_x64/Debug/libs/libWtUftCore.a "${focused_suffix[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-uft-batch-v10

  "${focused_prefix[@]}" \
    TestUnits/coverage/CMakeFiles/CoverageTests_idxfactory.dir/n03b_idxfactory/n03_idxfactory_worker_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_idxfactory.dir/__/__/WtIndexFactory/WtBlkIdxWorker.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_idxfactory.dir/__/__/WtIndexFactory/WtIndexFactory.cpp.o \
    "${focused_suffix[@]}" -o build_x64/Debug/bin/TestUnits/wt-idx-batch-v10

  "${focused_prefix[@]}" \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/l02_wtstools_mq/l02_variant_collection_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r12_variant_more_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r195_cfg_variant_test.cpp.o \
    "${focused_suffix[@]}" -o build_x64/Debug/bin/TestUnits/wt-variant-batch-v10

  "${focused_prefix[@]}" \
    TestUnits/coverage/CMakeFiles/CoverageTests_t_ParserUDP.dir/t_ParserUDP/r76_udp_boundary_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_t_ParserUDP.dir/t_ParserUDP/udp_handle_write_lock_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_t_ParserUDP.dir/__/__/ParserUDP/ParserUDP.cpp.o \
    "${focused_suffix[@]}" -o build_x64/Debug/bin/TestUnits/wt-udp-batch-v10
)

build-cov/build_x64/Debug/bin/TestUnits/wt-uft-batch-v10
build-cov/build_x64/Debug/bin/TestUnits/wt-idx-batch-v10
build-cov/build_x64/Debug/bin/TestUnits/wt-variant-batch-v10 \
  --gtest_filter='L02VariantFixture.VariantCrossTypeConversions:r12_variant_object.append_and_read_each_scalar_type:r12_variant_real.*:r12_variant_conversions.cross_type_numeric_conversion:r195_cfg_variant.json_all_number_types_and_nested_containers'
build-cov/build_x64/Debug/bin/TestUnits/wt-udp-batch-v10

# Keep this compatibility check on the repository's older bundled GTest. Do
# not mix these objects with Homebrew's libgtest/libgtest_main.
/usr/bin/c++ -std=c++17 -arch arm64 -I src/TestUnits -I src/TestUnits/gtest \
  -c "$(dirname "$0")/legacy_gtest_main.cpp" -o /tmp/wt-legacy-gtest-main-v11.o
/usr/bin/c++ -arch arm64 -fprofile-instr-generate -g \
  /tmp/wt-legacy-gtest-main-v11.o \
  build-cov/TestUnits/CMakeFiles/TestUnits.dir/test_wtsvariant.cpp.o \
  build-cov/TestUnits/CMakeFiles/TestUnits.dir/gtest/*.o \
  -lpthread -o /tmp/wt-variant-legacy-v11
/tmp/wt-variant-legacy-v11
