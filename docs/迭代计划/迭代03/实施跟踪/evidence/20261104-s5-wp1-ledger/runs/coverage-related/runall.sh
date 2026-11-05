#!/bin/bash
# usage: runall.sh <binary> <outroot> <list>
b=$1; o=$2; l=$3
mkdir -p "$o"
grep -v '^$' "$l" | xargs -P 4 -I{} /tmp/wt-i03-s5-wp1-ledger-run/rel/run1.sh "$b" "$o" {} > "$o/per-unsorted.txt"
sort -k2 "$o/per-unsorted.txt" > "$o/per.txt"
awk '{c[$1==0?"pass":"fail"]++} END {print "pass=" c["pass"] " fail=" c["fail"]}' "$o/per.txt"
