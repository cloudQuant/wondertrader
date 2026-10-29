import json
import pathlib
import re

ROOT = pathlib.Path("/Users/yunjinqi/Documents/new_projects/wondertrader")
TRIAGE = ROOT / "docs/迭代计划/迭代02/复核185/triage/08_ad_storage.json"
rows = json.loads(TRIAGE.read_text())
required = set("""lane id decision evidence_level source_evidence test_evidence
input_reachability oracle_basis reason root_cause fix acceptance canonical
test_change_requests execution_refs decision_request external_blocker
local_remaining""".split())
allowed = {"PRODUCT", "TEST", "NOT_BUG", "DUPLICATE", "NEEDS_DECISION",
           "EXTERNAL_BLOCKED", "LOCAL_INCOMPLETE"}
problems = []
for row in rows:
    missing = required - row.keys()
    if missing:
        problems.append((row.get("id"), "missing fields", sorted(missing)))
    if row.get("decision") not in allowed:
        problems.append((row.get("id"), "invalid decision", row.get("decision")))
    if row.get("evidence_level") not in {"STATIC", "LOCAL_REPRO", "PENDING"}:
        problems.append((row.get("id"), "invalid evidence level"))
    if row.get("decision") == "NEEDS_DECISION" and not isinstance(row.get("decision_request"), dict):
        problems.append((row.get("id"), "missing decision_request"))
    if row.get("decision") == "EXTERNAL_BLOCKED" and not isinstance(row.get("external_blocker"), dict):
        problems.append((row.get("id"), "missing external_blocker"))
    for field in ("source_evidence", "test_evidence"):
        for ref in row.get(field, []):
            match = re.fullmatch(r"(.+):(\d+)(?:-(\d+))?", ref)
            if not match:
                problems.append((row.get("id"), field, "bad ref", ref))
                continue
            path = ROOT / match.group(1)
            if not path.exists():
                problems.append((row.get("id"), field, "missing path", match.group(1)))
                continue
            end = int(match.group(3) or match.group(2))
            line_count = sum(1 for _ in path.open(errors="replace"))
            if end > line_count:
                problems.append((row.get("id"), field, "line out of range", ref, line_count))
print("rows", len(rows), "unique ids", len({(r["lane"], r["id"]) for r in rows}))
print("counts", {key: sum(r["decision"] == key for r in rows)
                  for key in sorted({r["decision"] for r in rows})})
print("problems", problems)
raise SystemExit(bool(problems))
