#!/bin/bash
set -euo pipefail
log=/tmp/wt-p08-side-run/build-v2.log
: > "$log"
compile_object() {
  local target="$1"
  local tag="$2"
  local output="/tmp/wt-p08-v2-${tag}.o"
  local command_file="/tmp/wt-p08-side-run/compile-${tag}.sh"
  printf 'compile target: %s\n' "$target" | tee -a "$log"
  /opt/homebrew/bin/ninja -C build-cov -t commands "$target" |
    sed -E "s|-MT [^ ]+|-MT /tmp/wt-p08-v2-${tag}.d|; s|-MF [^ ]+|-MF /tmp/wt-p08-v2-${tag}.d|; s|-o [^ ]+|-o ${output}|" > "$command_file"
  cat "$command_file" >> "$log"
  /bin/sh < "$command_file" >> "$log" 2>&1
  printf 'compile_exit=0 output=%s\n' "$output" | tee -a "$log"
}
compile_object 'WtBtCore/CMakeFiles/WtBtCore.dir/UftMocker.cpp.o' 'source'
compile_object 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/UftMocker.cpp.o' 'test-UftMocker'
compile_object 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o' 'p08-test'
printf 'link target: build_x64/Debug/bin/TestUnits/CoverageTests_btcore (temporary /tmp binary)\n' | tee -a "$log"
link_command=/tmp/wt-p08-side-run/link-v2.sh
/opt/homebrew/bin/ninja -C build-cov -t commands 'build_x64/Debug/bin/TestUnits/CoverageTests_btcore' |
  tail -1 |
  sed -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o|/tmp/wt-p08-v2-p08-test.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/r191_mocker_residual_test.cpp.o|/tmp/wt-p08-side-r191.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p03_btcore_mockers2/r72_hft_uft_paths_test.cpp.o|/tmp/wt-p08-side-r72.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/UftMocker.cpp.o|/tmp/wt-p08-v2-test-UftMocker.o|g' \
      -e 's|-o build_x64/Debug/bin/TestUnits/CoverageTests_btcore|-o /tmp/wt-p08-side-run/TestUnits/CoverageTests_btcore_p08_side|' > "$link_command"
cat "$link_command" >> "$log"
(cd build-cov && /bin/sh < "$link_command") >> "$log" 2>&1
printf 'temporary_link_exit=0\n' | tee -a "$log"
printf 'Uses existing build-cov objects and libraries for unchanged dependencies; this is not a full CMake target build.\n' | tee -a "$log"
