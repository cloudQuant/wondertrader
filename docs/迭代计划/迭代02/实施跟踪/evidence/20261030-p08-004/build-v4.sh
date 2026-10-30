#!/bin/bash
set -euo pipefail
log=/tmp/wt-p08-side-run/build-v4.log
: > "$log"
output=/tmp/wt-p08-v4-p08-test.o
command_file=/tmp/wt-p08-side-run/compile-v4-p08-test.sh
printf 'compile target: p08_uft_order_position_test.cpp\n' | tee -a "$log"
/opt/homebrew/bin/ninja -C build-cov -t commands 'TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o' |
  sed -E "s|-MT [^ ]+|-MT /tmp/wt-p08-v4-p08-test.d|; s|-MF [^ ]+|-MF /tmp/wt-p08-v4-p08-test.d|; s|-o [^ ]+|-o ${output}|" > "$command_file"
cat "$command_file" >> "$log"
/bin/sh < "$command_file" >> "$log" 2>&1
printf 'compile_exit=0 output=%s\n' "$output" | tee -a "$log"
printf 'link temporary focused binary with rebuilt P08 test TU and unchanged previously compiled dependencies\n' | tee -a "$log"
link_command=/tmp/wt-p08-side-run/link-v4.sh
/opt/homebrew/bin/ninja -C build-cov -t commands 'build_x64/Debug/bin/TestUnits/CoverageTests_btcore' |
  tail -1 |
  sed -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/p08_uft_order_position_test.cpp.o|/tmp/wt-p08-v4-p08-test.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p08_btcore_mockers3/r191_mocker_residual_test.cpp.o|/tmp/wt-p08-side-r191.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p03_btcore_mockers2/r72_hft_uft_paths_test.cpp.o|/tmp/wt-p08-side-r72.o|g' \
      -e 's|TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/UftMocker.cpp.o|/tmp/wt-p08-v2-test-UftMocker.o|g' \
      -e 's|-o build_x64/Debug/bin/TestUnits/CoverageTests_btcore|-o /tmp/wt-p08-side-run/TestUnits/CoverageTests_btcore_p08_side|' > "$link_command"
cat "$link_command" >> "$log"
(cd build-cov && /bin/sh < "$link_command") >> "$log" 2>&1
printf 'temporary_link_exit=0\n' | tee -a "$log"
printf 'Not a full CMake target build; unchanged dependencies are reused from build-cov.\n' | tee -a "$log"
