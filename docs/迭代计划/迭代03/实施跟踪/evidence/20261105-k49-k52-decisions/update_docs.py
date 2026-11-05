#!/usr/bin/env python3
"""Record user decisions K49=A and K52=否 (2026-11-05) in the iteration-03 documents.

Reads run XML from the k49 scratch runs, copies evidence into the repo, and updates:
执行跟踪.json (T3-015 row + summary counts), 新增问题.md (I03-NEW-119), 决策积压清单.md,
最终验收报告.md, 遗留问题清单.md, 迭代计划.md status line, 逐项验收.json.
"""
import json, os, re, shutil, sys, xml.etree.ElementTree as ET

K = os.path.dirname(os.path.abspath(__file__))
R = "/Users/yunjinqi/Documents/new_projects/wondertrader"
DOC = R + "/docs/迭代计划/迭代03"
EV_REL = "实施跟踪/evidence/20261105-k49-k52-decisions"
EV = DOC + "/" + EV_REL
TEST = "I03S5TraderCTPMini.K49InvalidTradingDayRecoversOnNextValidLogin"
DATE_TESTS = ["I03S5TraderCTPMini.ValidTradingDayLoginAndNightDatesStayCorrect",
              "I03S5TraderCTPMini.InvalidTradingDayFailsClosed",
              "I03S5TraderCTPMini.InvalidTradingDayMatrixIsRejected", TEST]


def xml_results(path):
    res = {}
    root = ET.parse(path).getroot()
    for tc in root.iter("testcase"):
        name = tc.get("classname") + "." + tc.get("name")
        if tc.get("name", "").startswith("DISABLED_"):
            continue
        if tc.get("status") == "notrun" or tc.find("skipped") is not None:
            res[name] = "skipped"
        elif tc.find("failure") is not None:
            res[name] = "fail"
        else:
            res[name] = "pass"
    return res


runs = {k: xml_results(f"{K}/run-{k}/all.xml") for k in ("binary", "asan", "tsan", "tsan-nodlclose")}
for k, r in runs.items():
    bad = {n: s for n, s in r.items() if s != "pass"}
    if bad:
        sys.exit(f"{k} has non-pass cases: {bad}")
    for t in DATE_TESTS:
        if r.get(t) != "pass":
            sys.exit(f"{k} missing {t}")
mut = {k: xml_results(f"{K}/run-{k}/k49.xml") for k in ("mut1-sticky", "mut2-stale-diag")}
for k, r in mut.items():
    if r.get(TEST) != "fail" or any(r.get(t) != "pass" for t in DATE_TESTS[:3]):
        sys.exit(f"mutation {k} not red-only-on-K49: {r}")
for k in ("asan", "tsan"):
    log = open(f"{K}/run-{k}/all.log", errors="ignore").read()
    if re.search(r"ERROR: (AddressSanitizer|UndefinedBehaviorSanitizer)|WARNING: ThreadSanitizer|runtime error:", log):
        sys.exit(f"{k} log has sanitizer report")

# ---- evidence copy
os.makedirs(EV, exist_ok=True)
for sub in ("logs", "run-green", "run-mut1-sticky", "run-mut2-stale-diag", "run-binary", "run-asan", "run-tsan", "run-tsan-nodlclose"):
    dst = f"{EV}/{sub}"
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(f"{K}/{sub}"):
        if f.endswith((".profraw", ".dylib")):
            continue
        shutil.copy2(f"{K}/{sub}/{f}", dst)
shutil.copy2(f"{K}/san.sh", f"{EV}/san.sh")
shutil.copy2(__file__, f"{EV}/update_docs.py")
n_bin = len(runs["binary"])
assert n_bin == 54, n_bin

