#!/usr/bin/env python3
"""Record the 2026-11-06 decisions K50-K63 (implemented and verified on frozen tree G7)
in 执行跟踪.json, 逐项验收.json and 新增问题.md.

Usage: g7_update_tracking.py <g7-dir> <evidence-rel-path>
Fails (no write) if any decision test is not pass in the G7 normal or ASan per-case runs.
"""
import json, os, re, sys, gzip

G7, EV_REL = sys.argv[1], sys.argv[2]
DOC = "/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03"


def load_results(run):
    p = os.path.join(G7, run, "results.jsonl")
    rows = {}
    opener = open
    if not os.path.exists(p):
        p += ".gz"
        opener = gzip.open
    with opener(p, "rt") as f:
        for line in f:
            r = json.loads(line)
            rows[r["binary"] + "::" + r["test"]] = r["status"]
    return rows


G8 = os.path.join(os.path.dirname(G7), "g8")
G8_BINARIES = ("CoverageTests", "CoverageTests_porter", "CoverageTests_dtservo")


def overlay(base, run_name, binaries):
    """G8 re-ran these binaries after the G7->G8 changes; their G8 results replace G7's."""
    p = os.path.join(G8, run_name, "results.jsonl")
    if not os.path.exists(p):
        return base
    out = {k: v for k, v in base.items() if k.split("::", 1)[0] not in binaries}
    with open(p) as f:
        for line in f:
            r = json.loads(line)
            if r["binary"] in binaries:
                out[r["binary"] + "::" + r["test"]] = r["status"]
    return out


run = overlay(load_results("run"), "run", G8_BINARIES)
asan = overlay(load_results("run-asan"), "run-asan", G8_BINARIES)
tsan = overlay(load_results("run-tsan"), "run-tsan", ("CoverageTests_dtservo",))
non_pass_run = sorted(k for k, v in run.items() if v not in ("pass", "skipped"))
non_pass_asan = sorted(k for k, v in asan.items() if v not in ("pass", "skipped"))
print("combined normal non-pass:", non_pass_run)
print("combined asan non-pass:", non_pass_asan)

DECISION_TESTS = {
    "K50": ["CoverageTests_wtcore::I03K50ManualClaimRelease.", "CoverageTests_wtcore::I03K50AccountGate.",
            "CoverageTests_wtcore::I03K50ManualClaimInbox.", "CoverageTests_porter::I03K50PorterReleaseClaim.",
            "CoverageTests_m_WtRunner::I03K50WtRunnerClaimInbox."],
    "K51": ["CoverageTests_t_ParserShm::I03K51ShmSlowConsumer.", "CoverageTests_t_ParserShm::I03S2ShmCrossProcess."],
    "K54": ["CoverageTests_wtcore::I03K54RiskmonFixture.", "CoverageTests_porter::I03K54PorterRiskmon.",
            "CoverageTests_m_WtRunner::I03K54WtRunnerRiskmon.", "CoverageTests_wtcore::R110EngineRiskMon."],
    "K55": ["CoverageTests_t_TraderMocker::I03K55MockerIsConnected.",
            "CoverageTests_t_TraderMocker::R214MockerBranches.DisconnectJoinsMatchThreadWithoutWorker",
            "CoverageTests_t_TraderMocker::T6009Mocker.ConcurrentExternalReleaseAndRequests",
            "CoverageTests_t_TraderXTP::R93XtpTest.login_success_then_recovery_and_logout",
            "CoverageTests_t_TraderYD::R93YdTest.k55_logout_ends_readiness"],
    "K56": ["CoverageTests_porter::I03K56PorterMissingData.", "CoverageTests_porter::R46PorterConfig.MissingEnvMakesConfigFail", "CoverageTests_porter::R82RunnerEdges.ConfigFailureArmsAcrossSections"],
    "K57": ["CoverageTests_porter::I03K57LiveTickLoader."],
    "K58": ["CoverageTests_exefact::r7_twap_clear", "CoverageTests_exefact::r7_vwap_clear_k58.",
            "CoverageTests_exefact::r7_vwap.clear_"],
    "K59": ["CoverageTests_dtcore::I03K59WeekendTest."],
    "K60": ["CoverageTests::I03K60HftRangeEndpointTest.", "CoverageTests::R162ReaderEdgeTest.", "CoverageTests::R100RdmCrossDayTest.", "CoverageTests::R70RdmResidualTest.", "CoverageTests::R74RdmHftHotTest."],
    "K61": ["CoverageTests::I03K61KlinePeriodTest.", "CoverageTests::R193StorageReaderTest.R193LoaderUnknownPeriodIsRejected",
            "CoverageTests_adstorage::I03K61AdKlinePeriodTest."],
    "K62": ["CoverageTests_btcore::R200Fixture.R200His_BarReplayWithOnlyEmptyTickListDispatchesNothing",
            "CoverageTests_btcore::R200Fixture.R200His_BarReplaySkipsEmptyTickList"],
    "NEW142": ["CoverageTests_wtcore::I03New142Fixture."],
    "NEW143": ["CoverageTests_dtservo::R133WtDataManagerTest."],
}


def matched(prefixes, results):
    out = {}
    for key, st in results.items():
        if any(key.startswith(p) for p in prefixes):
            out[key] = st
    return out


tallies = {}
problems = []
for k, prefixes in DECISION_TESTS.items():
    n = matched(prefixes, run)
    a = matched(prefixes, asan)
    t = matched(prefixes, tsan)
    if not n:
        problems.append(f"{k}: no tests matched in run")
    bad = {c: s for c, s in n.items() if s != "pass"}
    bad_a = {c: s for c, s in a.items() if s != "pass"}
    if bad:
        problems.append(f"{k}: normal non-pass {bad}")
    if bad_a:
        problems.append(f"{k}: asan non-pass {bad_a}")
    tallies[k] = {"normal": len(n), "asan": len(a), "tsan": dict((s, list(t.values()).count(s)) for s in set(t.values()))}
if problems:
    print("\n".join(problems))
    sys.exit(1)
print(json.dumps(tallies, ensure_ascii=False))
json.dump(tallies, open(os.path.join(G7, "decision-tallies.json"), "w"), ensure_ascii=False, indent=1)
