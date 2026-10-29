import collections
import json
import os
import re

triage_path = "docs/迭代计划/迭代02/复核185/triage/15_vendors.json"
input_path = "/tmp/wt-pending185-20261029T023202Z/groups/15_vendors.json"
rows = json.load(open(triage_path, encoding="utf-8"))
source = json.load(open(input_path, encoding="utf-8"))
required = {
    "lane", "id", "decision", "evidence_level", "source_evidence",
    "test_evidence", "input_reachability", "oracle_basis", "reason",
    "root_cause", "fix", "acceptance", "canonical", "test_change_requests",
    "execution_refs", "decision_request", "external_blocker", "local_remaining",
}
allowed = {
    "PRODUCT", "TEST", "NOT_BUG", "DUPLICATE", "NEEDS_DECISION",
    "EXTERNAL_BLOCKED", "LOCAL_INCOMPLETE",
}
assert len(rows) == source["count"] == 15
assert [(r["lane"], r["id"]) for r in rows] == [
    (r["lane"], r["id"]) for r in source["rows"]
]
counts = collections.Counter()
for row in rows:
    counts[row["decision"]] += 1
    assert set(row) == required, (row["id"], set(row) ^ required)
    assert row["decision"] in allowed
    assert row["evidence_level"] in {"STATIC", "LOCAL_REPRO", "PENDING"}
    assert (row["decision"] == "NEEDS_DECISION") == (row["decision_request"] is not None)
    assert (row["decision"] == "EXTERNAL_BLOCKED") == (row["external_blocker"] is not None)
    for key in ("source_evidence", "test_evidence"):
        for ref in row[key]:
            match = re.fullmatch(r"(.+?):(\d+)(?:-(\d+))?", ref)
            assert match, (row["id"], ref)
            path = match.group(1)
            start = int(match.group(2))
            end = int(match.group(3) or match.group(2))
            assert os.path.isfile(path), (row["id"], path)
            with open(path, encoding="utf-8", errors="replace") as file:
                line_count = sum(1 for _ in file)
            assert 1 <= start <= end <= line_count, (row["id"], ref, line_count)
    for request in row["test_change_requests"]:
        assert set(request) == {"file", "case", "issue", "change", "target", "filter", "expected_result"}
        assert request["file"]

print("validated rows:", len(rows))
print("classification counts:", dict(counts))
print("input keys/order: exact")
print("source/test paths and line ranges: valid")
