#!/usr/bin/env python3
"""Combine reviewer classification, fix overrides and final regression results into
per-item final dispositions for the 213 baseline items and the I03-NEW issues.

Usage: build_final.py --review DIR --overrides overrides.json --final DIR --out out.json
  --final DIR must contain run/ run-asan/ run-tsan/ [run-lsan/] (results.jsonl each)
  and optionally fixtures/summary.json
"""
import argparse, fnmatch, glob, json, os, re, collections

TOOL_TSAN_EXIT = "tsan-exit-after-pass"   # PASSED then SEGV/BUS at exit (atexit/dlclose tool limit)


def load_results(path):
    res = {}
    if not os.path.exists(path):
        return res
    for line in open(path):
        r = json.loads(line)
        res[(r["binary"], r["test"])] = r
    return res


def classify_tsan(r):
    """Return effective status for a TSan result, honouring the verified tool limitation."""
    st = r["status"]
    if st == "pass":
        return "pass"
    log = os.path.join(r.get("cwd", ""), "log.txt")
    text = open(log, errors="replace").read() if os.path.exists(log) else ""
    if st.startswith("signal-6") or st == "signal-6":
        if "[  PASSED  ]" in text and "DEADLYSIGNAL" in text and "WARNING: ThreadSanitizer" not in text:
            return TOOL_TSAN_EXIT
    if "WARNING: ThreadSanitizer: thread leak" in text and "data race" not in text and "[  PASSED  ]" in text:
        return "tsan-thread-leak-only"
    return st


def match(res, binary, pattern):
    out = {}
    for (b, t), r in res.items():
        if b == binary and (t == pattern or fnmatch.fnmatchcase(t, pattern)):
            out[t] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--review", required=True)
    ap.add_argument("--overrides", required=True)
    ap.add_argument("--final", required=True)
    ap.add_argument("--tracking", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    runs = {k: load_results(os.path.join(a.final, k, "results.jsonl"))
            for k in ("run", "run-asan", "run-tsan", "run-lsan")}
    fixtures = {}
    fp = os.path.join(a.final, "fixtures", "summary.json")
    if os.path.exists(fp):
        d = json.load(open(fp))
        items = d.get("suites") if isinstance(d, dict) and "suites" in d else d
        if isinstance(items, dict):
            items = list(items.values())
        for x in items:
            fixtures[x.get("suite")] = x
    ov = json.load(open(a.overrides))
    track = {r["id"]: r for r in json.load(open(a.tracking))["rows"]}

    review = {}
    for p in sorted(glob.glob(os.path.join(a.review, "slice*-result.json"))):
        d = json.load(open(p))
        for kind in ("items", "new_issues"):
            for it in d.get(kind, []):
                it["_kind"] = kind
                review[it["id"]] = it

    out = []
    for iid, it in review.items():
        o = ov.get(iid, {})
        cls = o.get("post_class", it.get("class", "?"))[:1]
        tests = list(it.get("covering_tests") or []) + list(o.get("extra_tests") or [])
        row = {"id": iid, "kind": it["_kind"], "review_class": it.get("class"), "post_class": cls,
               "current_status": track.get(iid, {}).get("状态") if iid in track else it.get("current_status"),
               "tests": {}, "fixture": {}, "problems": [], "external_pending": o.get("external_pending", it.get("external_pending") or []),
               "note": o.get("note", "")}
        matched = 0
        for ct in tests:
            b, t = ct.get("binary"), ct.get("test")
            if not b or not t:
                continue
            if re.search(r"[^\x00-\x7f]", t):
                t = re.split(r"[^\x00-\x7f]", t, 1)[0] or "*"
            hits = match(runs["run"], b, t)
            if not hits:
                row["problems"].append(f"未在最终普通回归中找到用例 {b}::{t}")
            for tn, r in hits.items():
                matched += 1
                key = f"{b}::{tn}"
                st = {"run": r["status"]}
                if r["status"] not in ("pass",):
                    if not (r["status"] == "skipped" and o.get("allow_skip")):
                        row["problems"].append(f"普通回归 {key} = {r['status']}")
                ra = runs["run-asan"].get((b, tn))
                if ra:
                    st["asan"] = ra["status"]
                    if ra["status"] not in ("pass", "skipped"):
                        row["problems"].append(f"ASan {key} = {ra['status']}")
                rt = runs["run-tsan"].get((b, tn))
                if rt:
                    eff = classify_tsan(rt)
                    st["tsan"] = eff
                    if eff not in ("pass", "skipped", TOOL_TSAN_EXIT, "tsan-thread-leak-only"):
                        row["problems"].append(f"TSan {key} = {eff}")
                row["tests"][key] = st
        for fs in (it.get("fixture_suites") or []) + (o.get("extra_fixtures") or []):
            name = (fs.get("dir") or "").rstrip("/").split("/")[-1] if isinstance(fs, dict) else fs
            if not name:
                continue
            fr = fixtures.get(name)
            if fr is None:
                row["fixture"][name] = "未运行"
                if fixtures:
                    row["problems"].append(f"夹具 {name} 未在最终运行中")
            else:
                row["fixture"][name] = fr.get("verdict")
                if fr.get("verdict") not in ("pass", "expected-red", "not-applicable"):
                    row["problems"].append(f"夹具 {name} = {fr.get('verdict')}")
        row["matched_tests"] = matched
        if matched == 0 and not row["fixture"] and not o.get("no_test_ok"):
            row["problems"].append("无可映射的运行证据")
        # final status
        if cls == "D":
            final = "阻塞"
        elif cls == "E":
            final = "阻塞"
        elif cls == "C":
            final = "待验证"
        elif row["problems"]:
            final = "待验证"
        elif cls == "A":
            final = "已验收"
        elif cls == "B":
            final = "本地验证通过"
        else:
            final = row["current_status"]
        row["final_status"] = final
        out.append(row)
    json.dump(out, open(a.out, "w"), ensure_ascii=False, indent=1)
    c = collections.Counter((r["kind"], r["final_status"]) for r in out)
    for k, v in sorted(c.items()):
        print(k, v)
    bad = [r for r in out if r["problems"] and r["post_class"] in ("A", "B")]
    print("A/B items with problems:", len(bad))
    for r in bad[:60]:
        print(" ", r["id"], "|", "; ".join(r["problems"])[:300])


if __name__ == "__main__":
    main()
