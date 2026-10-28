#!/usr/bin/env python3
"""Generate compact per-lane API briefs so test writers skip module-wide exploration.

Usage: python3 docs/迭代计划/迭代01/tools/api_brief.py [--repo ROOT]
Writes docs/迭代计划/迭代01/manifests/api-brief-L<NN>.md, one per lane.
Extraction is purely structural (declarations + file:line); semantics stay with the writer.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LANES = {
    "L01": {"Share", "WTSUtils"},
    "L02": {"WTSTools", "WtShareHelper", "WtMsgQue"},
    "L03": {"Includes"},
    "L04": {"WtDataStorage", "WtDataStorageAD"},
    "L05": {"WtDtCore", "WtDtHelper", "WtDtServo", "WtDtPorter"},
    "L06": {"WtBtCore"},
    "L07": {"WtCore"},
    "L08": {"WtExeFact", "WtExecMon", "WtRiskMonFact", "WtCtaTwoMaFact",
            "WtCtaStraFact", "WtUftStraFact", "WtSelStraFact", "WtHftStraFact"},
    # N03 波次（第二轮）：L07=WtCore、L08=执行与策略工厂、L02=WtMsgQue+WtShareHelper 复用；
    # 这条补上此前没有任何简报的 UFT/延迟内核与指数工厂。
    "N03": {"WtUftCore", "WtIndexFactory", "WtLatencyHFT", "WtLatencyUFT"},
}


def transport_lanes(src_root: Path) -> dict[str, set[str]]:
    """收发族按「一个目录 = 一个 lane 简报」派生，从磁盘读出，不手敲名字。

    2026-10-27 实测更正：这一族并非「要厂商环境才能测」——厂商头在 src/API/ 里，
    顶层构建已在编译它们；缺的只是每目录一个测试目标（extern "C" createParser/
    createTrader 每目录重名，两目录同二进制必 duplicate symbol）。
    探针 CoverageTests_t_TraderMocker 已证：2 条用例就让 TraderMocker.cpp 进了 37 行覆盖。
    """
    cands = sorted(
        p.name for p in src_root.iterdir()
        if p.is_dir() and (p.name.startswith(("Parser", "Trader")) or p.name.endswith("Loader"))
    )
    lanes: dict[str, set[str]] = {}
    for i in range(0, len(cands), 4):
        group = cands[i:i + 4]
        lanes[f"T{i // 4 + 1:02d}"] = set(group)
    return lanes

# Vendored trees are instrumented, not tested; keep them out of the briefs.
SKIP_DIR_PARTS = {"yamlcpp", "rapidjson", "lmdb", "googletest", "fmt", "spdlog"}

CLASS_RE = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(class|struct)\s+([A-Za-z_]\w*)")
FUNC_RE = re.compile(
    r"^\s*(?:inline\s+|static\s+|virtual\s+|explicit\s+|constexpr\s+)*"
    r"[A-Za-z_][\w:<>*&\s,~\[\]]*?\b([~A-Za-z_]\w*)\s*\(([^;{]*)\)\s*"
    r"(?:const)?\s*(?:=\s*0)?\s*(?:override)?\s*(?:noexcept)?\s*(?:{|;)"
)
DECL_SKIP = ("if", "for", "while", "switch", "return", "catch", "sizeof", "delete")


def interesting(path: Path) -> bool:
    return not (set(path.parts) & SKIP_DIR_PARTS)


def signatures(src: str):
    out = []
    for lineno, raw in enumerate(src.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith(("//", "/*", "*", "#")):
            continue
        if "(" not in line or ")" not in line:
            continue
        head = line.split("(")[0].strip()
        if any(head.startswith(k) or head == k for k in DECL_SKIP):
            continue
        m = FUNC_RE.match(raw)
        if not m:
            continue
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return"):
            continue
        args = " ".join(m.group(2).split())
        ret = " ".join(m.group(0)[: m.start(1)].split()).rstrip()
        suffix = " ".join(raw[raw.rfind(")") + 1:].split()).strip(" {;")
        out.append((lineno, f"{ret}{name}({args}){' ' + suffix if suffix else ''}"))
    return out


def brief_file(path: Path, root: Path):
    src = path.read_text(encoding="utf-8", errors="ignore")
    lines = src.splitlines()
    rel = path.relative_to(root)
    classes = [f"{m.group(2)}:{i}" for i, l in enumerate(lines, 1)
               for m in [CLASS_RE.match(l)] if m]
    funcs = signatures(src)
    if not classes and not funcs:
        return []
    out = [f"### `{rel}` ({len(lines)} 行)"]
    if classes:
        out.append("- 类型: " + ", ".join(classes[:24]))
    for lineno, sig in funcs[:70]:
        out.append(f"- L{lineno}: `{sig}`")
    if len(funcs) > 70:
        out.append(f"- （另有 {len(funcs) - 70} 个声明，按需用 Read 定位）")
    out.append("")
    return out


def lanes_from_scope(scope_md: Path):
    """读 tools/wave_scope.py 生成的派工范围，取出每一路的文件清单。

    为什么单独做「按文件」的简报：整模块简报（L06/L07 那种）让 lane 先读 2~5 万字符
    才开始写用例；按本轮实测 missed 排名只给它要动的几个文件，第一份用例出现得更快。
    """
    lanes = []
    cur = None
    for line in scope_md.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^## 路 (\d+)", line)
        if m:
            cur = {"lane": "R%s" % m.group(1), "files": [], "targets": []}
            lanes.append(cur)
            continue
        if cur is None:
            continue
        t = re.match(r"^- 测试目标 `CoverageTests(_[A-Za-z0-9_]*)?([^`]*)`", line)
        if t:
            cur["targets"].append(((t.group(1) or "").lstrip("_")) or "(主目标 CoverageTests)")
            continue
        f = re.match(r"^  - `(src/[^`]+)`", line)
        if f:
            cur["files"].append(f.group(1))
    return lanes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".", type=Path)
    ap.add_argument("--out", default=None, type=Path)
    ap.add_argument("--scope", default=None, type=Path,
                    help="wave_scope.py 生成的派工范围 md；按每路的文件清单出简报（R1…R6）")
    args = ap.parse_args()
    root = args.repo.resolve()
    src_root = root / "src"
    if not src_root.is_dir():
        print(f"error: {src_root} is not a directory", file=sys.stderr)
        return 2
    out_dir = (args.out or (root / "docs" / "迭代计划" / "迭代01" / "manifests")).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.scope is not None:
        lanes = lanes_from_scope(args.scope)
        if not lanes:
            print(f"error: {args.scope} 里没有解析出「## 路 N」段落", file=sys.stderr)
            return 2
        missing_total = 0
        for ln in lanes:
            body = [f"# API 简报 {ln['lane']}", "",
                    "测试目标: %s。" % ", ".join(ln["targets"]),
                    "本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），",
                    "每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。",
                    "语义/分支以源码为准；**清单外的文件本轮不要动**。", ""]
            files = 0
            for rel in ln["files"]:
                path = root / rel
                if not path.is_file():
                    body.append(f"### `{rel}` —— 文件不存在（检查报表/路径）")
                    missing_total += 1
                    continue
                body.extend(brief_file(path, root))
                files += 1
            dest = out_dir / f"api-brief-{ln['lane']}.md"
            dest.write_text("\n".join(body) + "\n", encoding="utf-8")
            print("%s: %d 文件 %d 个目标 -> %s" % (ln["lane"], files, len(ln["targets"]),
                                                  dest.relative_to(root)))
        if missing_total:
            print("清单里不存在的文件数 = %d" % missing_total, file=sys.stderr)
        return 0

    totals = {}
    lanes = dict(LANES)
    lanes.update(transport_lanes(src_root))
    for lane, mods in lanes.items():
        body = [f"# API 简报 {lane}", "",
                f"覆盖模块: {', '.join(sorted(mods))}。",
                "每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，"
                "不要整模块通读。语义/分支以源码为准，本表只给入口。", ""]
        lines = 0
        files = 0
        for mod in sorted(mods):
            d = src_root / mod
            if not d.is_dir():
                body.append(f"## `{mod}` —— 目录不存在（检查拼写）")
                continue
            body.append(f"## `{mod}`")
            body.append("")
            for path in sorted(d.glob("*")):
                if not path.is_file() or path.suffix not in {".h", ".hpp", ".cpp", ".cc"}:
                    continue
                if not interesting(path):
                    continue
                chunk = brief_file(path, root)
                if not chunk:
                    continue
                files += 1
                lines += len(path.read_text(encoding="utf-8", errors="ignore").splitlines())
                body.extend(chunk)
        dest = out_dir / f"api-brief-{lane}.md"
        dest.write_text("\n".join(body) + "\n", encoding="utf-8")
        totals[lane] = (files, lines, dest)

    for lane, (files, lines, dest) in totals.items():
        print(f"{lane}: {files:3d} 文件 {lines:6d} 行 -> {dest.relative_to(root)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
