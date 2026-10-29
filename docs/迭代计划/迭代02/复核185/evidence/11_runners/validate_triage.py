import collections
import json
import pathlib
import re

ROOT = pathlib.Path("/Users/yunjinqi/Documents/new_projects/wondertrader")
INPUT = pathlib.Path("/tmp/wt-pending185-20261029T023202Z/groups/11_runners.json")
TRIAGE = ROOT / "docs/迭代计划/迭代02/复核185/triage/11_runners.json"
REPORT = ROOT / "docs/迭代计划/迭代02/复核185/reports/11_runners.md"
input_rows = json.loads(INPUT.read_text())["rows"]
rows = json.loads(TRIAGE.read_text())
required = {
    "lane", "id", "decision", "evidence_level", "source_evidence",
    "test_evidence", "input_reachability", "oracle_basis", "reason",
    "root_cause", "fix", "acceptance", "canonical", "test_change_requests",
    "execution_refs", "decision_request", "external_blocker", "local_remaining",
}
allowed_decisions = {
    "PRODUCT", "TEST", "NOT_BUG", "DUPLICATE", "NEEDS_DECISION",
    "EXTERNAL_BLOCKED", "LOCAL_INCOMPLETE",
}
allowed_evidence = {"STATIC", "LOCAL_REPRO", "PENDING"}
errors = []
if [(x["lane"], x["id"]) for x in rows] != [
    (x["lane"], x["id"]) for x in input_rows
]:
    errors.append("input key/order mismatch")
for index, row in enumerate(rows):
    missing = required - set(row)
    if missing:
        errors.append(f"{index}: missing fields {sorted(missing)}")
    if row.get("decision") not in allowed_decisions:
        errors.append(f"{index}: invalid decision {row.get('decision')}")
    if row.get("evidence_level") not in allowed_evidence:
        errors.append(f"{index}: invalid evidence {row.get('evidence_level')}")
    for key in ("source_evidence", "test_evidence"):
        for reference in row.get(key, []):
            match = re.fullmatch(r"(.+):(\d+)(?:-(\d+))?", reference)
            if not match:
                errors.append(f"{row.get('id')}: malformed line reference {reference}")
                continue
            path = ROOT / match.group(1)
            if not path.is_file():
                errors.append(f"{row.get('id')}: missing evidence file {match.group(1)}")
                continue
            count = len(path.read_text(errors="replace").splitlines())
            start = int(match.group(2))
            end = int(match.group(3) or start)
            if start < 1 or end < start or end > count:
                errors.append(
                    f"{row.get('id')}: line out of range {reference} (file has {count})"
                )
    for reference in row.get("execution_refs", []):
        if not (ROOT / reference).is_file():
            errors.append(f"{row.get('id')}: missing execution ref {reference}")
    for request in row.get("test_change_requests", []):
        for field in (
            "file", "case", "issue", "change", "target", "filter", "expected_result"
        ):
            if field not in request:
                errors.append(f"{row.get('id')}: test request missing {field}")

print(f"rows={len(rows)}")
print(f"decisions={dict(collections.Counter(x['decision'] for x in rows))}")
print(f"evidence={dict(collections.Counter(x['evidence_level'] for x in rows))}")
print(f"report_exists={REPORT.is_file()}")
print(f"errors={errors if errors else 'none'}")
if errors:
    raise SystemExit(1)
