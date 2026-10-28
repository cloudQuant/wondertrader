#!/usr/bin/env python3
"""为每个可编译的收发族模块生成「真实构建口径」的语法门禁脚本。

为什么需要它：lane 自己拼 flags 会把可编译性判错（本轮已两次翻车：少了 -std=gnu++17
多了 -IAPI，测出假的「22 个不可编译」）。所以 flags 一律从构建系统本身取：
`ninja -t commands <对象>` 给出该对象在真实构建里的完整命令，去掉输出/依赖项，
把 `-o X -c Y` 换成 `-fsyntax-only "$1"`。

可编译性判定口径沿用 §2.2：磁盘上有生产对象 = 该模块在顶层构建里真的被编译。
"""
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
BUILD = os.path.join(ROOT, "build-cov")
COV_DIR = os.path.join(ROOT, "src", "TestUnits", "coverage")
OUT_DIR = os.path.join(os.path.dirname(__file__), "gate_flags")

PLACEHOLDER_BODY = """#include <gtest/gtest.h>

// 门禁占位：只为让构建系统产出该模块测试对象的编译命令，供 gate_flags 取真实 flags。
TEST(gate_placeholder_{ident}, compiles_with_module_flags) {{ SUCCEED(); }}
"""


def buildable_modules():
    mods = []
    for name in sorted(os.listdir(os.path.join(ROOT, "src"))):
        d = os.path.join(ROOT, "src", name)
        if not os.path.isdir(d):
            continue
        if not (name.startswith("Parser") or name.startswith("Trader") or name.endswith("Loader")):
            continue
        objdir = os.path.join(BUILD, name, "CMakeFiles", name + ".dir")
        has_obj = os.path.isdir(objdir) and any(f.endswith(".o") for f in os.listdir(objdir))
        if has_obj:
            mods.append(name)
    return mods


def placeholder_path(mod):
    ident = re.sub(r"[^0-9A-Za-z_]", "_", mod)
    d = os.path.join(COV_DIR, "t_" + mod)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "gate_placeholder_test.cpp")
    if not os.path.exists(p):
        with open(p, "w") as f:
            f.write(PLACEHOLDER_BODY.format(ident=ident))
    return p


def ninja_obj_path(mod):
    """在 build.ninja 里找该模块占位对象的目标路径。"""
    needle = "t_{}/gate_placeholder_test.cpp.o".format(mod)
    with open(os.path.join(BUILD, "build.ninja"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("build ") and needle in line:
                out = line[len("build "):].split(":")[0].strip()
                if out.endswith(needle):
                    return out
    return None


def gate_command(mod, obj):
    cmd = subprocess.run(["ninja", "-C", BUILD, "-t", "commands", obj],
                         capture_output=True, text=True)
    if cmd.returncode != 0:
        return None
    toks = shlex.split(cmd.stdout.strip().splitlines()[-1])
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t in ("-o", "-MT", "-MF"):
            i += 2
            continue
        if t in ("-c", "-MD", "-MMD"):
            i += 1
            continue
        if t.endswith("gate_placeholder_test.cpp"):
            i += 1
            continue
        out.append(t)
        i += 1
    return out


def main():
    if not os.path.isdir(BUILD):
        print("no build dir: %s" % BUILD, file=sys.stderr)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    mods = buildable_modules()
    print("buildable transport modules: %d -> %s" % (len(mods), " ".join(mods)))
    for m in mods:
        placeholder_path(m)
    subprocess.run(["cmake", "-S", os.path.join(ROOT, "src"), "-B", BUILD],
                   capture_output=True, text=True)
    written, missing = 0, []
    for m in mods:
        obj = ninja_obj_path(m)
        if not obj:
            missing.append(m)
            continue
        toks = gate_command(m, obj)
        if not toks:
            missing.append(m)
            continue
        script = os.path.join(OUT_DIR, "t_%s.sh" % m)
        with open(script, "w") as f:
            f.write("#!/bin/sh\n# 由 tools/gen_gate_flags.py 从真实构建命令生成，勿手改。\n")
            f.write("# 用法: sh %s <你的测试 .cpp>\n" % os.path.basename(script))
            f.write("if [ $# -ne 1 ]; then echo 'usage: $0 <file.cpp>' >&2; exit 2; fi\n")
            f.write("exec ")
            f.write(" ".join(shlex.quote(t) for t in toks))
            f.write(" -fsyntax-only \"$1\"\n")
        os.chmod(script, 0o755)
        written += 1
    print("gate scripts written: %d; without object rule: %s"
          % (written, " ".join(missing) if missing else "none"))
    return 0 if written == len(mods) else 1


if __name__ == "__main__":
    sys.exit(main())
