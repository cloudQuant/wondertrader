#!/usr/bin/env python3
"""Map each item's covering tests (from reviewer slice results) to one or more
regression result files. Usage:
  item_matrix.py --review DIR --results NAME=results.jsonl [--results ...] --out matrix.json
Covering test entries may use 'Suite.*' wildcards or exact 'Suite.Case'.
"""
import argparse, fnmatch, glob, json, os


def load_results(path):
    res = {}
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            res[(r["binary"], r["test"])] = r["status"]
    return res


def match(results, binary, pattern):
    hits = {}
    for (b, t), st in results.items():
        if b != binary:
            continue
        if t == pattern or fnmatch.fnmatchcase(t, pattern):
            hits[t] = st
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--review", required=True)
    ap.add_argument("--results", action="append", default=[])
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    runs = {}
    for spec in a.results:
        name, _, path = spec.partition("=")
        runs[name] = load_results(path)
    items = []
    for p in sorted(glob.glob(os.path.join(a.review, "slice*-result.json"))):
        d = json.load(open(p))
        for kind in ("items", "new_issues"):
            for it in d.get(kind, []):
                it["_kind"] = kind
                it["_slice"] = os.path.basename(p)
                items.append(it)
    out = []
    for it in items:
        row = {"id": it["id"], "kind": it["_kind"], "class": it.get("class"), "runs": {}}
        for name, res in runs.items():
            agg = {"matched": 0, "by_status": {}, "unmatched_patterns": [], "non_pass": []}
            for ct in it.get("covering_tests", []) or []:
                b, t = ct.get("binary"), ct.get("test")
                if not b or not t:
                    continue
                hits = match(res, b, t)
                if not hits:
                    agg["unmatched_patterns"].append(f"{b}::{t}")
                for tn, st in hits.items():
                    agg["matched"] += 1
                    agg["by_status"][st] = agg["by_status"].get(st, 0) + 1
                    if st != "pass":
                        agg["non_pass"].append(f"{b}::{tn}={st}")
            row["runs"][name] = agg
        out.append(row)
    json.dump(out, open(a.out, "w"), ensure_ascii=False, indent=1)
    # short console summary
    for name in runs:
        nm = sum(1 for r in out if r["runs"][name]["matched"] == 0)
        bad = [r["id"] for r in out if r["runs"][name]["non_pass"]]
        unm = sum(len(r["runs"][name]["unmatched_patterns"]) for r in out)
        print(f"{name}: items={len(out)} no-matched-tests={nm} with-non-pass={len(bad)} unmatched-patterns={unm}")
        if bad:
            print("  non-pass:", ", ".join(bad[:40]))


if __name__ == "__main__":
    main()
