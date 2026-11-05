#!/bin/sh
# Runs the vendor parser V2 fixture from its build directory.
# usage: run.sh <build-dir> <out-dir> [green|red|all] [families...]
# green: scenarios_<f>_cur + v2_<f>; red: scenarios_<f>_base one process per case.
# Every executable runs with cwd=<build-dir> (ParserAdapter resolves ./parsers).
set -u
B="$1"; OUT="$2"; MODE="${3:-all}"; shift 3 2>/dev/null || shift $#
FAMS="${*:-ctp ctpmini ctpopt femas xtp}"
mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
CASES="ReleaseInsideSdkCallbackIsDeferred DisconnectInsideSdkCallbackIsBusy ReleaseWaitsForInFlightCallback LateSdkCallbackAfterReleaseIsDropped RepeatedInitIsRejectedAndKeepsOriginal ConnectInsideCallbackIsBusy RepeatedReleaseAndRemoverKeepsSdkOwner RemoverDeletesParserWithoutSdkObject DirectDeleteReleasesSdkFirst"
run() { # name timeout cmd...
	name="$1"; t="$2"; shift 2
	( cd "$B" && LLVM_PROFILE_FILE=/dev/null perl -e "alarm $t; exec @ARGV" -- "$@" --gtest_output=xml:"$OUT/$name.xml" > "$OUT/$name.log" 2>&1 )
	rc=$?
	echo "$name exit=$rc" >> "$SUMMARY"
}
for f in $FAMS; do
	if [ "$MODE" != red ]; then
		run "scenarios-$f-cur" 120 ./scenarios_${f}_cur
		run "v2-$f" 120 ./v2_${f}
	fi
	if [ "$MODE" != green ]; then
		( cd "$B/baseline" && LLVM_PROFILE_FILE=/dev/null perl -e "alarm 120; exec @ARGV" -- "$B/v2_${f}_base" --gtest_output=xml:"$OUT/red-v2-$f.xml" > "$OUT/red-v2-$f.log" 2>&1 )
		echo "red-v2-$f exit=$?" >> "$SUMMARY"
		mkdir -p "$OUT/red-$f"
		for c in $CASES; do
			run "red-$f/$c" 30 ./scenarios_${f}_base --gtest_filter="Scenario.$c"
		done
	fi
done
cat "$SUMMARY"
