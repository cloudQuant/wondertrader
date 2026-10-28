#!/usr/bin/env python3
"""Aggregate first-party line coverage from an LLVM coverage build.

Usage: coverage_report.py <build-dir> <profdata> [--gate]
"""
import collections
import os
import re
import subprocess
import sys

EXCLUDED = [
    "/src/API/",
    "/src/FasterLibs/",
    "/src/WTSUtils/lmdb/",
    "/src/WTSUtils/zstdlib/",
    "/src/WTSUtils/yamlcpp/",
    "/src/WTSUtils/StackTracer/",
    "/src/TestUnits/",
    "/src/Share/threadpool/",
    "/src/Share/cppcli.hpp",
]
EXCLUDED_PREFIX = [
    "/src/TestPorter/",
    "/src/TestBtPorter/",
    "/src/TestDtPorter/",
    "/src/TestExecPorter/",
    "/src/TestParser/",
    "/src/TestTrader/",
]


def repo_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


def find_objects(build):
    """收集参与覆盖率统计的对象文件。

    /TestUnits/ 下混着两类东西，必须区分：
      * 测试代码自己的对象（gtest、各 lane 的 *_test.cpp）—— 不该计入第一方产品覆盖率；
      * 被覆盖率目标顺带编译的**生产** TU，路径形如
        `TestUnits/coverage/CMakeFiles/CoverageTests_selfthft.dir/__/__/WtSelStraFact/WtSelStraFact.cpp.o`。
        这类对象在顶层没有对应目标（WtSelStraFact / WtHftStraFact 根本不在
        ADD_SUBDIRECTORY 里），如果一律按 "/TestUnits/" 排除，它们的覆盖行数就会凭空消失。
        实测就是这样漏掉了这两个模块（报告里 0 行）。
    """
    objs = []
    for base, _, files in os.walk(build):
        for name in files:
            if not name.endswith(".o"):
                continue
            path = os.path.join(base, name)
            rel = path.replace(build, "")
            production_in_cov_target = ("/TestUnits/coverage/CMakeFiles/" in rel
                                         and "/__/__/" in rel)
            if not production_in_cov_target:
                if any(x in rel for x in ("/TestUnits/", "/gtest/", "/lmdb/",
                                          "/zstdlib/", "/yamlcpp/", "/API/")):
                    continue
            objs.append(path)
    return sorted(objs)


def first_party(rel):
    if not rel.startswith("src/"):
        return False
    if any(x in "/" + rel for x in EXCLUDED):
        return False
    if any(rel.startswith(x[1:]) for x in EXCLUDED_PREFIX):
        return False
    return True


def parse_report(lines):
    rows = []
    for line in lines:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) < 10:
            continue
        path = parts[0]
        try:
            lines_total = int(parts[7])
            lines_missed = int(parts[8])
        except ValueError:
            continue
        rows.append((path, lines_total, lines_missed))
    return rows


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    build, profdata = sys.argv[1], sys.argv[2]
    gate = "--gate" in sys.argv
    objs = find_objects(build)
    if not objs:
        print("no instrumented first-party objects found", file=sys.stderr)
        return 2

    cmd = ["xcrun", "llvm-cov", "report", "-instr-profile", profdata,
           "-object", objs[0]]
    for obj in objs[1:]:
        cmd += ["-object", obj]
    out = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        return out.returncode

    root = repo_root()
    modules = collections.defaultdict(lambda: [0, 0, 0])
    files = []
    for path, total, missed in parse_report(out.stdout.splitlines()):
        ap = path if path.startswith("/") else "/" + path
        if not ap.startswith(root + "/"):
            continue
        rel = ap[len(root) + 1:]
        if not first_party(rel):
            continue
        parts = rel.split("/")
        mod = "/".join(parts[:2])
        modules[mod][0] += total
        modules[mod][1] += missed
        modules[mod][2] += 1
        files.append((rel, total, missed))

    def cov(l, m):
        return (l - m) / l * 100.0 if l else 0.0

    total_l = sum(v[0] for v in modules.values())
    total_m = sum(v[1] for v in modules.values())
    print(f"\nFIRST-PARTY PRODUCT: lines={total_l} missed={total_m} coverage={cov(total_l, total_m):.2f}% files={sum(v[2] for v in modules.values())}")
    print("\nMODULE COVERAGE")
    for mod, (l, m, f) in sorted(modules.items(), key=lambda x: cov(x[1][0], x[1][1])):
        print(f"{mod:34s} files={f:4d} lines={l:6d} missed={m:6d} cover={cov(l, m):6.2f}%")

    uncovered = [(rel, l, m) for rel, l, m in files if m > 0]
    # 旧版这里写死 [:30]：表头报 files=276 lines=47569，正文却只列 30 行，
    # 于是按报表派工的 wave_scope.py 只看得到前 30 个文件，第二波之后就没有可选目标了
    # （实测「剩余可选=0」就是这么来的）。改成默认全列，可用第三个参数收窄。
    print(f"\nUNCOVERED (unexcluded) files={len(uncovered)} lines={sum(m for _, _, m in uncovered)}")
    topn = int(sys.argv[3]) if len(sys.argv) > 3 else len(uncovered)
    for rel, l, m in sorted(uncovered, key=lambda x: -x[2])[:topn]:
        print(f"  {m:5d}/{l:5d} {cov(l, m):6.2f}% {rel}")

    if gate and total_m > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
