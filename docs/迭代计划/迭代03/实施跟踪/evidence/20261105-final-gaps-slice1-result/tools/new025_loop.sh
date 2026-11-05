#!/bin/bash
SC=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
cd $SC
RX=$(cat slice1-tools/new025-fixed-regex.txt)
for r in 2 3 4 5 6 7 8 9 10; do
  OUT=$SC/slice1-result-run/new025-round$r
  python3 slice1-tools/signcmp_scan.py slice1-tools/cov_compile_cmds.txt $OUT 3 "$RX" > $SC/slice1-result-logs/new025-round$r.log 2>&1
  n=$(python3 -c "import json;print(len(json.load(open('$OUT/summary.json'))['sign_compare_sites']))")
  echo "round $r sites=$n"
  [ "$n" = "0" ] && break
  python3 slice1-patches/p_new025_iter.py $OUT/logs $SC/slice1-before2/new025 slice1-tools/new025-iter-record.jsonl slice1-tools/new025-locked-files.txt
  # next round: only files that still had warnings
  RX=$(python3 -c "import json,re;s=json.load(open('$OUT/summary.json'));print('|'.join(re.escape(x) for x in s['tus_with_sign_compare']))")
done
echo loop-end
