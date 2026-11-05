#!/bin/bash
# usage: run.sh <workdir> <label> <libdir> <header-src> <asan|tsan>
#   Links the I03-NEW-065 notifier test and the I03-NEW-066 SEL schedule test
#   (src/TestUnits/coverage/q01_wtcore3/s5_sel_schedule_thread_test.cpp) against
#   one sanitizer build of libWtCore/libWTSTools/libWTSUtils (<libdir>) whose
#   headers are under <header-src>, builds the fake MQ modules and runs each
#   case in its own cwd under a 120 s alarm. No symbolic links are created.
set -uo pipefail
fx=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$fx/../../.." && pwd -P)
work=${1:?workdir}; label=${2:?label}; libdir=${3:?libdir}; hdr=${4:?header-src}; san=${5:?asan|tsan}
llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
if [[ $san == tsan ]]; then sflags="-fsanitize=thread -fno-omit-frame-pointer -g"; else sflags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"; fi
mkdir -p "$work"; work=$(cd "$work" && pwd -P)
common=(-std=gnu++17 $sflags -include cstdlib -include "$hdr/Share/fmt_enum_compat.h" -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL
  -I "$hdr" -I "$hdr/Includes" -I "$hdr/Share" -I "$hdr/WTSTools" -I "$hdr/WTSUtils"
  -isystem /opt/homebrew/opt/boost/include -isystem /usr/local/include -isystem /opt/homebrew/include)
libs=("$libdir/libWtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
  -L /opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread -lboost_system /usr/local/lib/libiconv.2.dylib -pthread)
$llvm/clang++ "${common[@]}" "$fx/notifier_test.cpp" -o "$work/$label-notifier" "${libs[@]}" > "$work/$label-notifier-link.log" 2>&1 || { echo "notifier link failed"; tail -5 "$work/$label-notifier-link.log"; exit 11; }
$llvm/clang++ "${common[@]}" -I "$repo/TestUnits/coverage/q01_wtcore3" "$repo/TestUnits/coverage/q01_wtcore3/s5_sel_schedule_thread_test.cpp" \
  -o "$work/$label-sel" "${libs[@]}" > "$work/$label-sel-link.log" 2>&1 || { echo "sel link failed"; tail -5 "$work/$label-sel-link.log"; exit 12; }
for v in zero ok; do
  d="$work/$label-mq-$v"; mkdir -p "$d"
  id=0; [[ $v == ok ]] && id=7
  $llvm/clang++ -std=c++17 $sflags -fPIC -dynamiclib -DFAKE_CREATE_ID=${id}u "$fx/fake_mq.cpp" -o "$d/libWtMsgQue.dylib" || exit 13
done
# K24=E1: blocking-publisher variant (create id 7).
d="$work/$label-mq-block"; mkdir -p "$d"
$llvm/clang++ -std=c++17 $sflags -fPIC -dynamiclib -DFAKE_CREATE_ID=7u -DFAKE_BLOCK_PUBLISH "$fx/fake_mq.cpp" -o "$d/libWtMsgQue.dylib" || exit 14
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
shasum -a 256 "$work/$label-notifier" "$work/$label-sel" "$libdir"/*.a > "$work/$label-binaries.sha256"
: > "$work/$label-results.txt"
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
run() { # <binary> <filter> <cwd> <leaks>
  local out="$work/$label-$2"; out=${out//\*/_}
  (cd "$3" && ASAN_OPTIONS=detect_leaks=$4:halt_on_error=1 perl -e 'alarm shift; exec @ARGV or exit 127' 120 "$1" \
     --gtest_filter="$2" --gtest_output=xml:"$out.xml" > "$out.log" 2>&1)
  local rc=$?
  local reports; reports=$(grep -c "ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error\|WARNING: ThreadSanitizer" "$out.log")
  echo "$2 exit=$rc sanitizer_reports=$reports" | tee -a "$work/$label-results.txt"
}
run "$work/$label-notifier" S5EventNotifierInit.CreateServerFailureIsReportedAndStartsNothing "$work/$label-mq-zero" 1
run "$work/$label-notifier" S5EventNotifierInit.SuccessfulCreateStillPublishes "$work/$label-mq-ok" 1
run "$work/$label-notifier" S5EventNotifierStop.BlockedPublisherIsDetachedAfterBoundedWait "$work/$label-mq-block" 0
mkdir -p "$work/$label-selcwd"
# Death-test children fork; LSan in the child reports libsystem's fork-time allocation.
run "$work/$label-sel" S5SelScheduleThread.FiredTaskRunsOnceOnItsContextAndIsJoined "$work/$label-selcwd" 0
run "$work/$label-sel" S5SelScheduleThread.TasksOnSeveralDaysAreAllJoinedByRelease "$work/$label-selcwd" 0
exit 0
