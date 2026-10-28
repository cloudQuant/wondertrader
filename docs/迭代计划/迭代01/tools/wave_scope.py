#!/usr/bin/env python3
"""从覆盖率报表 + 测试构建表派生「下一波」分派范围。

为什么不做人工挑文件：手敲会把并不存在（或并不属于任何测试目标）的文件写进派工文本，
前几轮已经为此付过代价。这里两边都从磁盘读：
- 待补目标 = 报表 UNCOVERED 段里 missed 行数最大的文件；
- 归属测试目标 = src/TestUnits/coverage/CMakeLists.txt 的拆分表
  （`<后缀>|<生产目录>|<用例来源>`）+ 收发族按磁盘派生的 t_<模块>；
- 分桶 = 按「每路目标 missed 行数」贪心均衡，并保证同一路的文件
  都属于同一个测试目标（否则 lane 会把用例写进链不到一起的二进制）。

用法: python3 tools/wave_scope.py [--report <报表txt>] [--lanes 6] [--top 40]
"""
import argparse
import collections
import os
import re
import sys
from pathlib import Path


UNCOVERED_RE = re.compile(r"^\s*(\d+)/\s*(\d+)\s+([\d.]+)%\s+(\S+)$")
SPEC_RE = re.compile(r'"([a-z0-9_]+)\|([^"|]+)\|([^"]*)"')


def parse_report(path: Path):
    """返回 [(file, missed, total, pct)]，只取 UNCOVERED 段。"""
    rows = []
    started = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("UNCOVERED"):
            started = True
            continue
        if not started:
            continue
        m = UNCOVERED_RE.match(line)
        if m:
            missed, total, pct, f = int(m.group(1)), int(m.group(2)), float(m.group(3)), m.group(4)
            rows.append((f, missed, total, pct))
    return rows


def target_cases(cov_cmake: Path):
    """{测试目标后缀: 已在拆分表里登记的用例目录列表}。

    这条信息必须跟着派工范围一起给：主目标用 GLOB_RECURSE 收所有没被分派走的用例，
    所以 lane 自造一个新目录（比如 r2_btcore）会掉进**主二进制**，而主二进制不编
    WtBtCore 的生产 TU —— 用例立刻变成链不通的 undefined symbol。反过来 WtDataStorage
    的 TU 恰恰只在主二进制里编，所以它的新目录才是对的。
    判定只靠读表，不靠我记规则（记规则就会像前几轮一样写错方向）。
    """
    cases = {}
    for suffix, _dirs, src in SPEC_RE.findall(cov_cmake.read_text(encoding="utf-8")):
        got = []
        for tok in src.split("+"):
            for part in tok.split(","):
                part = part.strip()
                if part.startswith("dir:"):
                    got.append(part[4:])
                elif "/" in part:
                    got.append(part.rsplit("/", 1)[0])
        cases.setdefault(suffix, [])
        for g in got:
            if g not in cases[suffix]:
                cases[suffix].append(g)
    return cases


def target_dirs(cov_cmake: Path, src_root: Path, build: Path):
    """{测试目标后缀: 生产目录集合}。

    归属以**构建系统自己的产物摆放**为准，而不是我读表猜：build.ninja 里每条
    `CMakeFiles/CoverageTests_xxx.dir/__/__/<模块>/<文件>.cpp.o` 规则就是「哪个二进制编了哪个模块」，
    主目标编的 WtDataStorage/WtDtPorter 这类只在 CMake 尾部 file(GLOB) 出现的目录也能一并拿到
    （先前只读拆分表时会把这些误报成「没有测试目标」，等于把已存在的射程当成缺口）。
    表里的目录集合仍作为兜底（磁盘上还没编出来的目标）。
    """
    mapping = {}
    text = cov_cmake.read_text(encoding="utf-8")
    for suffix, dirs, _src in SPEC_RE.findall(text):
        mapping[suffix] = [d.strip() for d in dirs.split(",")]

    ninja = build / "build.ninja"
    from_build = 0
    if ninja.is_file():
        rule = re.compile(r"CMakeFiles/(CoverageTests[A-Za-z0-9_]*)\.dir/"
                          r"(?:\.\./)*((?:__/)+)?([A-Za-z0-9_]+)/[^ :]+\.cpp\.o")
        seen = set()
        for line in ninja.read_text(encoding="utf-8", errors="replace").splitlines():
            if not line.startswith("build "):
                continue
            for m in rule.finditer(line):
                suffix, module = m.group(1), m.group(3)
                if (suffix, module) in seen:
                    continue
                seen.add((suffix, module))
                key = suffix[len("CoverageTests_"):] if suffix != "CoverageTests" else ""
                mapping.setdefault(key, [])
                if module not in mapping[key]:
                    mapping[key].append(module)
                    from_build += 1
        if from_build:
            print("归属来自 build.ninja 的模块数 = %d" % from_build)

    for d in sorted(os.listdir(src_root)):
        p = src_root / d
        if not p.is_dir():
            continue
        if d.startswith("Parser") or d.startswith("Trader") or d.endswith("Loader"):
            mapping.setdefault("t_" + d, [d])
    for m in re.finditer(r'WT_MAIN_MODULES\s+([^)]*)\)', text, re.S):
        for tok in re.findall(r"\b([A-Za-z][A-Za-z0-9]+)\b", m.group(1)):
            if (src_root / tok).is_dir():
                mapping.setdefault("m_" + tok, [tok])
    return mapping


