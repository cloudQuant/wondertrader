#!/usr/bin/env python3
"""Run gtest cases one per process in scratch cwd; record argv/exit/xml counts.
usage: run_cases.py <cases.json> <results.json>
cases.json: [{"id": "...", "name": "...", "bin": "...", "filter": "...", "env": {...}, "timeout": 300}]
"""
import json, os, subprocess, sys, xml.etree.ElementTree as ET

R = "/tmp/wt-i03-reverify-20261103"
cases = json.load(open(sys.argv[1]))
out = []
for c in cases:
    d = os.path.join(R, "run", c["id"], c["name"])
    os.makedirs(d, exist_ok=True)
    xml = os.path.join(d, "result.xml")
    if os.path.exists(xml):
        os.remove(xml)
    argv = ["perl", "-e", "alarm %d; exec @ARGV" % c.get("timeout", 300), "--",
            c["bin"], "--gtest_filter=" + c["filter"], "--gtest_output=xml:" + xml]
    env = dict(os.environ)
    env["TMPDIR"] = d + "/"
    env["LLVM_PROFILE_FILE"] = os.path.join(d, "profile-%p.profraw")
    for k, v in c.get("env", {}).items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = v
    with open(os.path.join(d, "output.log"), "wb") as f:
        p = subprocess.run(argv, cwd=d, env=env, stdout=f, stderr=subprocess.STDOUT)
    rec = {"id": c["id"], "name": c["name"], "argv": argv, "cwd": d, "exit": p.returncode,
           "env_overrides": c.get("env", {})}
    if os.path.exists(xml):
        r = ET.parse(xml).getroot()
        rec["xml"] = {k: r.get(k) for k in ("tests", "failures", "disabled", "errors", "skipped")}
        rec["skipped_cases"] = sum(1 for tc in r.iter("testcase") if tc.find("skipped") is not None)
    else:
        rec["xml"] = None
    out.append(rec)
    print("%s %s exit=%s xml=%s" % (c["id"], c["name"], p.returncode, rec["xml"]), flush=True)
json.dump(out, open(sys.argv[2], "w"), ensure_ascii=False, indent=1)
