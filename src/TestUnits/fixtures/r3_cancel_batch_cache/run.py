#!/usr/bin/env python3
"""Compile three current production methods against isolated substitutes."""
import pathlib
import os
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
OUT = pathlib.Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=False)
text = (ROOT / "src/WtCore/TraderAdapter.cpp").read_text()
signatures = (
    "bool TraderAdapter::doCancel(WTSOrderInfo* ordInfo)",
    "bool TraderAdapter::cancel(uint32_t localid)",
    "OrderIDs TraderAdapter::cancel(const char* stdCode, bool isBuy, double qty /* = 0 */)",
)
parts = []
for signature in signatures:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                parts.append(text[start:pos + 1])
                break
    else:
        raise RuntimeError("unclosed function: " + signature)
(OUT / "extracted.inc").write_text("\n\n".join(parts) + "\n")
fixture = pathlib.Path(__file__).with_name("check.cpp")
sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.getenv("R3_SANITIZE") else []
subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", *sanitizers, "-I", str(OUT),
                str(fixture), "-o", str(OUT / "check")], check=True)
for mode in ("single", "failure", "batch"):
    result = subprocess.run([str(OUT / "check"), mode], check=False, timeout=20)
    print(f"{mode}: exit {result.returncode}", flush=True)
    expected = 1 if mode == "batch" else 0
    if result.returncode != expected:
        sys.exit(1)