# ---- 执行跟踪.json
tp = DOC + "/执行跟踪.json"
d = json.load(open(tp))
line = ("2026-11-05 用户裁定 K49=A（GetTradingDay 失效导致登录 fail-closed 后，下一次合法登录自动就绪，无需人工确认；现实现即合同）："
        "新增 I03S5TraderCTPMini.K49InvalidTradingDayRecoversOnNextValidLogin（失效→断开→重连→合法登录→就绪，交易日/诊断串/回报日期/查询按新交易日）；"
        "红控：冻结副本两种变异（失效后坚持拒绝＝选项 B 行为、恢复后不清诊断串）下仅该例失败；"
        f"绿：CoverageTests_t_TraderCTPMini 全二进制 普通/ASan+UBSan/TSan 各 {n_bin}/{n_bin} 通过、0 数据竞争/0 ASan/UBSan 报告"
        "（TSan 全二进制在全部用例通过后退出期 SEGV，exit 134，为已登记 atexit/dlclose 工具限制；dlclose 空实现下 exit 0、54/54、0 报告）；"
        "阻塞 → 本地验证通过；真实 CTPMini SDK GetTradingDay 合同仍外部。证据 " + EV_REL + "/README.md")
for row in d["rows"]:
    if row["id"] == "T3-015":
        assert row["状态"] == "阻塞", row["状态"]
        row["状态"] = "本地验证通过"
        row["剩余事项"] = [x for x in row["剩余事项"] if "人工恢复" not in x and "invalid login date" not in x]
        row["剩余事项"].append("K49=A 已裁定（2026-11-05）：恢复策略为下一次合法登录自动就绪，已补用例")
        fa = row["最终验收"]
        fa["结论"] = "本地验证通过"
        fa["依据"] = "本机部分全部满足；剩余为外部条件或已登记的已知限制"
        fa["覆盖用例数"] = 4
        fa["用例结果"] = {"普通": {"pass": 4}, "ASan+UBSan": {"pass": 4}, "TSan": {"pass": 4}}
        fa["说明"] = "K49=A 用户裁定（2026-11-05），补 K49 恢复用例并有变异红控；真实 CTPMini SDK 合同外部"
        fa["裁定"] = "K49=A"
        fa["证据"] = "docs/迭代计划/迭代03/" + EV_REL + "/README.md"
        row["备注"] = row["备注"] + "\n" + line
counts = {}
for row in d["rows"]:
    counts[row["状态"]] = counts.get(row["状态"], 0) + 1
assert counts == {"已验收": 141, "本地验证通过": 62, "阻塞": 10}, counts
OLD = "已验收 141、本地验证通过 61、待验证 0、阻塞 11（卡15 用户指示忽略 10、待裁定 1）"
NEW = "已验收 141、本地验证通过 62、待验证 0、阻塞 10（均为卡15 用户指示忽略；T3-015 经 K49=A 转本地验证通过）"
for key in ("当前阶段", "全量回归"):
    if isinstance(d.get(key), str):
        d[key] = d[key].replace(OLD, NEW)
s = json.dumps(d["整体回归验收"], ensure_ascii=False)
s = s.replace(OLD, NEW).replace("本地验证通过 61", "本地验证通过 62").replace("阻塞 11", "阻塞 10")
d["整体回归验收"] = json.loads(s)
d["整体回归验收"]["裁定补充"] = ("2026-11-05 用户裁定 K49=A（T3-015 转本地验证通过）、K52=否（I03-NEW-119 关闭）；"
                              "K50、K51、K53 用户要求先讨论再定，保持原状。证据 " + EV_REL + "/README.md")
open(tp, "w").write(json.dumps(d, ensure_ascii=False, indent=2) + "\n")

