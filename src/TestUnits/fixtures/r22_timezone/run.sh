#!/bin/bash
# BUG-R22-002 (TimeUtils::getTZOffset first-call cache race) red/green fixture.
# usage: run.sh <new-output-dir>
# Builds with build.sh, then runs the cases of the 2026-11-02 evidence run.py, each in its
# own process with a hard timeout:
#   green  current-tsan race x3, current-asan normal in 10 time zones, cache control and the
#          four controlled failure/retry stages, existing r16 cache/fasttime cases;
#   red    baseline-tsan race (pre-change header) must be reported by TSan as a data race;
#          baseline normal in the same 10 zones is a positive control (old algorithm, same
#          single-threaded result).
# Local probes only: no network, no symbolic links.
set -u
out=${1:?provide a new output directory}
test ! -e "$out" || { echo "output exists: $out" >&2; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
sh "$here/build.sh" "$out/build" > "$out/build.log" 2>&1 || { echo "build failed, see $out/build.log"; exit 3; }
bin="$out/build"
: > "$out/results.txt"
rc=0
# run <name> <expect: pass|race> <binary> <mode> <tz> [epoch] [sanitizer]
run() {
  local name=$1 expect=$2 binary=$3 mode=$4 tz=$5 epoch=${6:-1704067200} san=${7:-}
  local log="$out/$name.log"
  (
    export TZ="$tz" TSAN_OPTIONS=halt_on_error=1:exitcode=66 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    perl -e 'alarm 30; exec @ARGV' "$bin/$binary" "$mode" "$epoch"
  ) > "$log" 2>&1
  local code=$?
  local races reports ok=0
  races=$(grep -c "WARNING: ThreadSanitizer: data race" "$log")
  reports=$(grep -c -E "WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:" "$log")
  if [ "$expect" = pass ]; then
    [ "$code" -eq 0 ] && [ "$reports" -eq 0 ] && ok=1
  else
    [ "$code" -ne 0 ] && [ "$races" -gt 0 ] && ok=1
  fi
  printf '%s expect=%s exit=%s tsan_races=%s sanitizer_reports=%s %s\n' "$name" "$expect" "$code" "$races" "$reports" \
    "$([ $ok = 1 ] && echo OK || echo MISMATCH)" | tee -a "$out/results.txt"
  [ $ok = 1 ] || rc=1
}
run baseline-tsan-race race baseline-tsan race Asia/Shanghai
for i in 0 1 2; do run current-tsan-race-$i pass current-tsan race Asia/Shanghai; done
while read -r tz stamp label; do
  run normal-$label pass current-asan normal "$tz" "$stamp"
  run baseline-normal-$label pass baseline-tsan normal "$tz" "$stamp"
done <<'ZONES'
UTC 1704067200 utc
Asia/Shanghai 1704067200 shanghai
America/New_York 1704067200 ny-winter
America/New_York 1720000000 ny-summer
Asia/Kolkata 1704067200 kolkata
America/St_Johns 1704067200 st-johns
Pacific/Kiritimati 1704067200 kiritimati
Pacific/Honolulu 1704067200 honolulu
Asia/Shanghai 1735689599 shanghai-new-year
America/New_York 1704067200 ny-prior-year
ZONES
run cache pass current-asan cache America/New_York
for stage in 1 2 3 4; do run failure-$stage pass current-asan $stage Asia/Shanghai; done
(
  export TZ=America/New_York
  perl -e 'alarm 60; exec @ARGV' "$bin/r16-existing" '--gtest_filter=r16_timeutils.fasttime*:r16_timeutils.tz_offset_is_cached_and_in_whole_hours' \
    --gtest_output=xml:"$out/r16-existing.xml"
) > "$out/r16-existing.log" 2>&1
code=$?
printf 'r16-existing expect=pass exit=%s %s\n' "$code" "$([ $code -eq 0 ] && echo OK || echo MISMATCH)" | tee -a "$out/results.txt"
[ $code -eq 0 ] || rc=1
find "$out" -type l -print > "$out/symlinks.txt"
[ -s "$out/symlinks.txt" ] && rc=1
exit $rc
