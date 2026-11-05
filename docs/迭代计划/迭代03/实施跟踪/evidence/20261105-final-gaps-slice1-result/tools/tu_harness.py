#!/usr/bin/env python3
"""tu_harness.py BUILD TARGET OUT TEST_SRC [EXTRA_SRC[=VARIANT] ...]
Standalone gtest binary: TEST_SRC (+ each EXTRA_SRC, optionally compiled from VARIANT
with EXTRA_SRC's directory on the include path) compiled with TARGET's flags, linked
with the library part of TARGET's link line (objects ahead of archives, so a variant
object replaces the archive member of the same TU)."""
import hashlib, os, shlex, subprocess, sys, pathlib
def link_segment(argv):
    segs, cur = [], []
    for a in argv:
        if a == '&&': segs.append(cur); cur = []
        else: cur.append(a)
    segs.append(cur)
    return next(s for s in segs if '-o' in s)
build = pathlib.Path(sys.argv[1]).resolve(); target = sys.argv[2]; out = pathlib.Path(sys.argv[3]).resolve()
units = [(str(pathlib.Path(sys.argv[4]).resolve()), None)]
for a in sys.argv[5:]:
    src, _, var = a.partition('=')
    units.append((str(pathlib.Path(src).resolve()), str(pathlib.Path(var).resolve()) if var else None))
lines = subprocess.check_output(['/opt/homebrew/bin/ninja', '-C', str(build), '-t', 'commands', target], text=True).splitlines()
def compile_cmd(src):
    for line in lines:
        if ' -c ' in line:
            c = shlex.split(line)
            if os.path.realpath(c[c.index('-c') + 1]) == src:
                return c
    raise SystemExit('no compile command for ' + src)
out.parent.mkdir(parents=True, exist_ok=True)
objs = []
for n, (src, var) in enumerate(units):
    c = compile_cmd(src)
    c[c.index('-c') + 1] = var or src
    real = var or src
    key = hashlib.sha256((target + '|' + real + '|' + hashlib.sha256(open(real,'rb').read()).hexdigest()).encode()).hexdigest()[:16]
    cache = out.parent / 'objs'; cache.mkdir(exist_ok=True)
    obj = str(cache / (pathlib.Path(real).name + '.' + key + '.o')); c[c.index('-o') + 1] = obj
    if os.path.exists(obj):
        print('cached', real, flush=True); objs.append(obj); continue
    for flag in ('-MF', '-MT'):
        while flag in c:
            j = c.index(flag); del c[j:j+2]
    while '-MD' in c: c.remove('-MD')
    c.insert(1, '-I' + os.path.dirname(src))
    print('compile', var or src, flush=True)
    if subprocess.run(c, cwd=str(build)).returncode != 0: raise SystemExit('compile failed')
    objs.append(obj)
link = link_segment(shlex.split(lines[-1]))
keep = [link[0]] + [a for a in link[1:] if not a.endswith('.o')]
keep[keep.index('-o') + 1] = str(out)
first_lib = next(k for k, a in enumerate(keep) if a.endswith('.a') or a.startswith('-l') or a.endswith('.dylib'))
keep[first_lib:first_lib] = objs
pathlib.Path(str(out) + '.link.txt').write_text(' '.join(shlex.quote(x) for x in keep) + '\n')
r = subprocess.run(keep, cwd=str(build)); print('link exit', r.returncode); sys.exit(r.returncode)
