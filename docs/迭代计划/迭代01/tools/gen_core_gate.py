#!/usr/bin/env python3
"""为「核心/插件族」测试目标生成 lane 用的 -fsyntax-only 编译门禁。

gen_gate_flags.py 只处理收发族（t_<模块>），因为那里有生产对象可取 flags；
WtCore/WtBtCore/WtDtCore/WtDtServo/WtPorter 这些目标的生产 TU 是被测试目标直接编译的，
所以门禁 flags 要从**测试目标自己的某条既有 .cpp.o 编译命令**里取（同一目标内 flags 一致）。

用法: python3 tools/gen_core_gate.py CoverageTests_wtcore CoverageTests_btcore ...
产物: tools/gate_flags/<target>.sh ，用法 `sh <target>.sh <你的.cpp>`

为什么不让 lane 自己拼 flags：本迭代已两次把可编译性判错（少 -std=gnu++17、多 -IAPI），
flags 一律从构建系统本身取。取不到规则时报缺，不静默写一个残缺脚本。
"""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
BUILD = ROOT / "build-cov"
OUT_DIR = Path(__file__).resolve().parent / "gate_flags"


def find_rule(target):
    """从 build.ninja 里取该目标第一条 .cpp.o 编译边。

    实测边格式是 `build <对象路径>: CXX_COMPILER__<目标>_..._Debug <源路径>`，
    所以**行尾是源文件而不是 .o**（我第一版按 endswith(".cpp.o") 找边，六路全部 MISS，
    读起来像「目标没建起来」，其实是取边的谓词写错了）。改成按 `:` 前的输出段判定。
    """
    needle = "TestUnits/coverage/CMakeFiles/%s.dir/" % target
    path = BUILD / "build.ninja"
    if not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("build "):
            continue
        outs = line[len("build "):].split(":")[0].split()
        for o in outs:
            if o.endswith(".cpp.o") and needle in o:
                return o
    return None


def command_for(obj):
    r = subprocess.run(["ninja", "-C", str(BUILD), "-t", "commands", obj],
                       capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout.strip():
        return None
    toks = shlex.split(r.stdout.strip().splitlines()[-1])
    out, i = [], 0
    src = None
    while i < len(toks):
        t = toks[i]
        if t in ("-o", "-MT", "-MF"):
            i += 2
            continue
        if t in ("-c", "-MD", "-MMD"):
            if t == "-c":
                src = toks[i + 1] if i + 1 < len(toks) else None
            i += 1
            continue
        out.append(t)
        i += 1
    if not out or not src:
        return None
    return out


def main():
    targets = sys.argv[1:]
    if not targets:
        print(__doc__)
        return 2
    if not BUILD.is_dir():
        print("no build dir: %s（先跑一次覆盖率构建）" % BUILD, file=sys.stderr)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    bad = []
    for tgt in targets:
        obj = find_rule(tgt)
        if not obj:
            bad.append((tgt, "build.ninja 里没有该目标的 .cpp.o 编译边（目标没建起来？目录为空？）"))
            continue
        toks = command_for(obj)
        if not toks:
            bad.append((tgt, "取不到编译命令"))
            continue
        script = OUT_DIR / ("%s.sh" % tgt)
        script.write_text(
            "#!/bin/sh\n"
            "# 由 tools/gen_core_gate.py 从 build.ninja 的真实编译命令生成，勿手改。\n"
            "# 源对象: %s\n"
            "# 用法: sh %s <你的测试 .cpp>\n"
            "if [ $# -ne 1 ]; then echo 'usage: $0 <file.cpp>' >&2; exit 2; fi\n"
            "exec %s -fsyntax-only \"$1\"\n"
            % (obj, script.name, " ".join(shlex.quote(t) for t in toks)),
            encoding="utf-8")
        os.chmod(script, 0o755)
        print("wrote %s (from %s)" % (script.relative_to(ROOT), obj))
    for tgt, why in bad:
        print("MISS %s: %s" % (tgt, why), file=sys.stderr)
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main())
