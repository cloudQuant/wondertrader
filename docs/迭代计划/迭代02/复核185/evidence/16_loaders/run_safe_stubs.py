#!/usr/bin/env python3
"""Run reviewed, enabled loader/Femas stub tests in isolated processes."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

ROOT = Path("/Users/yunjinqi/Documents/new_projects/wondertrader")
EVIDENCE = ROOT / "docs/迭代计划/迭代02/复核185/evidence/16_loaders"
BIN_DIR = ROOT / "build-cov/build_x64/Debug/bin/TestUnits"
CASES = [
    ("CoverageTests_t_CTPLoader", "t26_ctploader_flow.login_success_writes_session_refs_and_queues_instrument_query"),
    ("CoverageTests_t_CTPLoader", "t26_ctploader_instrument.czce_future_code_is_expanded_and_altcode_kept"),
    ("CoverageTests_t_CTPLoader", "t26_ctploader_dump.margin_response_dumps_fees_to_configured_relative_name_with_product_fallback"),
    ("CoverageTests_t_TraderFemas", "R93FemasTest.authenticate_login_and_query_front_reach_ready"),
    ("CoverageTests_t_TraderFemas", "R93FemasTest.connect_and_release_drive_stub_lifecycle"),
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    results = []
    for index, (binary_name, test_filter) in enumerate(CASES, 1):
        binary = BIN_DIR / binary_name
        case_dir = EVIDENCE / f"run-{index:02d}"
        case_dir.mkdir(parents=True, exist_ok=True)
        workdir = Path(tempfile.mkdtemp(prefix=f"wt16-loaders-{index:02d}-"))
        xml_path = case_dir / "gtest.xml"
        profile_path = case_dir / "profile-%p.profraw"
        command = [
            str(binary),
            f"--gtest_filter={test_filter}",
            "--gtest_color=no",
            f"--gtest_output=xml:{xml_path}",
        ]
        env = os.environ.copy()
        env["LLVM_PROFILE_FILE"] = str(profile_path)
        started = time.monotonic()
        try:
            completed = subprocess.run(
                command,
                cwd=workdir,
                env=env,
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
            stdout, stderr = completed.stdout, completed.stderr
            returncode = completed.returncode
            timed_out = False
        except subprocess.TimeoutExpired as error:
            stdout = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
            stderr = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else (error.stderr or "")
            returncode = None
            timed_out = True
        elapsed = round(time.monotonic() - started, 3)
        (case_dir / "stdout.txt").write_text(stdout, encoding="utf-8")
        (case_dir / "stderr.txt").write_text(stderr, encoding="utf-8")
        counts = None
        if xml_path.exists():
            root = ET.parse(xml_path).getroot()
            counts = {key: int(root.attrib.get(key, "0")) for key in ("tests", "failures", "disabled", "errors")}
        results.append({
            "binary": str(binary),
            "target_sha256": sha256(binary),
            "filter": test_filter,
            "command": command,
            "cwd": str(workdir),
            "returncode": returncode,
            "timed_out": timed_out,
            "elapsed_seconds": elapsed,
            "gtest_counts": counts,
            "xml": str(xml_path) if xml_path.exists() else None,
            "stdout": str(case_dir / "stdout.txt"),
            "stderr": str(case_dir / "stderr.txt"),
            "profile_files": [str(path) for path in case_dir.glob("*.profraw")],
        })
        print(json.dumps(results[-1], ensure_ascii=False))
    report = {"runs": results}
    (EVIDENCE / "safe_stub_runs.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
