#!/usr/bin/env python3
"""Aggregate per-lane defect logs into one review-pending table.

Reads docs/迭代计划/迭代01/bugs/<lane>.md, emits docs/迭代计划/迭代01/bugs/汇总-待审核.md.
Every field is parsed from the lane files — nothing here is typed by hand, so a lane that
forgets a field shows up as an explicit <缺失> rather than a plausible-looking table.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

# 字段别名：lane 之间模板漂移（有人用「位置/现象」，有人把级别与状态写进标题尾巴）。
# 解析器必须认全，否则会把真实存在的条目报成 <缺失>，评审者看到的就是假空洞。
FIELDS = {
    "状态": "status",
    "严重级别": "severity",
    "模块/文件": "where",
    "位置": "where",
    "相关测试": "test",
    "关联用例": "test",
    "复现用例": "test",
    "实际行为": "actual",
    "现象": "actual",
    "输入/前置条件": "input",
    "前置条件": "input",
    "触发": "input",
    "期望行为": "expect",
    "复现步骤": "repro",
    "影响范围": "impact",
    "影响": "impact",
    "备注": "note",
    "分级": "severity",
    "复现": "repro",
    "用例侧回避": "note",
    "用例侧规避": "note",
    "建议": "fix",
    "留给主控": "note",
}

# These fields are atomic. An indented explanatory bullet after them is usually
# a child finding or note, not part of the severity/status value. In particular,
# joining that text can turn an explicit terminal status into a nonterminal one.
SCALAR_FIELDS = {"status", "severity"}

# 条目标题必须带可排序的编号。历史上只认 `BUG-`，结果 lane 用 `Q06-001` 这类
# 自己的前缀时，整份缺陷清单被静默丢掉（实测 q06_porters.md 7 条 0 进汇总），
# 汇总表面看是「这一路没发现 bug」。所以现在按 `<标识>-<数字>` 通用匹配，
# 并把仍然不合模板的 `## ` 标题显式计数打出来，交人工确认，不再静默。
ENTRY_ID = (
    r"(?:BUG-[A-Za-z][A-Za-z0-9_]*-\d+"
    r"|[A-Za-z][A-Za-z0-9_]*-BUG-\d+"
    r"|[A-Za-z][A-Za-z0-9_]*-\d+)"
    r"[A-Za-z0-9_.-]*"
)
HEAD_RE = re.compile(r"^(?:#\s+|##\s+)(" + ENTRY_ID + r")(.*)$")
ANY_HEAD_RE = re.compile(r"^##\s+(.*)$")
NUMERIC_HEAD_RE = re.compile(r"^#{2,6}\s+(\d+)[.)、]?\s+(.+)$")
SEV_IN_TITLE = re.compile(r"\b(Blocker|Major|Minor)\b")
FIELD_LINE_RE = re.compile(r"^-\s*\*{0,2}([^:：*]{1,14})\*{0,2}\s*[:：]\s*(.*)$")


CONT_RE = re.compile(r"^\s{2,}-\s+(.*)$")

# 第二种条目语法（T1/T2 就是这样写的）：顶层 bullet 带粗体编号 + 标题，
# 字段挂在缩进 bullet 上。只认 `## BUG-xx` 会让整条 lane 的缺陷静默消失
# （实测 rows=234 少了 T1 的 12 条 + T2 的 19 条），所以两种语法都要解析，
# 并在文件层面比对「条目数 vs 解析出的行数」。
BULLET_HEAD_RE = re.compile(r"^-\s*\*\*\s*([A-Za-z][A-Za-z0-9_-]*-\d+[A-Za-z0-9_.-]*)\s+(.*?)\s*\*\*\s*$")
TITLE_HEAD_RE = re.compile(r"^-\s*标题\s*[:：]\s*(.+?)\s*$")
NON_FINDING_REVIEW_HEAD_RE = re.compile(
    r"^#{2,6}\s+复核记录\s*[（(][^）)]*非新增缺陷[^）)]*[）)]\s*$"
)


# 允许「待审核」后面带说明文字（R1 的 `## 2.` 条目写的是
# `- 状态: 待审核（R1 用例只测「首次读」…）`，旧式带 $ 锚点会把整条静默丢掉，
# 实测行数 342 里就少了这一条）。只要「状态: 待审核」成行即可算条目。
ENTRY_MARK = re.compile(r"^\s*(?:[-*]\s*)?状态\s*[:：]\s*待审核")
TITLE_STATUS_RE = re.compile(r"[（(]\s*状态\s*[:：]\s*([^）)]*)[）)]\s*$")
TITLE_BARE_TERMINAL_STATUS_RE = re.compile(r"[（(]\s*(已撤回|已合并)\s*[）)]\s*$")
TERMINAL_STATUSES = {"已撤回", "已合并"}
DELIM = re.compile(
    r"^(?:"
    r"#{2,6}\s"                      # 任意级标题（## BUG-xx / ## 1. 标题 / ### A. …）
    r"|#\s+" + ENTRY_ID +            # 单井号只把带缺陷编号的标题作为条目边界
    r"|-{3,}$"                       # --- 分隔（T5 用法）
    r"|-\s*\*\*\s*[A-Za-z][A-Za-z0-9_-]*-\d+"   # - **R5-001 …** （T1/T2 用法）
    r"|-\s*(?:标题|ID)\s*[:：]"            # - 标题: … / - ID: … （R2/T5 用法）
    r")")


def status_value(value):
    """Return the explicit status before an optional explanatory note."""
    return re.split(r"[（(，,]", str(value).strip(), maxsplit=1)[0].strip()


def title_status(head):
    """Read an explicit status annotation at the end of a numbered heading."""
    title = re.sub(r"^#+\s*", "", head.strip())
    match = TITLE_STATUS_RE.search(title)
    if match:
        return status_value(match.group(1))
    match = TITLE_BARE_TERMINAL_STATUS_RE.search(title)
    return match.group(1) if match else None


def parse_file(path: Path):
    """按「块」解析，而不是按某一种标题语法解析。

    历史上栽过两次：只认 `## BUG-` 时，T1/T2 的 bullet 语法整路静默消失（31 条）；
    补了 bullet 语法后，R1（`## 1. 数字标题`）、R2（`- 标题:`）、T5（`---` 分隔的 `- ID:`）
    又整路丢掉 0 行。因此判定支持已知的几种条目写法：
    **一个条目块 = 含有一行独立「状态: 待审核」字段、编号标题、字段签名完整的数字标题，或 `- 标题:` 标记的最小段落**，
    块边界取「标题 / --- / 粗体编号 bullet / `- 标题:` / `- ID:`」这几种实际出现过的写法。
    解析器只对已识别的编号标题和字段签名完整的数字标题负责；这不是对任意非标准标题的穷举检查。
    """
    body = path.read_text(encoding="utf-8").splitlines()
    blocks, cur = [], []
    for raw in body:
        if DELIM.match(raw) and cur:
            blocks.append(cur)
            cur = [raw]
        else:
            cur.append(raw)
    if cur:
        blocks.append(cur)

    rows = []
    skipped = []
    intentionally_excluded_ids = set()
    seq = 0
    for blk in blocks:
        # Some lane files append a review matrix under an explicitly labelled
        # section. Its trailing `状态: 待审核` describes the section, not a new
        # defect candidate. Keep this exclusion narrow: ordinary unnumbered
        # entries and review headings without this explicit label still parse.
        if NON_FINDING_REVIEW_HEAD_RE.match(blk[0].strip()):
            continue
        has_mark = any(ENTRY_MARK.match(l) for l in blk)
        head_id = HEAD_RE.match(blk[0]) or BULLET_HEAD_RE.match(blk[0])
        title_head = TITLE_HEAD_RE.match(blk[0])
        numeric_head = NUMERIC_HEAD_RE.match(blk[0].strip())
        numeric_id = None
        numeric_signature = False
        numeric_has_finding_fields = False
        if numeric_head:
            numeric_id = "%s#%s" % (path.stem, numeric_head.group(1))
            numeric_fields = set()
            for raw in blk[1:]:
                fm = FIELD_LINE_RE.match(raw.strip())
                if fm:
                    key = FIELDS.get(fm.group(1).strip())
                    if key:
                        numeric_fields.add(key)
            descriptive_fields = {"actual", "impact", "fix", "input", "expect", "repro", "test"}
            numeric_signature = "where" in numeric_fields and bool(numeric_fields & descriptive_fields)
            numeric_has_finding_fields = bool(numeric_fields & (descriptive_fields | {"where"}))
        # 条目判据取并集：独立「状态: 待审核」字段行、标题自带编号，或显式 `- 标题:`
        # （p03/p08 那一族把状态写在标题尾巴上 ——「（状态：待审核）」——
        # 只认字段行会整族丢掉 21 条，实测行数 302→300 就是这么来的；R2 的无编号标题也不能依赖固定状态值）。
        if not (has_mark or head_id or title_head or numeric_signature):
            if numeric_head and numeric_has_finding_fields:
                skipped.append(
                    "%s: 数字标题候选 %s（`%s`）含疑似缺陷字段但未满足自动收录签名（需位置/模块字段及至少一个行为、影响、复现或建议字段）"
                    % (path.stem, numeric_id, numeric_head.group(0).strip()))
            continue
        seq += 1
        head = blk[0].strip()
        idm = re.search(r"([A-Za-z][A-Za-z0-9_]*-[A-Za-z0-9_.-]*\d+[A-Za-z0-9_.-]*)", head)
        title = re.sub(r"^#+\s*", "", head)
        title = re.sub(r"^-\s*\*{0,2}\s*", "", title)
        title = re.sub(r"^(?:标题|ID|状态)\s*[:：]\s*", "", title)
        title = title.replace("**", "").strip()
        heading_status = title_status(head)
        row = {"id": numeric_id or (idm.group(1) if idm else "%s#%d" % (path.stem, seq)),
               "lane": path.stem, "line": None, "title": title,
               "status": heading_status or "待审核"}
        last_key = None
        for raw in blk[1:]:
            fm = FIELD_LINE_RE.match(raw.strip())
            if fm:
                key = FIELDS.get(fm.group(1).strip())
                if key:
                    row[key] = fm.group(2).strip()
                    last_key = key
                    continue
            if last_key in SCALAR_FIELDS:
                continue
            if last_key and row.get(last_key) == "":
                cm = CONT_RE.match(raw)
                if cm:
                    row[last_key] = cm.group(1).strip()
                elif raw.strip():
                    last_key = None
            elif last_key and row.get(last_key):
                cm = CONT_RE.match(raw)
                if cm:
                    row[last_key] += " / " + cm.group(1).strip()
        sev = row.get("severity") or ""
        if not sev:
            t = SEV_IN_TITLE.search(title)
            if t:
                row["severity"] = t.group(1)
                row["_severity_from_title"] = True
        else:
            row["severity"] = norm_sev(sev)

        if status_value(row.get("status", "")) in TERMINAL_STATUSES:
            # These are explicit review outcomes, not pending defects. Track their
            # IDs so the legacy unparsed-title diagnostic does not flag them.
            intentionally_excluded_ids.add(row["id"])
            continue

        rows.append(row)

    for l in body:
        hm = HEAD_RE.match(l)
        if (hm and hm.group(1) not in intentionally_excluded_ids
                and not any(r["id"] == hm.group(1) for r in rows)):
            skipped.append("%s: 标题 `%s` 未被解析成条目" % (path.stem, hm.group(1)))
    return rows, skipped


def clip(text, n=110):
    if not text:
        return "<缺失>"
    text = " ".join(str(text).split())
    return text if len(text) <= n else text[: n - 1] + "…"


def norm_sev(value):
    """`Blocker（涨跌停单边盘口下崩溃）` -> `Blocker`; the note stays in the lane file.

    没有级别的条目一律显示 `未标注`——不替 lane 猜级别，也不许它隐身。
    """
    if not value:
        return "未标注"
    return str(value).split("（")[0].split("(")[0].strip()


def main() -> int:
    # 默认按脚本自身位置倒推仓库根，不依赖调用时的 cwd
    # （实测从 docs/迭代计划/迭代01 里调用会把路径拼成 …/迭代01/docs/迭代计划/迭代01/bugs 而报 missing）。
    _here = Path(__file__).resolve()
    # tools/迭代01/迭代计划/docs/<repo> ⇒ parents[4] 才是仓库根（先按目录层数解出来，再校验 bugs 是否存在，
    # 免得哪天文档挪一层就又拼出 docs/docs/…）
    default_root = next((p for p in _here.parents[1:]
                         if (p / "docs" / "迭代计划" / "迭代01" / "bugs").is_dir()), _here.parents[4])
    root = Path(sys.argv[1] if len(sys.argv) > 1 else default_root).resolve()
    bugs_dir = root / "docs" / "迭代计划" / "迭代01" / "bugs"
    if not bugs_dir.is_dir():
        print(f"error: {bugs_dir} missing", file=sys.stderr)
        return 2

    lanes = sorted(p for p in bugs_dir.glob("*.md") if p.stem != "README"
                    and not p.stem.startswith("汇总"))
    if not lanes:
        print("error: no lane defect logs found", file=sys.stderr)
        return 2

    rows = []
    unparsed = []
    for p in lanes:
        one, skip = parse_file(p)
        rows.extend(one)
        unparsed.extend(skip)

    sev = Counter(norm_sev(r.get("severity")) for r in rows)
    st = Counter(r.get("status", "<缺失>") for r in rows)
    per_lane = Counter(r["lane"] for r in rows)

    order = {"Blocker": 0, "Major": 1, "Minor": 2}
    rows.sort(key=lambda r: (order.get(norm_sev(r.get("severity")), 9),
                             r["lane"], r["id"]))

    out = []
    out.append("# 问题汇总 — 尚未撤回/合并")
    out.append("")
    out.append(f"来源：{len(lanes)} 份 lane 问题记录，共 **{len(rows)}** 条尚未撤回/合并的候选或确认问题；"
               "其中有些已复核、有些仍待验证，该计数不等于已确认产品缺陷总数。")
    out.append("")
    out.append("## 计数")
    out.append("")
    out.append("| 维度 | 分布 |")
    out.append("|---|---|")
    out.append("| 严重级别 | " + " / ".join(f"{k} {v}" for k, v in sorted(
        sev.items(), key=lambda kv: order.get(kv[0], 9))) + " |")
    out.append("| 状态 | " + " / ".join(f"{k} {v}" for k, v in st.items()) + " |")
    out.append("| 按 lane | " + " / ".join(f"{k} {v}" for k, v in sorted(per_lane.items())) + " |")
    out.append("")
    if any(v == 0 or k == "<缺失>" for k, v in st.items()):
        out.append("> 注意：存在状态缺失或为 0 的条目，说明某个 lane 没按模板填写，需回查。")
        out.append("")

    out.append("## 明细")
    out.append("")
    out.append("| ID | lane | 级别 | 摘要 | 位置 | 实际行为 |")
    out.append("|---|---|---|---|---|---|")
    for r in rows:
        out.append("| {id} | {lane} | {sev} | {title} | {where} | {actual} |".format(
            id=r["id"], lane=r["lane"].replace("_", "\\_"),
            sev=norm_sev(r.get("severity")), title=clip(r.get("title") or r.get("test")),
            where=clip(r.get("where"), 70), actual=clip(r.get("actual"))))
    out.append("")
    out.append("## 评审须知")
    out.append("")
    out.append("- 逐条完整上下文（输入/期望/复现/修复方向）在对应 `bugs/<lane>.md` 里，本表只是索引。")
    out.append("- 标注「已绕过」的条目意味着测试用例外壳做了规避，**产品缺陷仍然存在**。")
    out.append("- 部分用例以 `DISABLED_` 前缀留在测试树里（会崩溃/死循环的路径），"
               "它们计入用例总数但不产生覆盖，修缺陷后需重新启用。")
    out.append("")
    out.append(f"复现：`/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python docs/迭代计划/迭代01/tools/aggregate_bugs.py .`")

    dest = bugs_dir / "汇总-待审核.md"
    dest.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"lanes={len(lanes)} rows={len(rows)} -> {dest.relative_to(root)}")
    print(f"已知编号标题遗漏或字段签名不完整的告警={len(unparsed)}（仅覆盖可识别 BUG-ID 标题及疑似缺陷数字标题，不是对所有非标准标题的穷举扫描）")
    for u in unparsed[:20]:
        print("   !", u)
    print("severity:", dict(sev))
    print("status:", dict(st))
    print("per-lane:", dict(sorted(per_lane.items())))
    missing = [r["id"] for r in rows if not r.get("severity") or not r.get("where")]
    print("字段缺失条目:", missing if missing else "无")
    from_title = sum(1 for r in rows if r.get("_severity_from_title"))
    no_title = sum(1 for r in rows if not r.get("title"))
    print(f"模板漂移面: 级别写在标题里的条目={from_title}/{len(rows)}，"
          f"无摘要标题的条目={no_title}/{len(rows)}（模板本身需要统一，别靠解析器长期兜底）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