# ---- 逐项验收.json
zp = DOC + "/实施跟踪/evidence/20261105-final-acceptance/逐项验收.json"
z = json.load(open(zp))
rows = z if isinstance(z, list) else z.get("rows", z.get("items", []))
hit = 0
for r in rows:
    if r.get("id") == "T3-015":
        for k in ("final_status", "结论", "状态"):
            if k in r:
                r[k] = "本地验证通过"
        r["decision"] = "K49=A（2026-11-05）"
        r["post_class"] = "B"
        r["matched_tests"] = 4
        r["note"] = "K49=A 用户裁定（2026-11-05），补 K49 恢复用例并有变异红控；真实 CTPMini SDK 合同外部"
        r.setdefault("tests", {})["CoverageTests_t_TraderCTPMini::" + TEST] = {"run": "pass", "asan": "pass", "tsan": "pass"}
        hit += 1
assert hit == 1, hit
open(zp, "w").write(json.dumps(z, ensure_ascii=False, indent=1) + "\n")

# ---- 新增问题.md
np_ = DOC + "/实施跟踪/新增问题.md"
s = open(np_, encoding="utf-8").read()
i = s.find("## I03-NEW-119")
assert i >= 0
j = s.find("\n- 状态：", i)
k_end = s.find("\n", j + 1)
s = s[:k_end] + ("\n- **2026-11-05 用户裁定 K52=否**：不要求 `CoverageTests_porter` 单进程整组运行，逐例独立进程为验收口径；"
                 "本条按裁定关闭（已验收口径不变）。I03-NEW-125（porter 测试链接集未隔离）保持登记。") + s[k_end:]
open(np_, "w", encoding="utf-8").write(s)

# ---- 决策积压清单.md
dp = DOC + "/实施跟踪/决策积压清单.md"
s = open(dp, encoding="utf-8").read()
assert "| K53 |" in s
s = s.rstrip("\n") + ("\n\n**用户裁定（2026-11-05 晚）：K49=A**（下一次合法登录自动就绪，现实现即合同；已补用例，T3-015 转本地验证通过）；"
                      "**K52=否**（逐例独立进程为验收口径，I03-NEW-119 关闭）。**K50、K51、K53 用户要求先详细讨论再定**，在此之前保持原状态"
                      "（I03-NEW-089 阻塞；BUG-R18-011 慢消费者子项未裁定；I03-NEW-121 附带子项未覆盖）。证据 `" + EV_REL + "/README.md`。\n")
open(dp, "w", encoding="utf-8").write(s)

# ---- 最终验收报告.md
rp = DOC + "/最终验收报告.md"
s = open(rp, encoding="utf-8").read()
s = s.replace("已验收 141、本地验证通过 61、阻塞 11、待验证 0。",
              "已验收 141、本地验证通过 62、阻塞 10、待验证 0（2026-11-05 晚 K49=A 裁定后 T3-015 由阻塞转本地验证通过）。")
s = s.replace("| T3-015 | GetTradingDay 失效登录后恢复策略待用户裁定（建议 A=现实现）；真实 CTPMini SDK 合同外部 |\n", "")
s = s.replace("## 8. 待用户裁定（已登记为决策积压清单 K49–K53）\n",
              "## 8. 待用户裁定（已登记为决策积压清单 K49–K53）\n\n"
              "2026-11-05 晚用户已裁定 K49=A、K52=否（见下文标注）；K50、K51、K53 待讨论。\n")
s = s.replace("- K49（T3-015）：", "- **已裁定 A**｜K49（T3-015）：")
s = s.replace("- K52（I03-NEW-119）：", "- **已裁定 否**｜K52（I03-NEW-119）：")
open(rp, "w", encoding="utf-8").write(s)

# ---- 迭代计划.md
pp = DOC + "/迭代计划.md"
s = open(pp, encoding="utf-8").read()
old = "213 项已验收 141、本地验证通过 61、阻塞 11（卡15 用户指示忽略 10、待裁定 1）、待验证 0。"
assert old in s
s = s.replace(old, "213 项已验收 141、本地验证通过 62、阻塞 10（均为卡15 用户指示忽略）、待验证 0（K49=A、K52=否 已裁定，K50/K51/K53 待讨论）。")
open(pp, "w", encoding="utf-8").write(s)
print("updated", counts, "binary cases", n_bin)
