#!/usr/bin/env python3
"""tsan_triage.py RUN_DIR: group TSan non-pass cases (race frames / tool-limitation classes)."""
import json, re, os, sys, collections
run = sys.argv[1]
g = collections.defaultdict(list)
for l in open(os.path.join(run, "results.jsonl")):
    r = json.loads(l)
    if r["status"] == "pass":
        continue
    p = os.path.join(r["cwd"], "log.txt")
    t = open(p, errors="replace").read() if os.path.exists(p) else ""
    if "WARNING: ThreadSanitizer" in t:
        kinds = set(re.findall(r"WARNING: ThreadSanitizer: ([a-z -]+?) \(", t))
        blocks = re.split(r"\n\s*\n", t[t.find("WARNING: ThreadSanitizer"):])
        fr = []
        for b in blocks[:3]:
            fs = [f for f in re.findall(r"#\d+ (\S+)", b) if not f.startswith(("std::", "void std", "__tsan", "pthread_", "testing::", "boost::", "operator", "<null>"))]
            if fs:
                fr.append(fs[0][:55])
        key = f"{'/'.join(sorted(kinds))}: " + " | ".join(fr[:2]) + f" [{r['status']}]"
    else:
        m = re.search(r"ERROR: ThreadSanitizer: (\w+)", t)
        passed = "[  PASSED  ]" in t
        key = f"{r['status']} {'(after PASSED)' if passed else ''} {m.group(1) if m else ''} {'DEADLYSIGNAL' if 'DEADLYSIGNAL' in t else ''}"
    g[key].append(r["binary"] + "::" + r["test"])
for k, v in sorted(g.items(), key=lambda x: -len(x[1])):
    print(f"[{len(v)}] {k}")
    for c in v[:4]:
        print("     ", c)
