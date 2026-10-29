#!/usr/bin/env python3
"""Validate 07_storage triage shape, source line references, and evidence paths."""

import json
from pathlib import Path
import re


ROOT = Path("/Users/yunjinqi/Documents/new_projects/wondertrader")
INPUT = Path("/tmp/wt-pending185-20261029T023202Z/groups/07_storage.json")
TRIAGE = ROOT / "docs/迭代计划/迭代02/复核185/triage/07_storage.json"
REPORT = ROOT / "docs/迭代计划/迭代02/复核185/reports/07_storage.md"
OUT = ROOT / "docs/迭代计划/迭代02/复核185/evidence/07_storage/validation.json"

required = {
    "lane", "id", "decision", "evidence_level", "source_evidence", "test_evidence",
    "input_reachability", "oracle_basis", "reason", "root_cause", "fix", "acceptance",
    "canonical", "test_change_requests", "execution_refs", "decision_request",
    "external_blocker", "local_remaining",
}
test_request_required = {"file", "case", "issue", "change", "target", "filter", "expected_result"}
decisions = {"PRODUCT", "TEST", "NOT_BUG", "DUPLICATE", "NEEDS_DECISION", "EXTERNAL_BLOCKED", "LOCAL_INCOMPLETE"}
levels = {"STATIC", "LOCAL_REPRO", "PENDING"}


def check_evidence_reference(reference: str, errors: list[str], kind: str) -> None:
    match = re.fullmatch(r"(.+):(\d+)(?:-(\d+))?", reference)
    if not match:
        errors.append(f"invalid {kind} reference: {reference}")
        return
    rel, first, last = match.group(1), int(match.group(2)), int(match.group(3) or match.group(2))
    path = ROOT / rel
    if not path.is_file():
        errors.append(f"missing {kind} source file: {rel}")
        return
    line_count = sum(1 for _ in path.open(encoding="utf-8", errors="replace"))
    if first < 1 or last < first or last > line_count:
        errors.append(f"out-of-range {kind} line: {reference} (file has {line_count})")


source = json.loads(INPUT.read_text(encoding="utf-8"))
rows = json.loads(TRIAGE.read_text(encoding="utf-8"))
errors = []
expected_keys = [(row["lane"], row["id"]) for row in source["rows"]]
actual_keys = [(row.get("lane"), row.get("id")) for row in rows]
if expected_keys != actual_keys:
    errors.append("triage keys/order do not match input rows")
if len(rows) != source["count"]:
    errors.append(f"row count {len(rows)} != expected {source['count']}")
if not REPORT.is_file():
    errors.append("missing markdown report")

for index, row in enumerate(rows):
    missing = required - row.keys()
    extra = row.keys() - required
    if missing:
        errors.append(f"row {index} missing fields: {sorted(missing)}")
    if extra:
        errors.append(f"row {index} has unexpected fields: {sorted(extra)}")
    if row.get("decision") not in decisions:
        errors.append(f"row {index} invalid decision: {row.get('decision')}")
    if row.get("evidence_level") not in levels:
        errors.append(f"row {index} invalid evidence level: {row.get('evidence_level')}")
    for field in ("source_evidence", "test_evidence"):
        refs = row.get(field, [])
        if not isinstance(refs, list):
            errors.append(f"row {index} {field} is not a list")
            continue
        for ref in refs:
            check_evidence_reference(ref, errors, field)
    for field in ("execution_refs",):
        for ref in row.get(field, []):
            if not (ROOT / ref).is_file():
                errors.append(f"row {index} missing execution evidence: {ref}")
    for test_request in row.get("test_change_requests", []):
        missing_request = test_request_required - test_request.keys()
        if missing_request:
            errors.append(f"row {index} test request missing fields: {sorted(missing_request)}")
        if test_request.get("file") and not (ROOT / test_request["file"]).is_file():
            errors.append(f"row {index} missing requested test file: {test_request['file']}")
    if row.get("decision") == "DUPLICATE" and not row.get("canonical"):
        errors.append(f"row {index} duplicate missing canonical")
    if row.get("decision") == "NEEDS_DECISION" and not isinstance(row.get("decision_request"), dict):
        errors.append(f"row {index} needs decision but has no decision_request")
    if row.get("decision") == "EXTERNAL_BLOCKED" and not isinstance(row.get("external_blocker"), dict):
        errors.append(f"row {index} externally blocked but has no external_blocker")

result = {
    "triage": str(TRIAGE.relative_to(ROOT)),
    "report": str(REPORT.relative_to(ROOT)),
    "input_count": source["count"],
    "actual_count": len(rows),
    "decisions": {name: sum(row.get("decision") == name for row in rows) for name in sorted(decisions)},
    "evidence_levels": {name: sum(row.get("evidence_level") == name for row in rows) for name in sorted(levels)},
    "errors": errors,
    "valid": not errors,
}
OUT.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result, ensure_ascii=False, indent=2))
if errors:
    raise SystemExit(1)
