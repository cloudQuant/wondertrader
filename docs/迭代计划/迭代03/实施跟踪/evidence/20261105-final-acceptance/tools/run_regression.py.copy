#!/usr/bin/env python3
"""Iteration 03 isolated regression runner.

Runs every gtest case of every test binary in its own process and its own
scratch working directory, with a hard per-case timeout. Nothing is written to
the repository tree; all output goes to --out.

Usage:
  run_regression.py --bin-dir <build>/build_x64/Debug/bin/TestUnits --out <dir>
                    [--workers 6] [--timeout 120] [--only NAME[,NAME...]]
                    [--filter-file FILE]   (lines "Binary<TAB>Suite.Case")

Outputs in --out:
  manifest.json      binaries, sha256, list counts, start/end time
  results.jsonl      one JSON object per case
  summary.json       counts per binary and overall, plus non-pass lists
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path


def now() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def list_tests(binary: Path, cwd: Path, timeout: float) -> tuple[list[str], list[str], str]:
    cwd.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LLVM_PROFILE_FILE=str(cwd / "list-%p.profraw"))
    try:
        p = subprocess.run([str(binary), "--gtest_list_tests"], cwd=cwd, env=env,
                           stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return [], [], "list-timeout"
    if p.returncode != 0:
        return [], [], f"list-exit-{p.returncode}"
    enabled, disabled = [], []
    suite = None
    for raw in p.stdout.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if not raw.startswith(" "):
            suite = line.strip()
            continue
        case = line.strip()
        name = f"{suite}{case}"
        if suite.startswith("DISABLED_") or case.startswith("DISABLED_") or "/DISABLED_" in suite:
            disabled.append(name)
        else:
            enabled.append(name)
    return enabled, disabled, "ok"


def parse_xml(xml_path: Path) -> dict:
    out = {"xml": False}
    if not xml_path.is_file():
        return out
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError:
        out["xml"] = "corrupt"
        return out
    out["xml"] = True
    cases = root.findall(".//testcase")
    out["cases"] = len(cases)
    out["failures"] = sum(1 for c in cases if c.find("failure") is not None)
    out["skipped"] = sum(1 for c in cases if c.find("skipped") is not None
                         or c.get("result") == "skipped" or c.get("status") == "notrun")
    msgs = []
    for c in cases:
        for f in c.findall("failure"):
            msgs.append((f.get("message") or "")[:400])
    out["messages"] = msgs[:3]
    return out


MODULE_DIR = None  # set by --provision-modules: copy these dylibs into each case cwd
PROVISION_ONLY: set[str] = set()  # if non-empty, provision only cases of these binaries
PROVISION_EXCLUDE: set[str] = set()  # "Binary::Suite.Case" entries that install modules themselves
NO_PROFILE_ENV = False  # --no-profile-env: do not set LLVM_PROFILE_FILE (LSan runs; default.profraw lands in the case cwd)
RUN_DISABLED = False    # --only-disabled: run only DISABLED_ cases (informational, never counted as pass)


def wants_modules(binary: Path, test: str = "") -> bool:
    if MODULE_DIR is None or f"{binary.name}::{test}" in PROVISION_EXCLUDE:
        return False
    return not PROVISION_ONLY or binary.name in PROVISION_ONLY


def run_case(binary: Path, test: str, out: Path, timeout: float) -> dict:
    safe = test.replace("/", "_")
    cwd = out / "cwd" / binary.name / safe
    cwd.mkdir(parents=True, exist_ok=True)
    if wants_modules(binary, test):
        import shutil
        for lib in MODULE_DIR.glob("lib*.dylib"):
            if lib.is_file() and not lib.is_symlink():
                shutil.copy2(lib, cwd / lib.name)
    xml = cwd / "result.xml"
    log = cwd / "log.txt"
    env = dict(os.environ)
    if NO_PROFILE_ENV:
        env.pop("LLVM_PROFILE_FILE", None)
    else:
        env["LLVM_PROFILE_FILE"] = str(cwd / "%p.profraw")
    start = time.monotonic()
    status = None
    with log.open("wb") as lf:
        argv = [str(binary), f"--gtest_filter={test}", f"--gtest_output=xml:{xml}"]
        if RUN_DISABLED:
            argv.append("--gtest_also_run_disabled_tests")
        proc = subprocess.Popen(argv,
                                cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=lf,
                                stderr=subprocess.STDOUT, start_new_session=True)
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            status = "timeout"
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            rc = proc.wait()
    # Kill any leftover children of this case (process group).
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    elapsed = round(time.monotonic() - start, 3)
    info = parse_xml(xml)
    if status is None:
        if rc < 0:
            status = f"signal-{-rc}"
        elif info.get("xml") is not True:
            status = f"noxml-exit-{rc}"
        elif info.get("cases", 0) == 0:
            status = f"nocase-exit-{rc}"
        elif info.get("failures", 0) > 0:
            status = "fail"
        elif rc != 0:
            status = f"exit-{rc}"
        elif info.get("skipped", 0) > 0:
            status = "skipped"
        else:
            status = "pass"
    # A sanitizer report in the log turns any status into a sanitizer finding.
    try:
        text = log.read_text(errors="replace")
    except OSError:
        text = ""
    for marker, tag in (("ERROR: AddressSanitizer", "asan"), ("ERROR: LeakSanitizer", "lsan"),
                        ("WARNING: ThreadSanitizer", "tsan"), ("runtime error:", "ubsan")):
        if marker in text:
            status = f"sanitizer-{tag}" + ("" if status == "pass" else f"({status})")
            break
    # Keep logs only for non-passing cases; drop profile data to save space.
    if wants_modules(binary, test):
        for lib in MODULE_DIR.glob("lib*.dylib"):
            try:
                (cwd / lib.name).unlink()
            except OSError:
                pass
    for prof in cwd.glob("*.profraw"):
        try:
            prof.unlink()
        except OSError:
            pass
    if status == "pass":
        try:
            log.unlink()
        except OSError:
            pass
    return {"binary": binary.name, "test": test, "status": status, "rc": rc,
            "seconds": elapsed, "messages": info.get("messages", []), "cwd": str(cwd)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--only", default="")
    ap.add_argument("--filter-file", default="")
    ap.add_argument("--provision-modules", default="",
                    help="directory whose lib*.dylib are copied (not linked) into each case cwd")
    ap.add_argument("--provision-only", default="",
                    help="comma-separated binary names that get --provision-modules (default: all)")
    ap.add_argument("--provision-exclude", default="",
                    help="comma-separated Binary::Suite.Case entries never provisioned")
    ap.add_argument("--env", action="append", default=[],
                    help="KEY=VALUE added to every case environment (e.g. ASAN_OPTIONS=...)")
    ap.add_argument("--no-profile-env", action="store_true",
                    help="do not set LLVM_PROFILE_FILE (for LSan runs)")
    ap.add_argument("--only-disabled", action="store_true",
                    help="run only DISABLED_ cases with --gtest_also_run_disabled_tests (informational)")
    args = ap.parse_args()
    global MODULE_DIR, NO_PROFILE_ENV, RUN_DISABLED
    NO_PROFILE_ENV = args.no_profile_env
    RUN_DISABLED = args.only_disabled
    for kv in args.env:
        k, _, v = kv.partition("=")
        os.environ[k] = v
    if args.provision_modules:
        MODULE_DIR = Path(args.provision_modules).resolve()
        PROVISION_ONLY.update(x for x in args.provision_only.split(",") if x)
        PROVISION_EXCLUDE.update(x for x in args.provision_exclude.split(",") if x)

    bin_dir = Path(args.bin_dir).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    only = {x for x in args.only.split(",") if x}
    binaries = sorted(p for p in bin_dir.iterdir()
                      if p.is_file() and os.access(p, os.X_OK) and not p.is_symlink()
                      and (p.name.startswith("CoverageTests") or p.name == "TestUnits")
                      and p.suffix == "")
    if only:
        binaries = [b for b in binaries if b.name in only]
    selected: dict[str, set[str]] | None = None
    if args.filter_file:
        selected = {}
        for line in Path(args.filter_file).read_text().splitlines():
            if "\t" in line:
                b, t = line.split("\t", 1)
                selected.setdefault(b, set()).add(t.strip())

    # I03-NEW-106: some cases need runtime modules that the build produces but the
    # suite cannot locate by itself. Provide them when present and not already set,
    # and record what was provided in the manifest.
    auto_env: dict[str, str] = {}
    build_root = bin_dir.parent.parent.parent.parent   # <build>/build_x64/<cfg>/bin/TestUnits -> <build>
    fake_mq = build_root / "TestUnits" / "coverage" / "r160_fake_mq"
    if not os.environ.get("WT_R66_FAKE_MQ_DIR") and fake_mq.is_dir():
        auto_env["WT_R66_FAKE_MQ_DIR"] = str(fake_mq)
    suffix = ".dll" if sys.platform == "win32" else (".dylib" if sys.platform == "darwin" else ".so")
    prefix = "" if sys.platform == "win32" else "lib"
    hft_fact = bin_dir.parent / (prefix + "WtHftStraFact" + suffix)
    if not os.environ.get("R42_HFT_PLUGIN") and hft_fact.is_file():
        auto_env["R42_HFT_PLUGIN"] = str(hft_fact)
    os.environ.update(auto_env)

    manifest = {"start": now(), "bin_dir": str(bin_dir), "workers": args.workers,
                "timeout": args.timeout, "auto_env": auto_env, "extra_env": args.env,
                "no_profile_env": NO_PROFILE_ENV, "only_disabled": RUN_DISABLED,
                "argv": sys.argv, "binaries": []}
    jobs = []
    for b in binaries:
        enabled, disabled, st = list_tests(b, out / "list" / b.name, 120)
        if RUN_DISABLED:
            enabled = list(disabled)
        if selected is not None:
            enabled = [t for t in enabled if t in selected.get(b.name, set())]
        manifest["binaries"].append({"name": b.name, "sha256": sha256(b), "list_status": st,
                                     "enabled": len(enabled), "disabled": len(disabled),
                                     "disabled_names": disabled})
        jobs += [(b, t) for t in enabled]
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False))

    lock = threading.Lock()
    results = []
    done = [0]
    with (out / "results.jsonl").open("w") as rf:
        def work(job):
            r = run_case(job[0], job[1], out, args.timeout)
            with lock:
                rf.write(json.dumps(r, ensure_ascii=False) + "\n")
                rf.flush()
                results.append(r)
                done[0] += 1
                if done[0] % 200 == 0:
                    print(f"{now()} {done[0]}/{len(jobs)}", flush=True)
            return r
        with ThreadPoolExecutor(max_workers=args.workers) as ex:
            list(ex.map(work, jobs))

    summary = {"start": manifest["start"], "end": now(), "total": len(results),
               "by_status": {}, "per_binary": {}, "non_pass": []}
    for r in results:
        summary["by_status"][r["status"]] = summary["by_status"].get(r["status"], 0) + 1
        pb = summary["per_binary"].setdefault(r["binary"], {})
        pb[r["status"]] = pb.get(r["status"], 0) + 1
        if r["status"] != "pass":
            summary["non_pass"].append([r["binary"], r["test"], r["status"]])
    summary["non_pass"].sort()
    summary["disabled_total"] = sum(b["disabled"] for b in manifest["binaries"])
    summary["list_failures"] = [b["name"] for b in manifest["binaries"] if b["list_status"] != "ok"]
    # 2026-11-05 user instruction: items that need another platform, real vendor
    # SDK/data or a production environment are reported as skipped (platform) when
    # this run is not on that platform. They never count as pass.
    ext_file = Path(__file__).with_name("external-platform-items.json")
    summary["external_platform_skipped"] = []
    if ext_file.exists():
        host = {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux" if sys.platform.startswith("linux") else sys.platform)
        host_tags = {host}
        host_tags.update(t for t in os.environ.get("WT_EXTERNAL_PLATFORM_TAGS", "").split(",") if t)
        for item in json.loads(ext_file.read_text(encoding="utf-8")).get("items", []):
            if not host_tags.intersection(item.get("platforms", [])):
                summary["external_platform_skipped"].append({"id": item["id"], "requires": item.get("requires", ""),
                                                             "rerun": item.get("rerun", ""), "status": "skipped(platform)"})
    (out / "summary.json").write_text(json.dumps(summary, indent=1, ensure_ascii=False))
    manifest["end"] = summary["end"]
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1, ensure_ascii=False))
    print(json.dumps({k: summary[k] for k in ("total", "by_status", "disabled_total", "list_failures")} |
                     {"external_platform_skipped": len(summary["external_platform_skipped"])},
                     ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
