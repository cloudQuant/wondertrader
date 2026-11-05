#!/bin/bash
# Copy run summaries, per-case XML and failing-case logs into the evidence dir.
E=${EVID:-/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-uftcore}
R=/tmp/wt-i03-s5-uftcore-run
for d in "$@"; do
  mkdir -p $E/${SUB:-runs}/$d
  cp $R/$d/summary.txt $R/$d/cases.txt $E/${SUB:-runs}/$d/ 2>/dev/null
  for c in $(ls $R/$d); do
    [ -d $R/$d/$c ] || continue
    [ "$c" = "_list" ] && continue
    mkdir -p $E/${SUB:-runs}/$d/$c
    [ -f $R/$d/$c/result.xml ] && cp $R/$d/$c/result.xml $E/${SUB:-runs}/$d/$c/
    if ! grep -q 'failures="0"' $R/$d/$c/result.xml 2>/dev/null; then cp $R/$d/$c/out.log $E/${SUB:-runs}/$d/$c/ 2>/dev/null; fi
  done
done
