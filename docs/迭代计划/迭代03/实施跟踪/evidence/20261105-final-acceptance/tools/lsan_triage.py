#!/usr/bin/env python3
"""Group LSan reports by first in-repo allocation frame (direct leaks) across a run dir."""
import json, os, re, sys, collections
run = sys.argv[1]
SKIP = ("operator new", "malloc", "calloc", "realloc", "strdup", "__", "std::", "testing::", "posix_memalign",
        "_Znwm", "_Znam", "wrap_", "boost::", "fmt::", "<null>", "dyld", "start", "main")
sites = collections.defaultdict(lambda: {"cases": set(), "bytes": 0, "direct": 0})
per_case = {}
for line in open(os.path.join(run, "results.jsonl")):
    r = json.loads(line)
    if "lsan" not in r["status"]:
        continue
    log = os.path.join(r["cwd"], "log.txt")
    if not os.path.exists(log):
        continue
    t = open(log, errors="replace").read()
    blocks = re.split(r"\n(?=(?:Direct|Indirect) leak of )", t)
    cs = set()
    for b in blocks:
        m = re.match(r"(Direct|Indirect) leak of (\d+) byte", b)
        if not m:
            continue
        frames = re.findall(r"#\d+ 0x[0-9a-f]+ in (\S+)", b)
        site = next((f for f in frames if not f.startswith(SKIP)), frames[0] if frames else "?")
        key = (m.group(1), site[:90])
        sites[key]["cases"].add(f"{r['binary']}::{r['test']}")
        sites[key]["bytes"] += int(m.group(2))
        cs.add(key)
    per_case[f"{r['binary']}::{r['test']}"] = sorted(f"{k[0]}:{k[1]}" for k in cs)
direct = sorted(((k, v) for k, v in sites.items() if k[0] == "Direct"), key=lambda kv: -len(kv[1]["cases"]))
print("distinct direct-leak sites:", len(direct), " cases with lsan:", len(per_case))
for (kind, site), v in direct[:70]:
    ex = sorted(v["cases"])[0]
    print(f"{len(v['cases']):5d} cases {v['bytes']:9d} B  {site}   e.g. {ex}")
json.dump({"per_case": per_case, "direct_sites": {k[1]: sorted(v["cases"]) for k, v in direct}},
          open(os.path.join(run, "lsan-triage.json"), "w"), ensure_ascii=False, indent=1)
