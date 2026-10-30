#!/bin/bash
set -o pipefail
compile_object() {
  local target="$1"
  local tag="$2"
  /opt/homebrew/bin/ninja -C build-cov -t commands "$target" |
    sed -E "s|-MT [^ ]+|-MT /tmp/wt-p08-side-${tag}.d|; s|-MF [^ ]+|-MF /tmp/wt-p08-side-${tag}.d|; s|-o [^ ]+|-o /tmp/wt-p08-side-${tag}.o|" |
    /bin/sh
}
compile_object 'WtBtCore/CMakeFiles/WtBtCore.dir/UftMocker.cpp.o' 'src'
compile_object 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/UftMocker.cpp.o' 'test-UftMocker'
compile_object 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o' 'p08'
compile_object 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/r191_mocker_residual_test.cpp.o' 'r191'
compile_object 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p03_btcore_mockers2/r72_hft_uft_paths_test.cpp.o' 'r72'
/opt/homebrew/bin/ninja -C build-cov -t commands 'build_x64/Debug/bin/TestUnits/CoverageTests_btcore' |
  tail -1 |
  sed -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o|/tmp/wt-p08-side-p08.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/r191_mocker_residual_test.cpp.o|/tmp/wt-p08-side-r191.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p03_btcore_mockers2/r72_hft_uft_paths_test.cpp.o|/tmp/wt-p08-side-r72.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/UftMocker.cpp.o|/tmp/wt-p08-side-test-UftMocker.o|g' \
      -e 's|-o build_x64/Debug/bin/TestUnits/CoverageTests_btcore|-o /tmp/wt-p08-side-run/TestUnits/CoverageTests_btcore_p08_side|' |
  (cd build-cov && /bin/sh)
