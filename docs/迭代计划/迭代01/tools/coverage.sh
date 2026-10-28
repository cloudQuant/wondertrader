#!/usr/bin/env bash
# 迭代 01 覆盖率构建/运行/报告入口
#
# 运行模型：用例级并行。一条 gtest 用例起一个进程，TEST_JOBS 条并行；
# 用例失败、崩溃（段错误）、挂死都只影响它自己那一条，流程绝不中断。
# 这样做的代价是每个用例都要付一次进程启动 + 库加载，所以用 -P 并行摊平。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
TOOLS="$ROOT/docs/迭代计划/迭代01/tools"
BUILD="${1:-$ROOT/build-cov}"
JOBS="${JOBS:-8}"
TEST_JOBS="${TEST_JOBS:-4}"
CASE_TIMEOUT="${CASE_TIMEOUT:-60}"
PROF_DIR="$BUILD/coverage"
RUN_DIR="$PROF_DIR/run"
RC_DIR="$PROF_DIR/rc"

# 计数器落盘位置必须在**驱动层**就定下来：case_runner 只在它自己的子 shell 里
# export LLVM_PROFILE_FILE，于是 `--gtest_list_tests` 这类直接由本脚本启动的进程
# 没有该变量，就把 default.profraw 写到仓库根目录（实测一轮里被反复覆盖 34 次、7MB）。
# 影响已经量过，不是「少了很多」：对 CoverageTests_t_TraderMocker 单独对照
# 「只有用例计数」vs「用例+枚举计数」，llvm-cov 汇总三项完全一致
# （regions 8939/missed 8621、functions 3146/3059、lines 21831/21378），
# 因为枚举进程跑的行是每个用例进程都跑过的子集。所以这条修的是**卫生**（工作树里
# 不再长出二进制垃圾），不是补覆盖率数字——两者分开说，别拿它当「之前数字偏低」的 explanation。
# 不再补 mkdir：下面 rm -rf "$PROF_DIR" && mkdir -p 已经负责目录新鲜度，
# 也正因为它每轮都重建，历次运行的计数器不会被串到一起虚增数字。
export LLVM_PROFILE_FILE="$PROF_DIR/enumerate-%p.profraw"

if [ "${SKIP_BUILD:-0}" = "1" ]; then
	# 只做「跑用例 + 出覆盖率」：沿用上一次成功链接的二进制。
	# 用于在有目标仍链不通时先量出已可用部分的数字；不改变任何构建判定。
	echo "== SKIP_BUILD=1: 跳过 configure/build，沿用现有测试二进制 =="
else
	cmake -S "$ROOT/src" -B "$BUILD" -G Ninja \
	  -DCMAKE_BUILD_TYPE=Debug \
	  -DWT_ENABLE_COVERAGE=ON \
	  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# BUG-L00-001 (待审核): 5 个 CMakeLists 的 POST_BUILD 拷贝没有 add_dependencies 排序保证，
# 冷目录首建时宿主目标先于被拷贝的 parser/trader 链接，拷贝报 No such file or directory，
# 而 ninja 遇错即停，测试可执行文件因此根本没被构建（重试与 -k 0 都不能绕过：失败边会让
# 下游把宿主 dylib 当作未生成而跳过）。这里先构建那 18 个被拷贝的插件作为前置，
# 名单由 deploy_prereq_targets.py 从 CMakeLists 本身解析，不手填、不改生产构建脚本。
PRE_TARGETS="$(python3 "$TOOLS/deploy_prereq_targets.py" "$ROOT/src" | tr '\n' ' ')"
if [ -n "${PRE_TARGETS// /}" ]; then
	echo "== pre-building deploy prerequisites: $PRE_TARGETS"
	cmake --build "$BUILD" -j "$JOBS" --target $PRE_TARGETS || true
fi
cmake --build "$BUILD" -j "$JOBS"
fi

rm -rf "$PROF_DIR"
mkdir -p "$PROF_DIR" "$RUN_DIR" "$RC_DIR"

TEST_BINS=()
while IFS= read -r line; do
	TEST_BINS+=("$line")
done < <(find "$BUILD" -type f \( -name TestUnits -o -name CoverageTests -o -name 'CoverageTests_*' \) -perm -111 | sort)
if [ "${#TEST_BINS[@]}" -eq 0 ]; then
	echo "no test executables found under $BUILD" >&2
	exit 1
fi

