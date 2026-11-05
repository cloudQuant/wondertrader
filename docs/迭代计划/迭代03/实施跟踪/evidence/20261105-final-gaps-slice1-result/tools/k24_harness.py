#!/usr/bin/env python3
"""k24_harness.py BUILD OUT_BIN EVENTNOTIFIER_SRC_DIR
Standalone binary: s1_k24_notifier_bounded_stop_test.cpp + EventNotifier.cpp from
EVENTNOTIFIER_SRC_DIR (its own EventNotifier.h shadows the repository header) +
the library part of the CoverageTests_wtcore link line. Used for the K24 red control
(pre-K24 EventNotifier from evidence/20261104-s5-main/k24-notifier/before/src/WtCore)."""
import os, shlex, subprocess, sys, pathlib
def link_segment(argv):
    segs, cur = [], []
    for a in argv:
        if a == '&&':
            segs.append(cur); cur = []
        else:
            cur.append(a)
    segs.append(cur)
    return next(s for s in segs if '-o' in s)

build = pathlib.Path(sys.argv[1]).resolve(); out = pathlib.Path(sys.argv[2]).resolve(); endir = pathlib.Path(sys.argv[3]).resolve()
repo = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader/src')
lines = subprocess.check_output(['/opt/homebrew/bin/ninja', '-C', str(build), '-t', 'commands', 'CoverageTests_wtcore'], text=True).splitlines()
def compile_cmd(src):
    for line in lines:
        if ' -c ' in line and 'CoverageTests_wtcore.dir' in line:
            c = shlex.split(line)
            if os.path.realpath(c[c.index('-c') + 1]) == str(src):
                return c
    raise SystemExit('no command for %s' % src)
out.parent.mkdir(parents=True, exist_ok=True)
objs = []
test_src = repo / 'TestUnits/coverage/n02_wtcore_engines/s1_k24_notifier_bounded_stop_test.cpp'
for src, label, extra in ((test_src, 'test', ['-I' + str(endir.parent)]), (repo / 'WtCore/EventNotifier.cpp', 'notifier', ['-I' + str(repo / 'WtCore')])):
    c = compile_cmd(src)
    real = endir / 'EventNotifier.cpp' if label == 'notifier' else src
    c[c.index('-c') + 1] = str(real)
    obj = str(out) + '.' + label + '.o'
    c[c.index('-o') + 1] = obj
    for flag in ('-MF', '-MT'):
        while flag in c:
            j = c.index(flag); del c[j:j+2]
    while '-MD' in c: c.remove('-MD')
    c[1:1] = extra
    print('compile', real, flush=True)
    if subprocess.run(c, cwd=str(build)).returncode != 0: raise SystemExit('compile failed ' + label)
    objs.append(obj)
link = link_segment(shlex.split(lines[-1]))
keep = [link[0]] + [a for a in link[1:] if not a.endswith('.o')]
keep[keep.index('-o') + 1] = str(out)
first_lib = next(k for k, a in enumerate(keep) if a.endswith('.a') or a.startswith('-l') or a.endswith('.dylib'))
keep[first_lib:first_lib] = objs
pathlib.Path(str(out) + '.link.txt').write_text(' '.join(shlex.quote(x) for x in keep) + '\n')
r = subprocess.run(keep, cwd=str(build)); print('link exit', r.returncode); sys.exit(r.returncode)
