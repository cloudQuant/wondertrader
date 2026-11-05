#!/bin/bash
# rt.sh <tree:green|red> <binary> <name> <filter> [extra gtest args...]
S=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
E=/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-gaps-slice3-result/logs
tree=$1; bin=$2; name=$3; filter=$4; shift 4
if [ "$tree" = red ]; then B=$S/slice3-red-build/build_x64/Debug/bin; else B=$S/slice3-result-build/build_x64/Debug/bin; fi
D=$S/slice3-result-run/$tree-$name; rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 99
if [ -n "$PROVISION" ]; then for f in $B/lib*.dylib; do [ -f "$f" ] && [ ! -L "$f" ] && cp "$f" .; done; fi
LLVM_PROFILE_FILE="$D/run.profraw" perl -e 'alarm 900; exec @ARGV' "$B/TestUnits/$bin" --gtest_filter="$filter" --gtest_output=xml:"$D/result.xml" "$@" > "$D/run.log" 2>&1
rc=$?
cp "$D/run.log" "$E/$tree-$name.log"; [ -f "$D/result.xml" ] && cp "$D/result.xml" "$E/$tree-$name.xml"
echo "$tree $name rc=$rc $(grep -E '^\[  PASSED  \]|^\[  FAILED  \] [0-9]+ test' "$D/run.log" | tr '\n' ' ')"
echo "$(date +%FT%T) $tree $bin $name filter=$filter args=$* rc=$rc" >> "$E/commands.txt"
