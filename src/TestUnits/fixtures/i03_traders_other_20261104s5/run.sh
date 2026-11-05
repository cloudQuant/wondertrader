#!/bin/bash
# Reproduce the s5 traders-other focused runs (macOS arm64, local fakes only).
# usage: run.sh [fix|red|all]   (default all)
set -u
REPO=${WT_REPO:-$(cd "$(dirname "$0")/../../../.." && pwd)}
F=$REPO/src/TestUnits/fixtures/i03_traders_other_20261104s5
B=${WT_S5_BUILD:-/tmp/wt-i03-s5-traders-other-cov}
RUN=${WT_S5_RUN:-/tmp/wt-i03-s5-traders-other-run}
RED=${WT_S5_RED:-/tmp/wt-i03-s5-traders-other-red}
BEFORE=${WT_S5_BEFORE:-$REPO/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-traders-other/before}
RC=$REPO/src/TestUnits/fixtures/i03_leaks_main_20261104/run_cases.py
what=${1:-all}
mkdir -p "$RUN/cases"
export WT_I03_LEAKS_ROOT=$RUN

if [ ! -f "$B/build.ninja" ]; then
	cmake -S "$REPO/src" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include || exit 1
	cmake --build "$B" --target CoverageTests_t_TraderFemas CoverageTests_t_TraderMocker \
		CoverageTests_t_TraderXTP CoverageTests_t_TraderYD -j 3 || exit 1
fi

if [ "$what" = fix ] || [ "$what" = all ]; then
	ninja -C "$B" build.ninja >/dev/null
	objs=""
	for m in Femas Mocker YD; do
		objs="$objs $(ninja -C "$B" -t query build_x64/Debug/bin/TestUnits/CoverageTests_t_Trader$m | grep '\.o$' | sed 's/^ *//')"
	done
	ninja -C "$B" -j 3 $objs || exit 1
	python3 "$F/link_only.py" "$B" "$RUN/link-fix.json" Femas Mocker YD || exit 1
	python3 "$F/make_cases.py" "$B/build_x64/Debug/bin/TestUnits" fix "$RUN/cases" >/dev/null
	for g in t4002 t6009 r60003 r60001 t4003 r93006 lane; do
		(cd "$RUN" && python3 "$RC" "cases/cases-$g-fix.json" "results-$g-fix.json")
	done
fi

if [ "$what" = red ] || [ "$what" = all ]; then
	python3 "$F/red_overlay_build.py" "$REPO" "$B" "$BEFORE" "$RED" Mocker Femas YD || exit 1
	python3 "$F/make_cases.py" "$RED" red2 "$RUN/cases" >/dev/null
	for g in r60001 t4003 r93006; do
		(cd "$RUN" && python3 "$RC" "cases/cases-$g-red2.json" "results-$g-red2.json")
	done
fi
