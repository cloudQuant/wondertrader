#!/usr/bin/env python3
"""Red-control overlay build for i03_traders_other_20261104s5.

usage: red_overlay_build.py <repo> <cov_build_dir> <before_dir> <out_dir> <Module>...

1. rsync (no links) <repo>/src to <out_dir>/src, then overwrite the owned product
   files with the pre-change copies kept in <before_dir>/src (TraderFemas,
   TraderMocker, TraderYD). Test files stay current, so the new tests are compiled
   against the old product (WT_T4003_BASELINE disables calls to APIs that do not
   exist before the change).
2. Take the exact ninja compile/link commands of CoverageTests_t_Trader<Module> from
   <cov_build_dir>, rewrite the source root to the overlay and the object/binary
   paths into <out_dir>, compile and link. Dependency libraries are the ones in
   <cov_build_dir> (same flags, same -Werror policy).
Writes <out_dir>/commands-<Module>.json with argv and exit codes.
"""
import json, os, shlex, subprocess, sys

repo, build, before, out = [os.path.abspath(p) for p in sys.argv[1:5]]
mods = sys.argv[5:]
src = os.path.join(repo, "src")
osrc = os.path.join(out, "src")
os.makedirs(out, exist_ok=True)
subprocess.run(["rsync", "-a", "--no-links", "--delete", src + "/", osrc + "/"], check=True)
for rel in ["TraderFemas/TraderFemas.cpp", "TraderFemas/TraderFemas.h", "TraderMocker/TraderMocker.cpp",
            "TraderMocker/TraderMocker.h", "TraderYD/TraderYD.cpp", "TraderYD/TraderYD.h",
            "TraderXTP/TraderXTP.cpp", "TraderXTP/TraderXTP.h"]:
    b = os.path.join(before, "src", rel)
    if os.path.exists(b):
        subprocess.run(["cp", "-p", b, os.path.join(osrc, rel)], check=True)
links = subprocess.run(["find", osrc, "-type", "l"], capture_output=True, text=True).stdout.strip()
if links:
    sys.exit("overlay contains symlinks: " + links)

rc_all = 0
for mod in mods:
    tgt = "CoverageTests_t_Trader" + mod
    cmds = subprocess.run(["ninja", "-C", build, "-t", "commands", tgt], capture_output=True, text=True).stdout
    lines = [l for l in cmds.splitlines() if (tgt + ".dir") in l or ("-o build_x64/Debug/bin/TestUnits/" + tgt + " ") in l]
    objdir = os.path.join(out, "obj-" + mod)
    os.makedirs(objdir, exist_ok=True)
    log = []
    for line in lines:
        line = line.replace(src + "/", osrc + "/")
        # The source root also appears as a bare include directory (-isystem <src>).
        line = line.replace(" " + src + " ", " " + osrc + " ")
        if src in line.replace(osrc, ""):
            sys.exit("unreplaced source root in: " + line)
        line = line.replace("TestUnits/coverage/CMakeFiles/" + tgt + ".dir/", objdir + "/")
        line = line.replace("build_x64/Debug/bin/TestUnits/" + tgt + " ", os.path.join(out, tgt) + " ")
        if " -c " in line and mod == "Femas":
            line = line.replace(" -c ", " -DWT_T4003_BASELINE -c ", 1)
        if " -c " in line:
            line = line.replace(" -c ", " -DWT_S5_RED_BASELINE -c ", 1)
        if line.startswith(": && "):
            line = line[5:]
        if line.endswith(" && :"):
            line = line[:-5]
        for d in set(os.path.dirname(t) for t in shlex.split(line) if t.startswith(objdir)):
            os.makedirs(d, exist_ok=True)
        p = subprocess.run(line, shell=True, cwd=build, capture_output=True, text=True)
        log.append({"cmd": line, "exit": p.returncode, "stderr": p.stderr[-4000:]})
        if p.returncode != 0:
            rc_all = 1
            print("FAILED", mod, p.stderr[-2000:], flush=True)
            break
    json.dump(log, open(os.path.join(out, "commands-%s.json" % mod), "w"), indent=1)
    print(mod, "steps", len(log), "exit", log[-1]["exit"] if log else None, flush=True)
sys.exit(rc_all)
