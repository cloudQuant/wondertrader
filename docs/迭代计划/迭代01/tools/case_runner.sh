#!/bin/sh
# 单条 gtest 用例的执行体：一条用例一个进程，失败/崩溃/挂死都只记在自己身上。
# 用法: case_runner.sh <bin> <prof_dir> <run_dir> <rc_dir> <case_timeout> <Suite.Case>
# 产物: <prof_dir>/case.<slug>.log 与 <rc_dir>/<slug>.rc（首词是判决：PASS/FAIL/CRASH/HANG）
bin="$1"; pd="$2"; rd="$3"; rcdir="$4"; tmo="$5"; c="$6"; tag="${7:-case}"

if [ -z "$bin" ] || [ -z "$pd" ] || [ -z "$rcdir" ] || [ -z "$c" ]; then
	echo "case_runner: bad argv (bin/pd/rcdir/case required)" >&2
	exit 2
fi

slug=$(printf "%s" "$c" | tr "/:. " "____")
log="$pd/$tag.$slug.log"

# 运行目录不存在 ≠ 用例失败：实测我把 cd 失败的 rc=99 统计成 FAIL，
# 167 条看起来「全红、108 条回归」，据此就会得出完全错误的裁定结论。
# 现在单独给 RUNDIR 判决，coverage.sh 把它和非通过一起计数但不混进 FAIL。
if [ ! -d "$rd" ]; then
	printf 'RUNDIR %s (缺运行目录 %s)\n' "$c" "$rd" >"$rcdir/$slug.rc"
	exit 0
fi

(
	cd "$rd" 2>/dev/null || exit 99
	export LLVM_PROFILE_FILE="$pd/$tag-%p.profraw"
	exec "$bin" --gtest_filter="$c" </dev/null
) >"$log" 2>&1 &
bp=$!

waited=0
while :; do
	# kill -0 看不见已退出的子进程（僵尸仍在），所以读 ps 的 state
	st=$(ps -o stat= -p "$bp" 2>/dev/null | tr -d " ")
	case "$st" in
		"" | Z*) break ;;
	esac
	if [ "$waited" -ge "$tmo" ]; then
		kill -KILL "$bp" 2>/dev/null
		printf 'HANG %s (%ss)\n' "$c" "$tmo" >"$rcdir/$slug.rc"
		exit 0
	fi
	sleep 1
	waited=$((waited + 1))
done

rc=0
wait "$bp" || rc=$?

# 判决不能只看退出码，两种「假绿/假红」都实测出现过：
# 1) gtest 的 filter 没匹配到任何用例时进程 exit=0，原来会被记成 PASS；
# 2) 同一个 suite 名下有两个不同 fixture 类（匿名命名空间头被多个 TU 包含）时，
#    gtest 在运行期直接拒绝整族用例，文本里没有断言 —— 那是用例结构问题，不是产品缺陷，
#    混在 FAIL 里会让人去改断言（实测连带 17 条）。
if grep -q 'must use the same test fixture' "$log" 2>/dev/null; then
	verdict=SUITECONF
elif [ "$rc" -eq 0 ] && grep -qE '^\[  PASSED  \] [1-9][0-9]* test' "$log" 2>/dev/null; then
	verdict=PASS
elif [ "$rc" -eq 0 ]; then
	verdict=NORUN
elif [ "$rc" -gt 128 ]; then
	# shellcheck disable=SC2003
	verdict="CRASH(rc=$rc,sig=$((rc - 128)))"
else
	verdict="FAIL(rc=$rc)"
fi

printf '%s %s\n' "$verdict" "$c" >"$rcdir/$slug.rc"
exit 0
