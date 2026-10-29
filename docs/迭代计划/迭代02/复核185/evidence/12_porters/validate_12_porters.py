import json
import re
from pathlib import Path

root = Path.cwd()
triage_path = root / "docs/迭代计划/迭代02/复核185/triage/12_porters.json"
input_path = Path("/tmp/wt-pending185-20261029T023202Z/groups/12_porters.json")
rows = json.loads(triage_path.read_text(encoding="utf-8"))
source_rows = json.loads(input_path.read_text(encoding="utf-8"))["rows"]
allowed = {"PRODUCT", "TEST", "NOT_BUG", "DUPLICATE", "NEEDS_DECISION", "EXTERNAL_BLOCKED", "LOCAL_INCOMPLETE"}
required = {
    "lane", "id", "decision", "evidence_level", "source_evidence", "test_evidence",
    "input_reachability", "oracle_basis", "reason", "root_cause", "fix", "acceptance",
    "canonical", "test_change_requests", "execution_refs", "decision_request",
    "external_blocker", "local_remaining",
}
errors = []
if [(r["lane"], r["id"]) for r in rows] != [(r["lane"], r["id"]) for r in source_rows]:
    errors.append("triage row keys/order differ from input")
if len(rows) != 10:
    errors.append(f"expected 10 rows, found {len(rows)}")

ref_re = re.compile(r"^(.*):(\d+)(?:-(\d+))?$")
for i, row in enumerate(rows):
    missing = required - row.keys()
    if missing:
        errors.append(f"row {i} missing fields: {sorted(missing)}")
    if row.get("decision") not in allowed:
        errors.append(f"row {i} has invalid decision {row.get('decision')}")
    if row.get("evidence_level") not in {"STATIC", "LOCAL_REPRO", "PENDING"}:
        errors.append(f"row {i} has invalid evidence level")
    for field in ("source_evidence", "test_evidence"):
        for ref in row.get(field, []):
            match = ref_re.match(ref)
            if not match:
                errors.append(f"row {i} invalid {field} ref: {ref}")
                continue
            rel, first, last = match.group(1), int(match.group(2)), int(match.group(3) or match.group(2))
            path = root / rel
            if not path.is_file():
                errors.append(f"row {i} missing evidence source: {rel}")
                continue
            lines = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
            if first < 1 or last < first or last > lines:
                errors.append(f"row {i} line range out of bounds: {ref} (file lines={lines})")
    for ref in row.get("execution_refs", []):
        if not (root / ref).is_file():
            errors.append(f"row {i} missing execution ref: {ref}")
    for change in row.get("test_change_requests", []):
        need = {"file", "case", "issue", "change", "target", "filter", "expected_result"}
        if need - change.keys():
            errors.append(f"row {i} incomplete test_change_request: {sorted(need - change.keys())}")
        if not (root / change.get("file", "")).is_file():
            errors.append(f"row {i} missing test change file: {change.get('file')}")
    if row.get("decision") == "NEEDS_DECISION" and not isinstance(row.get("decision_request"), dict):
        errors.append(f"row {i} missing decision_request")
    if row.get("decision") != "NEEDS_DECISION" and row.get("decision_request") is not None:
        errors.append(f"row {i} unexpected decision_request")
    if row.get("decision") == "EXTERNAL_BLOCKED" and not isinstance(row.get("external_blocker"), dict):
        errors.append(f"row {i} missing external_blocker")
    if row.get("decision") != "EXTERNAL_BLOCKED" and row.get("external_blocker") is not None:
        errors.append(f"row {i} unexpected external_blocker")

counts = {}
for row in rows:
    counts[row["decision"]] = counts.get(row["decision"], 0) + 1
print(json.dumps({"rows": len(rows), "counts": counts, "errors": errors}, ensure_ascii=False, indent=2))
raise SystemExit(1 if errors else 0)
