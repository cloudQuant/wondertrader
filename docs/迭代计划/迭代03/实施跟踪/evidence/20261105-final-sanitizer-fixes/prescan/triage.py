#!/usr/bin/env python3
"""triage.py RUN_DIR : group non-pass cases by first sanitizer frame / message."""
import json, sys, re, os, collections
run = sys.argv[1]
rs = [json.loads(l) for l in open(os.path.join(run, "results.jsonl"))]
groups = collections.defaultdict(list)
for r in rs:
    if r["status"] == "pass":
        continue
    log = os.path.join(r["cwd"], "log.txt")
    key = r["status"]
    try:
        t = open(log, errors="replace").read()
    except OSError:
        t = ""
    m = re.search(r"/src/([^:\s]+:\d+):\d+: runtime error: ([^\n]{0,110})", t)
    if m:
        key = f"ubsan {m.group(1)} {m.group(2)}"
    else:
        m = re.search(r"ERROR: (AddressSanitizer|LeakSanitizer): ([\w-]+)", t)
        if m:
            fr = re.findall(r"#\d+ 0x[0-9a-f]+ in ([^\s(]+)", t)
            fr = [f for f in fr if not f.startswith(("__asan", "__sanitizer", "_platform", "wrap_", "__interceptor", "malloc", "free", "operator", "std::", "testing::"))]
            key = f"{m.group(1)} {m.group(2)} @ {' < '.join(fr[:3])}"
        elif "WARNING: ThreadSanitizer" in t:
            fr = re.findall(r"#\d+ ([^\s]+) ", t)
            key = f"tsan {' < '.join(fr[:2])}"
        else:
            m = re.search(r"(Failure|error|Expected)[^\n]{0,120}", t)
            if m and r["status"] in ("fail",) :
                key = f"{r['status']}: {m.group(0)[:120]}"
    groups[key].append(f"{r['binary']}::{r['test']}")
for k, v in sorted(groups.items(), key=lambda x: -len(x[1])):
    print(f"[{len(v)}] {k}")
    for c in v[:3]:
        print("     ", c)
