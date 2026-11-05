#!/bin/bash
SC=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
R=/Users/yunjinqi/Documents/new_projects/wondertrader/src
cd $SC
P=()
for f in CtaMocker EventNotifier ExecMocker HftMocker HisDataMgr HisDataReplayer MatchEngine SelMocker UftMocker WtHelper; do P+=("$R/WtBtCore/$f.cpp"); done
python3 slice1-tools/tu_harness.py $SC/slice1-result-build CoverageTests_btcore $SC/slice1-result-bins/r2-green $R/TestUnits/coverage/q02_btcore3/r91_replayer_bin_bars_test.cpp "${P[@]}" > $SC/slice1-result-logs/r2-green-build.log 2>&1
echo "exit=$?" >> $SC/slice1-result-logs/r2-green-build.log
PV=()
for f in "${P[@]}"; do if [[ "$f" == *HisDataReplayer.cpp ]]; then PV+=("$f=$SC/slice1-variants/HisDataReplayer.red-static-key.cpp"); else PV+=("$f"); fi; done
python3 slice1-tools/tu_harness.py $SC/slice1-result-build CoverageTests_btcore $SC/slice1-result-bins/r2-red $R/TestUnits/coverage/q02_btcore3/r91_replayer_bin_bars_test.cpp "${PV[@]}" > $SC/slice1-result-logs/r2-red-build.log 2>&1
echo "exit=$?" >> $SC/slice1-result-logs/r2-red-build.log
