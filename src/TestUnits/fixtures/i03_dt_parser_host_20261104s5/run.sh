#!/bin/bash
# Builds and runs the data-kit parser host lifecycle fixture (iteration 03, card 9).
# usage: run.sh <wt-coverage-build-dir> <new-fixture-build-dir> <before-dir> [thread]
#   wt-coverage-build-dir : coverage build providing build_x64/Debug/libs/{libWtDtCore,libWTSTools,libWTSUtils}.a
#   before-dir            : directory holding src/WtDtCore/ParserAdapter.* and src/WtDtServo/{ParserAdapter,WtDtRunner}.*
#                           pre-change copies (red controls); pass "" to skip the baselines
#   thread                : optional; builds with Homebrew LLVM -fsanitize=thread
set -u
WT=$1; B=$2; BEFORE=${3:-}; SAN=${4:-}
here="$(cd "$(dirname "$0")" && pwd)"
extra=()
if [ "$SAN" = thread ]; then
  extra=(-DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ "-DCMAKE_CXX_FLAGS=-include stdlib.h" -DWT_SANITIZE=thread)
fi
# ${extra[@]+...}: macOS /bin/bash 3.2 treats an empty array as unbound under set -u.
cmake -S "$here" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_BUILD="$WT" -DWT_BEFORE="$BEFORE" ${extra[@]+"${extra[@]}"} || exit 1
cmake --build "$B" -j 3 || exit 1
export I03_DT_HOST_PARSER_DIR="$B/parsers" LLVM_PROFILE_FILE=/dev/null
[ "$SAN" = thread ] && export TSAN_OPTIONS=halt_on_error=1:report_thread_leaks=0
mkdir -p "$B/run" && cd "$B/run" || exit 1
rc=0
for t in dtcore_host_cur servo_host_cur dtporter_host_cur; do
  perl -e 'alarm 120; exec @ARGV' "$B/$t" --gtest_output=xml:"$B/run/$t.xml" > "$B/run/$t.log" 2>&1; ec=$?
  echo "$t exit=$ec"; [ $ec -ne 0 ] && rc=1
done
for m in self self-external-join exception; do
  perl -e 'alarm 60; exec @ARGV' "$B/monitor_writer_probe" "$m" "$B" > "$B/run/monitor-$m.log" 2>&1; ec=$?
  echo "monitor_writer_probe $m exit=$ec"; [ $ec -ne 0 ] && rc=1
done
# Baselines are red controls: each case is expected to fail or crash.
for t in dtcore_host_base servo_host_base dtporter_host_base; do
  [ -x "$B/$t" ] || continue
  for c in $("$B/$t" --gtest_list_tests | awk '/^  /{print s $1} /^[^ ]/{s=$1}'); do
    perl -e 'alarm 30; exec @ARGV' "$B/$t" --gtest_filter="$c" > "$B/run/$t-$c.log" 2>&1
    echo "$t $c exit=$? (red control: non-zero expected)"
  done
done
exit $rc
