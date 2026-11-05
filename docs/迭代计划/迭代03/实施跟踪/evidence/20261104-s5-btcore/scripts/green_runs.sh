#!/bin/bash
S=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/a64516e7-fd3c-498f-b283-179c28398f44/scratchpad/s5btcore
R=/tmp/wt-i03-s5-btcore-run; G=/tmp/wt-i03-s5-btcore-bins/green
FAKE=/tmp/wt-i03-s5-btcore-cov/TestUnits/coverage/r160_fake_mq
run() { name=$1; bin=$2; filter=$3; echo "== $name"; $S/runcases.sh $G/$bin $R/$name 180 "$filter"; }
run g-q02frozen CoverageTests_btcore 'S5Q02FrozenTDate.*'
run g-r103 CoverageTests_btcore 'S5R103LastTickInterval.*:R103LastTickBoundaryTest.*'
run g-q02int CoverageTests_btcore 'Q02CtaIntegrationTest.*'
run g-r80 CoverageTests_btcore 'R80SliceUnsub.*'
run g-r2pub CoverageTests_btcore 'R2PublicTest.*'
run g-new090 CoverageTests_btcore 'R200Fixture.*'
run g-new093 CoverageTests_btcore 'Z15MatchEngineUnsinked.*:MatchEngineTest.*:R20ExecMocker.CancelByIdDoesNotRestoreUndoneTwice'
run g-r20notifier CoverageTests_btcore 'R20EventNotifier.*'
run g-r2adj CoverageTests_btcore 'R91Fixture.R2AdjFactors_*:R91Fixture.R91BinBars_Adjusted*'
run g-p07 CoverageTests_btcore 'P07TickLoaderTest.*'
run g-q02cta CoverageTests_btcore 'Q02Cta*:C3CtaFrozen.*'
EXTRA_ENV="WT_R66_FAKE_MQ_DIR=$FAKE" run g-r20runner CoverageTests_btporter 'S5R20RunnerNotifierRelease.*:R66BtPorterNotifier.*'
run g-btrunner-life CoverageTests_btporter 'WtBtRunnerShutdown.*:N04ExecMocker.*:R205BtRunnerConfig.*:Z15BtRunnerConfigLock.*:R113BtPorterRelease*'
run g-new019 CoverageTests_m_WtBtRunner 'S5BtRunnerPrepareDiagnostic.*:r14_wtbtrunner_cli.*'
