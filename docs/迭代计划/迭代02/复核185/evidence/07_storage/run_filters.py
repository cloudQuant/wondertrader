#!/usr/bin/env python3
"""Run selected CoverageTests filters in isolated evidence directories."""

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid


ROOT = Path("/Users/yunjinqi/Documents/new_projects/wondertrader")
BIN = ROOT / "build-cov/build_x64/Debug/bin/TestUnits/CoverageTests"
OUT_ROOT = ROOT / "docs/迭代计划/迭代02/复核185/evidence/07_storage/runs"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--filter", action="append", required=True)
    args = parser.parse_args()

    build_sha = sha256(BIN)
    run_id = dt.datetime.now().strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]
    results = []

    for index, gtest_filter in enumerate(args.filter, 1):
        safe_name = gtest_filter.replace("*", "all").replace(".", "_").replace(":", "_")
        case_dir = OUT_ROOT / f"{run_id}-{index:02d}-{safe_name}"
        cwd = case_dir / "cwd"
        cwd.mkdir(parents=True, exist_ok=False)
        profile = case_dir / "profile-%p.profraw"
        xml_path = case_dir / "results.xml"
        log_path = case_dir / "stdout.log"
        command = [str(BIN), f"--gtest_filter={gtest_filter}", f"--gtest_output=xml:{xml_path}"]
        env = os.environ.copy()
        env["LLVM_PROFILE_FILE"] = str(profile)
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        try:
            proc = subprocess.run(
                command,
                cwd=cwd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=15,
                check=False,
            )
            output = proc.stdout
            returncode = proc.returncode
            timed_out = False
        except subprocess.TimeoutExpired as exc:
            output = exc.stdout or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", errors="replace")
            returncode = None
            timed_out = True
        log_path.write_text(output, encoding="utf-8")
        xml_count = None
        xml_failures = None
        if xml_path.exists():
            import xml.etree.ElementTree as ET

            root = ET.parse(xml_path).getroot()
            xml_count = int(root.attrib.get("tests", "0"))
            xml_failures = int(root.attrib.get("failures", "0"))
        results.append({
            "filter": gtest_filter,
            "command": command,
            "cwd": str(cwd),
            "started_utc": started,
            "returncode": returncode,
            "timeout_seconds": 15,
            "timed_out": timed_out,
            "xml": str(xml_path) if xml_path.exists() else None,
            "xml_tests": xml_count,
            "xml_failures": xml_failures,
            "stdout_log": str(log_path),
            "binary_sha256": build_sha,
        })

    manifest = {
        "run_id": run_id,
        "binary": str(BIN),
        "binary_sha256": build_sha,
        "results": results,
    }
    manifest_path = OUT_ROOT / f"{run_id}-manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
