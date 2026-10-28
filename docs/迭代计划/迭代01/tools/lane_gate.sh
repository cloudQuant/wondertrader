#!/bin/sh
# lane_gate.sh —— 覆盖率 lane 的唯一「绿」定义，主控也用它复核 lane 交付。
# 用法: lane_gate.sh <TargetName> [--build] [--jobs N] [--only <grep正则>]
# 例:   lane_gate.sh CoverageTests_wtcore --build --jobs 4 --only '^r23'
#
# 为什么要一个脚本而不是让每条 lane 自己敲三条命令：本迭代实测踩过的四个坑
#   1) 判决在 rc 文件的首词（PASS/FAIL/CRASH/HANG/RUNDIR/NORUN/SUITECONF），不是退出码；
#   2) case_runner 会先 cd 到运行目录，bin 必须是绝对路径，否则 exec 失败被记成 FAIL(rc=126)；
#   3) --gtest_list_tests 的 suite 行自带结尾点号，不去掉会拼成 Suite..Case，与历史 id join 不上，
#      而且 DISABLED_ 前缀可能挂在 suite 上（DISABLED_Suite.Case），只滤 '.DISABLED_' 会漏；
#   4) 运行目录必须是新建的 —— per-case 进程共用一个 cwd，固定文件名的产物互相覆盖会造出假回归。
set -eu

ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
TOOLS="$ROOT/docs/迭代计划/迭代01/tools"
tgt="${1:-}"
[ -n "$tgt" ] || { echo "用法: lane_gate.sh <TargetName> [--build] [--jobs N] [--only 正则]" >&2; exit 2; }
shift

DO_BUILD=0
JOBS=4
ONLY=''
while [ $# -gt 0 ]; do
	case "$1" in
		--build) DO_BUILD=1; shift ;;
		--jobs) JOBS="$2"; shift 2 ;;
		--only) ONLY="$2"; shift 2 ;;
		*) echo "lane_gate: 未知参数 $1" >&2; exit 2 ;;
	esac
done

BIN="$ROOT/build-cov/build_x64/Debug/bin/TestUnits/$tgt"
if [ "$DO_BUILD" = "1" ]; then
	echo "== build $tgt"
	cmake --build "$ROOT/build-cov" --target "$tgt" 2>&1 | tail -3
fi
[ -x "$BIN" ] || { echo "lane_gate: 二进制不存在或不可执行：$BIN（先加 --build）" >&2; exit 3; }

T=$(mktemp -d /tmp/lane_gate_"$tgt".XXXXXX)
mkdir -p "$T/prof" "$T/rc" "$T/run"
echo "== work dir $T (run 目录新建，保证与上一轮不可比的因素被排除)"

"$BIN" --gtest_list_tests 2>/dev/null \
	| awk '/^[^ ]/{s=$0; sub(/\.$/,"",s); next} NF{gsub(/^[ \t]+/,""); print s "." $0}' \
	| grep -vE '(^|\.)DISABLED_' > "$T/cases.txt" || true
if [ -n "$ONLY" ]; then
	grep -e "$ONLY" "$T/cases.txt" > "$T/filtered.txt" || true
	mv "$T/filtered.txt" "$T/cases.txt"
fi
total=$(wc -l < "$T/cases.txt" | tr -d ' ')
if [ "$total" -eq 0 ]; then
	echo "lane_gate: 枚举到 0 条用例 —— 检查 --only 正则或目标是否真的建立了用例" >&2
	exit 4
fi
echo "== running $total cases, one process each, jobs=$JOBS, timeout 60s"
cat "$T/cases.txt" | xargs -P "$JOBS" -I'@' -n 1 /bin/sh "$TOOLS/case_runner.sh" \
	"$BIN" "$T/prof" "$T/run" "$T/rc" 60 '@' g >/dev/null 2>&1 || true

rows=$(cat "$T/rc"/*.rc 2>/dev/null | wc -l | tr -d ' ')
unaccounted=$((total - rows))
echo "== verdict census =="
cat "$T/rc"/*.rc 2>/dev/null | awk '{print $1}' | sort | uniq -c | sort -rn
echo "== rows=$rows enumerated=$total unaccounted=$unaccounted =="
[ "$unaccounted" -eq 0 ] || echo "WARNING: 有用例没留下判决文件，计数不可信（查 xargs/case_runner 是否被中断）" >&2

echo "== 非通过清单（含断言面）=="
python3 - "$T" "$ROOT" <<'PY' | tee "$T/redlist.txt"
import glob,os,re,sys
T,root=sys.argv[1],sys.argv[2]
n=0
for p in sorted(glob.glob(T+'/rc/*.rc')):
    c=(open(p,encoding='utf-8',errors='replace').read().split() or ['?'])
    if len(c)<2 or c[0]=='PASS': continue
    n+=1
    case=c[1]; slug=case.replace('.','_').replace(':','_').replace(' ','_')
    log=f'{T}/prof/g.{slug}.log'
    site=''
    if os.path.exists(log):
        lines=open(log,encoding='utf-8',errors='replace').read().splitlines()
        i=next((i for i,l in enumerate(lines) if re.search(r':\s*Failure\s*$',l) or 'must use the same' in l),None)
        if i is not None:
            site=f'{lines[i].split(":")[0].replace(root+"/","")}:{lines[i].split(":")[1]} ' if re.match(r'^/.*:\d+:',lines[i]) else ''
            site+=' '.join(x.strip() for x in lines[i+1:i+4])[:88]
        else:
            site=' / '.join(x.strip() for x in lines[6:10])[:88]
    print(f'  {c[0]:22s} {case[:52]:52s} {site}')
print(f'非通过合计 = {n}')
PY
n_red=$(grep '^非通过合计 = ' "$T/redlist.txt" | awk '{print $3}' || true)
if [ -z "${n_red:-}" ]; then
	echo "lane_gate: 读不到非通过计数 —— 判定不可信，按失败处理" >&2
	exit 6
fi
echo "== 复现用判决目录（隔离、可复查）: $T"
if [ "$n_red" -gt 0 ] || [ "$unaccounted" -ne 0 ]; then
	echo "lane_gate: $tgt 门禁未过（非通过 $n_red 条 / 未记账 $unaccounted 条）"
	exit 5
fi
echo "lane_gate: $tgt 门禁通过（$total 条全 PASS）"
