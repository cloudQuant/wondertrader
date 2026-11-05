#!/usr/bin/env python3
"""Relink CoverageTests_t_Trader<Module> with the exact ninja link command, without
letting ninja rebuild sibling libraries (used when another package's in-progress
edit breaks an unrelated library; the libraries already in <build> are reused).

usage: link_only.py <cov_build_dir> <out_json> <Module>...
The target's own objects must be up to date first, e.g.
  ninja -C <build> $(ninja -C <build> -t query build_x64/Debug/bin/TestUnits/CoverageTests_t_Trader<M> | grep '\\.o$')
"""
import json, subprocess, sys

build, out = sys.argv[1], sys.argv[2]
log, rc = [], 0
for mod in sys.argv[3:]:
    tgt = "CoverageTests_t_Trader" + mod
    cmds = subprocess.run(["ninja", "-C", build, "-t", "commands", tgt], capture_output=True, text=True).stdout
    link = [l for l in cmds.splitlines() if ("-o build_x64/Debug/bin/TestUnits/" + tgt + " ") in l]
    if len(link) != 1:
        log.append({"module": mod, "error": "link command not found", "count": len(link)})
        rc = 1
        continue
    cmd = link[0]
    p = subprocess.run(cmd, shell=True, cwd=build, capture_output=True, text=True)
    log.append({"module": mod, "cmd": cmd, "exit": p.returncode, "stderr": p.stderr[-4000:]})
    print(mod, "link exit", p.returncode, flush=True)
    rc |= (p.returncode != 0)
json.dump(log, open(out, "w"), indent=1)
sys.exit(rc)
