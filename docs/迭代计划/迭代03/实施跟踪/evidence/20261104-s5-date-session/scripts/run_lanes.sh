#!/bin/sh
# Runs the coverage-lane groups directly related to the S5 date-session changes.
# Usage: sh run_lanes.sh <cov-build-dir> <run-dir>
set -u
B=${1:?build}; R=${2:?run}
BIN=$B/build_x64/Debug/bin/TestUnits
mkdir -p "$R"
: > "$R/results.txt"
run() { # name binary filter timeout
  name=$1; bin=$2; filter=$3; secs=$4
  mkdir -p "$R/$name"
  (cd "$R/$name" && LLVM_PROFILE_FILE="$R/$name/%p.profraw" perl -e 'alarm shift; exec @ARGV' "$secs" "$BIN/$bin" \
     --gtest_filter="$filter" --gtest_output="xml:$R/$name/result.xml" > "$R/$name/output.log" 2>&1)
  rc=$?
  echo "$name rc=$rc $(grep -E '^\[  PASSED  \]|^\[  FAILED  \] [0-9]+ test' "$R/$name/output.log" | tr '\n' ' ')" >> "$R/results.txt"
}
run l01_timeutils CoverageTests 'l01_timeutils.*' 120
run l03_includes CoverageTests 'l03_*:L03DaySessionTest.*' 300
run r12_session_bdm CoverageTests 'r12_session.*:R12BaseDataMgr*' 300
run r1_bdm_readers CoverageTests 'R1BaseDataMgr*:R1RdmHftTest.*:R1ReaderBarsTest.*' 300
run p09_readers CoverageTests 'P09*' 300
run q03_readers CoverageTests 'Q03*:R100Rdm*:R100Reader*:R112*:R122*:R132*:R142*:R162Reader*:R172*:R182*:R193Storage*:R203*:R212Storage*:R52Reader*:R70*:R74*' 600
run r23_rdm CoverageTests 'R23Rdm*:R52RdmTest.*' 300
#STAGE1B run q05_state_monitor CoverageTests_dtcore 'Q05StateMonitorTest.*:R145StateMonitorRunTest.*:R154StateMonitorStatesTest.*:R164StateMonitorHolidayTest.*:R174State*:R22StateMonitorStop.*:R44StateMonitorLifecycleTest.*:R5StateMonitorBdtTest.*:R68StateMonitorInputsTest.*:R84bStateMonitorPositiveOffsetTest.*' 600
#STAGE1B run t_ParserXeleSkt CoverageTests_t_ParserXeleSkt '*' 300
cat "$R/results.txt"
# second stage (optional): btcore replayer, AD readers, servo data manager
if [ "${S5_STAGE2:-0}" = 1 ]; then
run btcore_replayer CoverageTests_btcore 'P04*:R43Replayer*:P07*:R67CsvBars.*:ReplayTimeRangeTest.*:R103*:R2LoaderTest.*:R2PublicTest.*:R2TickTest.*:R67LoaderGap.*:R80*:R91Fixture.*:S5Q02FrozenTDate.*:S5R103LastTickInterval.*:HisDataReplayerRuntimeTest.*' 900
run ad_readers CoverageTests_adstorage 'P05RdmReaderADTest.*:R100ADReaderTest.*:R100MixedPeriodReaderTest.*:R112ReaderAD*:R132ReaderADCacheTest.*:R142ReaderADTest.*:R162AdReaderTest.*:R182AdReaderTest.*:R212AdReaderTest.*:R21ReaderADTest.*:R4ReaderAD*:S5ReaderADTest.*' 900
run ad_readers_r122 CoverageTests_adstorage 'R122ReaderADTest.*' 300
run servo_datamgr CoverageTests_dtservo 'Q05ServoWtDataManagerTest.*:R102*:R133WtDataManagerTest.*:R8WtDataManagerSinksTest.*:R78DtDataManager.*' 600
cat "$R/results.txt"
fi
