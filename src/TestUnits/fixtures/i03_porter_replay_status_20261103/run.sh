#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$root"
build=${1:-/tmp/wt-i03-porter-status-20261103}
if [[ -e "$build" || -L "$build" ]]; then
  echo "output directory already exists: $build" >&2
  exit 1
fi
mkdir -p "$build/launch-injection"
source_files=(src/WtBtPorter/WtBtPorter.cpp src/WtBtPorter/WtBtPorter.h src/WtBtPorter/WtBtRunner.cpp src/WtBtPorter/WtBtRunner.h src/WtBtPorter/ExpCtaMocker.cpp src/WtBtCore/HisDataReplayer.cpp src/WtBtCore/HisDataReplayer.h src/WtBtCore/CtaMocker.cpp src/WtBtCore/SelMocker.cpp src/WTSTools/WTSBaseDataMgr.cpp src/Share/FeeSchedule.hpp src/Includes/ReplayStatusV1.h src/TestUnits/fixtures/i03_porter_replay_status_20261103/status_export_test.cpp src/TestUnits/fixtures/i03_porter_replay_status_20261103/inject_launch.py)
shasum -a 256 "${source_files[@]}" > "$build/source-before.sha256"
cmake -S src -B "$build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$build/configure.log" 2>&1
cmake --build "$build" --target WtBtPorter -j2 > "$build/build.log" 2>&1
lib="$build/build_x64/Debug/bin/WtBtPorter"
/usr/bin/c++ -std=c++17 -I src -I /opt/homebrew/include -I /opt/local/include -include src/Share/fmt_enum_compat.h src/TestUnits/fixtures/i03_porter_replay_status_20261103/status_export_test.cpp "$lib/libWtBtPorter.dylib" -o "$build/status_export_test"
nm -g "$lib/libWtBtPorter.dylib" | grep ' T _get_backtest_status_v1$' > "$build/export.log"
: > "$build/test.log"
for mode in arguments normal normal-async fee stop callback-throw init-throw; do
  DYLD_LIBRARY_PATH="$lib" "$build/status_export_test" "$mode" | tee -a "$build/test.log"
done
# Test-only copy of the actual runner: inject an allocation failure immediately
# before thread creation, retaining the production catch/termination path.
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/i03_porter_replay_status_20261103/inject_launch.py "$build/launch-injection/WtBtRunner.cpp" | tee "$build/launch-injection/source-check.log"
/usr/bin/c++ -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DWtBtPorter_EXPORTS -I src/WtBtPorter -I /opt/homebrew/include -isystem src -include src/Share/fmt_enum_compat.h -g -std=gnu++17 -arch arm64 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy '-Wno-#pragma-messages' -Werror -c "$build/launch-injection/WtBtRunner.cpp" -o "$build/launch-injection/WtBtRunner.o"
obj="$build/WtBtPorter/CMakeFiles/WtBtPorter.dir"
libs="$build/build_x64/Debug/libs"
/usr/bin/c++ -g -arch arm64 -dynamiclib -Wl,-headerpad_max_install_names -Wl,-fatal_warnings -Wl,-no_warn_duplicate_libraries -o "$build/launch-injection/libWtBtPorterInject.dylib" -install_name @rpath/libWtBtPorterInject.dylib "$obj/ExpCtaMocker.cpp.o" "$obj/ExpHftMocker.cpp.o" "$obj/ExpSelMocker.cpp.o" "$obj/WtBtPorter.cpp.o" "$build/launch-injection/WtBtRunner.o" -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib "$libs/libWtBtCore.a" "$libs/libWTSTools.a" "$libs/libWTSUtils.a" -ldl -lpthread /usr/local/lib/libiconv.dylib
/usr/bin/c++ -std=c++17 -I src -I /opt/homebrew/include -I /opt/local/include -include src/Share/fmt_enum_compat.h src/TestUnits/fixtures/i03_porter_replay_status_20261103/status_export_test.cpp "$build/launch-injection/libWtBtPorterInject.dylib" -o "$build/launch-injection/status_export_test"
DYLD_LIBRARY_PATH="$build/launch-injection" "$build/launch-injection/status_export_test" launch-failure | tee "$build/launch-injection/test.log"
shasum -a 256 "${source_files[@]}" > "$build/source-after.sha256"
cmp "$build/source-before.sha256" "$build/source-after.sha256"