# 执行体在 tools/case_runner.sh，单独成文件是为了能拿假二进制做正反对照测试
fail_cases=0
for bin in "${TEST_BINS[@]}"; do
	name="$(basename "$bin")"
	caselist="$PROF_DIR/$name.cases"
	RC_DIR="$PROF_DIR/rc/$name"
	mkdir -p "$RC_DIR"

	# --gtest_list_tests 也会列出 DISABLED_ 用例（实测 362 枚举 vs 354 运行），显式滤掉。
	# 注意 DISABLED_ 前缀可能挂在 case 上（Suite.DISABLED_Case），也可能挂在 suite 上
	# （DISABLED_Suite.Case）：只滤 '\.DISABLED_' 会漏掉后者，实测 11 条挂 suite 的用例被枚举、
	# 被 gtest 自己跳过（"Running 0 tests"）、退出码 0，于是旧 corpus 把它们记成了 PASS。
	"$bin" --gtest_list_tests 2>/dev/null \
		| awk '/^[^ ]/{s=$0; sub(/\.$/,"",s); next} NF{gsub(/^[ \t]+/,""); print s "." $0}' \
		| grep -vE '(^|\.)DISABLED_' > "$caselist" || true
	total=$(wc -l < "$caselist" | tr -d ' ')
	if [ "$total" -eq 0 ]; then
		echo "== $name: enumerated 0 cases — 检查 --gtest_list_tests 是否可用 ==" >&2
		fail_cases=$((fail_cases + 1))
		continue
	fi
	echo "== running $name: $total cases, one process each, TEST_JOBS=$TEST_JOBS, CASE_TIMEOUT=${CASE_TIMEOUT}s =="

	xargs -P "$TEST_JOBS" -I'@' -n 1 /bin/sh "$TOOLS/case_runner.sh" \
		"$bin" "$PROF_DIR" "$RUN_DIR" "$RC_DIR" "$CASE_TIMEOUT" '@' "$name" < "$caselist"

	# 只统计本二进制的 rc 目录——早期版本 cat 的是共享目录，会把另一个二进制的判决混进来
	results="$PROF_DIR/$name.results"
	cat "$RC_DIR"/*.rc > "$results" 2>/dev/null || true
	n_rows=$(wc -l < "$results" | tr -d ' ')
	n_pass=$(grep -c '^PASS ' "$results" || true)
	n_fail=$(grep -c '^FAIL' "$results" || true)
	n_crash=$(grep -c '^CRASH' "$results" || true)
	n_hang=$(grep -c '^HANG ' "$results" || true)
	n_rundir=$(grep -c '^RUNDIR ' "$results" || true)
	n_suiteconf=$(grep -c '^SUITECONF ' "$results" || true)
	n_norun=$(grep -c '^NORUN ' "$results" || true)
	unaccounted=$((total - n_rows))
	# 非通过按恒等式 rows - PASS 推导，而不是把各类判决相加：
	# 新增判决类别时（本轮就加了 SUITECONF/NORUN）漏加会把它静默算成通过。
	n_nonpass=$((n_rows - n_pass))
	echo "== $name verdict: PASS=$n_pass FAIL=$n_fail CRASH=$n_crash HANG=$n_hang RUNDIR=$n_rundir SUITECONF=$n_suiteconf NORUN=$n_norun nonpass=$n_nonpass rows=$n_rows enumerated=$total unaccounted=$unaccounted =="
	if [ "$unaccounted" -ne 0 ]; then
		echo "WARNING: $unaccounted case(s) 没有判决文件 — 计数不可信，需查 xargs/case_runner 是否被中断" >&2
	fi
	if [ "$n_nonpass" -gt 0 ]; then
		echo "-- $name 非通过用例 --"
		grep -v '^PASS ' "$results" | sort | sed 's/^/   /'
		fail_cases=$((fail_cases + n_nonpass))
	fi
done

# 空 profraw = 该进程崩溃/被 KILL，没落计数器。显式丢弃并报数，不给假数据。
kept=0
dropped=0
MERGE_INPUTS=""
while IFS= read -r f; do
	[ -e "$f" ] || continue
	if [ -s "$f" ]; then
		kept=$((kept + 1))
		MERGE_INPUTS="$MERGE_INPUTS $f"
	else
		dropped=$((dropped + 1))
	fi
done < <(find "$PROF_DIR" -name '*.profraw' | sort)
echo "== profraw: kept=$kept dropped_empty=$dropped =="
if [ "$kept" -eq 0 ]; then
	echo "no usable profraw produced; coverage cannot be measured" >&2
	exit 1
fi
if [ "$dropped" -ne 0 ]; then
	echo "WARNING: $dropped process(es) produced no counters — reported coverage excludes them" >&2
fi

# shellcheck disable=SC2086
xcrun llvm-profdata merge -sparse $MERGE_INPUTS -o "$PROF_DIR/coverage.profdata"

report_rc=0
# 报表落盘，否则「这轮到底多少」只存在于当时那次终端输出里，下一轮无法对照
REPORT_DIR="$ROOT/docs/迭代计划/迭代01/reports"
mkdir -p "$REPORT_DIR"
REPORT_FILE="$REPORT_DIR/coverage-$(date -u +%Y%m%dT%H%M%SZ).txt"
python3 "$TOOLS/coverage_report.py" "$BUILD" "$PROF_DIR/coverage.profdata" 2>&1 | tee "$REPORT_FILE" || report_rc=$?
echo "report saved: $REPORT_FILE"
if [ "$report_rc" -ne 0 ]; then
	exit "$report_rc"
fi

if [ "$fail_cases" -ne 0 ]; then
	echo "COVERAGE_REPORT_DONE with $fail_cases non-passing case(s)" >&2
	exit 1
fi
echo "COVERAGE_REPORT_DONE all cases passed"
