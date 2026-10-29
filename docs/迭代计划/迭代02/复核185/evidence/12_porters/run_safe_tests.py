#!/usr/bin/env python3
"""Run reviewed, filesystem-safe porters tests in isolated processes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = Path("/Users/yunjinqi/Documents/new_projects/wondertrader")
EVIDENCE = ROOT / "docs/迭代计划/迭代02/复核185/evidence/12_porters"
CASES = [
    ("r125_schedule_early", "build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_selfthft",
     "R125SelStrategy.ScheduleEarlyReturns"),
    ("r125_multicode_subscription", "build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_selfthft",
     "R125SelStrategy.InitSubscribesAllCodes"),
    ("r69_empty_data_optional", "build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_porter",
     "R69PorterRunnerConfig.MissingExecuterFileAndEmptyDataStillReturnTrue"),
    ("r35_entertag_contract", "build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_porter",
     "R35PorterTrailGetters.CtaLastEnterTagRealHandleIsBlankUnknownAllowsNullOrEmpty"),
    ("r42_dt_mgr_failure_control", "build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore",
     "R42DtMgrTest.InitReportsStoreFailures"),
]

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

results = []
for name, relative_binary, test_filter in CASES:
    binary = ROOT / relative_binary
    case_dir = EVIDENCE / "runs" / name
    case_dir.mkdir(parents=True, exist_ok=True)
    xml_path = case_dir / "gtest.xml"
    stdout_path = case_dir / "stdout.txt"
    stderr_path = case_dir / "stderr.txt"
    command = [str(binary), f"--gtest_filter={test_filter}", f"--gtest_output=xml:{xml_path}"]
    env = os.environ.copy()
    env["LLVM_PROFILE_FILE"] = str(case_dir / "profile-%p.profraw")
    started = time.monotonic()
    timed_out = False
    returncode = None
    try:
        completed = subprocess.run(command, cwd=case_dir, env=env, capture_output=True,
                                   text=True, timeout=15, check=False)
        returncode = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        stdout = exc.stdout.decode("utf-8", errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = exc.stderr.decode("utf-8", errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
    stdout_path.write_text(stdout, encoding="utf-8")
    stderr_path.write_text(stderr, encoding="utf-8")
    tests = failures = errors = 0
    if xml_path.exists():
        root = ET.parse(xml_path).getroot()
        tests = int(root.attrib.get("tests", 0))
        failures = int(root.attrib.get("failures", 0))
        errors = int(root.attrib.get("errors", 0))
    record = {
        "case": name,
        "filter": test_filter,
        "command": command,
        "cwd": str(case_dir),
        "target": str(binary),
        "target_sha256": sha256(binary),
        "timeout_seconds": 15,
        "timed_out": timed_out,
        "returncode": returncode,
        "elapsed_seconds": round(time.monotonic() - started, 6),
        "gtest_xml": str(xml_path) if xml_path.exists() else None,
        "tests": tests,
        "failures": failures,
        "errors": errors,
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
    }
    (case_dir / "result.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    results.append(record)

(EVIDENCE / "safe-tests.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps(results, ensure_ascii=False, indent=2))
if any(item["timed_out"] or item["returncode"] != 0 or item["tests"] != 1 or item["failures"] or item["errors"] for item in results):
    raise SystemExit(1)
