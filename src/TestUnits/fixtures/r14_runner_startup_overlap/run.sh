#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
out=$(cd "$out" && pwd)
cd "$root"
old=/tmp/wt-i03-r14-010-20261102
snap="$old/snapshot/src"
build="$old/build"
for file in TestUnits/coverage/m_WtUftRunner/r95_wtuftrunner_config_run_test.cpp TestUnits/coverage/m_WtUftRunner/r220_uft_runner_sink_shutdown_test.cpp; do
  cmp "src/$file" "$snap/$file"
done
runner_source=${R14_BASELINE:-src/WtUftRunner}
shasum -a 256 "$runner_source/WtUftRunner.cpp" "$runner_source/WtUftRunner.h" src/TestUnits/fixtures/r14_runner_startup_overlap/probe.inc src/TestUnits/fixtures/r14_runner_startup_overlap/run.sh > "$out/source-before.sha256"
cp -R "$snap" "$out/src"
cp "$runner_source/WtUftRunner.cpp" "$runner_source/WtUftRunner.h" "$out/src/WtUftRunner/"
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python - "$out/src" "$root/src/TestUnits/fixtures/r14_runner_startup_overlap/probe.inc" <<'PY'
import sys
from pathlib import Path
src=Path(sys.argv[1]); probe=Path(sys.argv[2]); runner=src/'WtUftRunner/WtUftRunner.cpp'
text=runner.read_text(encoding='utf-8-sig')
needle='\t\t_uft_engine.run();'
assert text.count(needle)==1
text='extern "C" void r14_startup_overlap_hook();\n'+text.replace(needle,'\t\tr14_startup_overlap_hook();\n'+needle)
runner.write_text(text)
test=src/'TestUnits/coverage/m_WtUftRunner/r95_wtuftrunner_config_run_test.cpp'
with test.open('a') as f: f.write(probe.read_text())
PY
test -z "$(find "$out/src" -type l -print -quit)"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
common=(-std=c++17 -g -O1 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/include
  -I"$out/src/Includes" -I"$out/src/Share" -I"$out/src/WTSTools" -I"$out/src/WTSUtils"
  -isystem "$out/src" -isystem /opt/homebrew/Cellar/googletest/1.17.0/include
  -include stdlib.h -include "$out/src/Share/fmt_enum_compat.h" -fprofile-instr-generate -fcoverage-mapping
  -DWT_R95_PARSER_PLUGIN_PATH=\"$build/TestUnits/coverage/r130_parser_plugins/libR130LifecycleParser.dylib\"
  -DWT_R95_UFT_STRATEGY_PLUGIN_PATH=\"$build/TestUnits/coverage/r95_uft_strategy_fixture/libr95_uft_strategy.dylib\"
  -DWT_UFT_LIFECYCLE_FAKE_PLUGIN_PATH=\"$build/TestUnits/coverage/uft_lifecycle_plugins/libUftLifecycleFakeTrader.dylib\")
for variant in normal asan tsan; do
  flags=(-O1)
  if [[ "$variant" == asan ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
  if [[ "$variant" == tsan ]]; then flags=(-fsanitize=thread -fno-omit-frame-pointer); fi
  "$compiler" "${common[@]}" "${flags[@]}" -Dinst_hlp=cov_inst_hlp_1721ec9e -c "$out/src/TestUnits/coverage/m_WtUftRunner/r95_wtuftrunner_config_run_test.cpp" -o "$out/$variant-test.o" > "$out/$variant-build.log" 2>&1
  "$compiler" "${common[@]}" "${flags[@]}" -Dinst_hlp=cov_inst_hlp_r14_extra -c "$out/src/TestUnits/coverage/m_WtUftRunner/r220_uft_runner_sink_shutdown_test.cpp" -o "$out/$variant-r220.o" >> "$out/$variant-build.log" 2>&1
  "$compiler" "${common[@]}" "${flags[@]}" -Dinst_hlp=cov_inst_hlp_b0902cea -c "$out/src/WtUftRunner/WtUftRunner.cpp" -o "$out/$variant-runner.o" >> "$out/$variant-build.log" 2>&1
  "$compiler" "${flags[@]}" -fprofile-instr-generate "$out/$variant-test.o" "$out/$variant-r220.o" "$out/$variant-runner.o" \
    "$build/TestUnits/coverage/CMakeFiles/CoverageTests_m_WtUftRunner.dir/__/__/WtUftRunner/main.cpp.o" \
    -o "$out/$variant-probe" -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib \
    /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a \
    "$build/build_x64/Debug/libs/libWtUftCore.a" "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
    -ldl -lpthread -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib \
    /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a >> "$out/$variant-build.log" 2>&1
  for case in shutdown_during_engine_startup_is_pending concurrent_run_is_rejected_before_engine_startup startup_thread_shutdown_is_pending_and_control_retry_succeeds sequential_run_is_rejected_before_engine_startup failed_configuration_closes_instance_and_shutdown_retry_is_safe startup_exception_releases_lease_for_shutdown_retry pending_shutdown_is_completed_after_startup_exception; do
    set +e
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 LLVM_PROFILE_FILE="$out/$variant-$case.profraw" \
      perl -e 'alarm 30; exec @ARGV' "$out/$variant-probe" --gtest_filter="r14_startup_overlap.$case" > "$out/$variant-$case.log" 2>&1
    status=$?
    set -e
    printf '%s %s %s\n' "$variant" "$case" "$status" >> "$out/results.txt"
  done
  for case in r95_uft_run.async_mode_returns_after_startup_and_owner_can_shutdown r95_uft_run.synchronous_mode_waits_for_exit_signal WtUftRunnerParserStart.shutdownWaitsForParserStartAndDoesNotHoldGateLockInPlugin WtUftRunnerParserStart.sameThreadShutdownReentryDoesNotWaitOnItself WtUftRunnerParserStart.parserCallbackShutdownDoesNotWaitForConnectThread; do
    set +e
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 LLVM_PROFILE_FILE="$out/$variant-$case.profraw" \
      perl -e 'alarm 30; exec @ARGV' "$out/$variant-probe" --gtest_filter="$case" > "$out/$variant-$case.log" 2>&1
    status=$?
    set -e
    printf '%s %s %s\n' "$variant" "$case" "$status" >> "$out/results.txt"
  done
done
shasum -a 256 "$runner_source/WtUftRunner.cpp" "$runner_source/WtUftRunner.h" src/TestUnits/fixtures/r14_runner_startup_overlap/probe.inc src/TestUnits/fixtures/r14_runner_startup_overlap/run.sh > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
cat "$out/results.txt"
if [[ -z ${R14_BASELINE:-} ]]; then
  awk '$3 != 0 { bad=1 } END { exit bad }' "$out/results.txt"
fi