def module_of(path_str: str):
    parts = Path(path_str).parts
    return parts[1] if len(parts) > 2 and parts[0] == "src" else parts[-1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".", type=Path)
    ap.add_argument("--report", default=None, type=Path)
    ap.add_argument("--lanes", type=int, default=6)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--build", default=None, type=Path)
    ap.add_argument("--exclude-target", action="append", default=[],
                    help="按测试目标前缀排除（如 t_=收发族），用于跳过在跑的那一波")
    args = ap.parse_args()
    root = args.repo.resolve()
    build = args.build or (root / "build-cov")
    rep_dir = root / "docs" / "迭代计划" / "迭代01" / "reports"
    report = args.report
    if report is None:
        cands = sorted(rep_dir.glob("coverage-*.txt"))
        if not cands:
            print("error: 没有覆盖率报表，先跑 tools/coverage.sh", file=sys.stderr)
            return 2
        report = cands[-1]
    rows = parse_report(report)
    if not rows:
        print(f"error: {report} 里没解析出 UNCOVERED 行（检查报表格式）", file=sys.stderr)
        return 2
    cov_cmake = root / "src" / "TestUnits" / "coverage" / "CMakeLists.txt"
    mapping = target_dirs(cov_cmake, root / "src", build)
    cases = target_cases(cov_cmake)
    main_cases = sorted(p.name for p in (root / "src" / "TestUnits" / "coverage").iterdir()
                        if p.is_dir() and not any(p.name in v for v in cases.values()))
    # 同一模块可能被主目标和专用目标同时编（WtDtCore 就是）。派工时专用目标才是
    # lane 该写进去的地方，所以主目标（key=''）排最后，不让它抢走归属。
    mod2target = {}
    for suffix, dirs in sorted(mapping.items(), key=lambda kv: (kv[0] == "",)):
        for d in dirs:
            mod2target.setdefault(d, suffix)

    rows.sort(key=lambda r: -r[1])
    picked, unowned, excluded = [], [], 0
    for f, missed, total, pct in rows:
        mod = module_of(f)
        tgt = mod2target.get(mod)
        if tgt is None:
            # Includes/WTSTools/WTSUtils/Share 这类：产品 TU 不在任何测试二进制里编，
            # 符号来自被链接的静态库，行覆盖照样被 llvm-cov 计入。
            # 它们的用例写主目标目录（l02_wtstools_mq / l03_includes / ws01_share_core 等），
            # 所以归属回落到主目标，而不是报成「没有测试目标」——那会把已有射程当成缺口。
            tgt = ""
            unowned.append((f, missed, mod))
        if any(tgt.startswith(x) for x in args.exclude_target):
            excluded += 1
            continue
        picked.append((f, missed, total, pct, mod, tgt))
        if len(picked) >= args.top:
            break

    # 贪心均衡，但同一目标的文件尽量落在同一路（一个二进制一个 lane 的写作面）
    by_target = collections.OrderedDict()
    for f, missed, total, pct, mod, tgt in picked:
        by_target.setdefault(tgt, [0, []])
        by_target[tgt][0] += missed
        by_target[tgt][1].append((f, missed, total, pct, mod))

    lanes = [[] for _ in range(args.lanes)]
    load = [0] * args.lanes
    for tgt, (msum, items) in sorted(by_target.items(), key=lambda kv: -kv[1][0]):
        i = load.index(min(load))
        lanes[i].append((tgt, msum, items))
        load[i] += msum

    out = report.parent / ("分派范围-%s.md" % report.stem.replace("coverage-", ""))
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("# 下一波分派范围（由报表 `%s` 派生，非人工挑选）\n\n" % report.name)
        fh.write("- 每路目标 missed 行数：%s\n" % ", ".join(str(x) for x in load))
        fh.write("- 报表里前 %d 个待补文件按 missed 行数排序；同一路只放同一个测试目标的文件。\n" % args.top)
        if unowned:
            fh.write("- **没有测试目标的模块（需要主控先建目标，不能派给 lane）**：%s\n"
                     % ", ".join("%s(%d)" % (m, mm) for _, mm, m in unowned))
        for i, lane in enumerate(lanes, 1):
            fh.write("\n## 路 %d（预计 %d 行未覆盖）\n" % (i, load[i - 1]))
            for tgt, msum, items in lane:
                if tgt:
                    dirs = cases.get(tgt) or []
                    fh.write("- 测试目标 `CoverageTests_%s`；**用例只能写进这些已登记目录**：%s\n"
                             % (tgt, ", ".join("src/TestUnits/coverage/%s/" % d for d in dirs)
                                or "（表里没有登记目录 —— 需要主控先加 dir: 条目，lane 不要自造新目录）"))
                else:
                    fh.write("- 测试目标 `CoverageTests`（主目标，GLOB_RECURSE 自动收新目录）；"
                             "新目录可用，例如 %s\n"
                             % ", ".join("src/TestUnits/coverage/%s/" % d for d in main_cases[:3] or ["rN_<主题>"]))
                for f, missed, total, pct, mod in items:
                    fh.write("  - `%s` missed=%d/%d（现覆盖 %.2f%%）\n" % (f, missed, total, pct))
    if excluded:
        print("按 --exclude-target 排除的文件数 = %d（前缀 %s）" % (excluded, args.exclude_target))
    print("lanes=%d per-lane missed=%s -> %s" % (args.lanes, load, out))
    if unowned:
        print("无归属模块 = %s" % ", ".join(sorted({m for _, _, m in unowned})))
    return 0


if __name__ == "__main__":
    sys.exit(main())
