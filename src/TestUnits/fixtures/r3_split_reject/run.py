#!/usr/bin/env python3
"""Probe current buy/sell control flow with a rejecting split-order seam."""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[4]
out = pathlib.Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
source = (root / "src/WtCore/TraderAdapter.cpp").read_text()
parts = []
for method in ("buy", "sell"):
    start = source.index("OrderIDs TraderAdapter::" + method + "(")
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if not depth:
                parts.append(source[start:end + 1])
                break
    else:
        raise RuntimeError("unclosed " + method)
(out / "extracted.inc").write_text("\n\n".join(parts) + "\n")
fixture = pathlib.Path(__file__).with_name("check.cpp")
subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-I", str(out),
                str(fixture), "-o", str(out / "check")], check=True)
for mode, expected in (("buy_control", 0), ("sell_control", 0),
                       ("buy_reject", 1), ("sell_reject", 1)):
    result = subprocess.run([str(out / "check"), mode], timeout=20, check=False)
    print(f"{mode}: exit {result.returncode}", flush=True)
    if result.returncode != expected:
        sys.exit(1)
