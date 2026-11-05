#!/bin/bash
# Usage: run_full.sh <build-dir> <out-dir> [extra run_regression.py args...]
# Same case set/provisioning as round 1 (m_WtRunner modules provisioned per case; the one
# real-factory case excluded from provisioning), per-case process + cwd, 600 s timeout.
set -u
B="$1/build_x64/Debug/bin"; OUT="$2"; shift 2
TOOL=/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/regression-tools/run_regression.py
cd "$(dirname "$OUT")"
env -u WT_R66_FAKE_MQ_DIR -u R42_HFT_PLUGIN WT_COV_PLUGIN_BIN="$B" python3 -I "$TOOL" \
  --bin-dir "$B/TestUnits" --out "$OUT" --timeout 600 \
  --provision-modules "$B" --provision-only CoverageTests_m_WtRunner \
  --provision-exclude "CoverageTests_m_WtRunner::r28_wtrunner_strategies.real_cta_factory_null_self_is_rejected_and_dual_thrust_configures" \
  "$@"
