#!/usr/bin/env python3
"""relink.py BUILD TARGET OUT [--variant PROD_SRC=VARIANT_SRC ...] [--extra-tu SRC ...]
Link a private copy of a coverage test binary into OUT:
 - objects of TARGET that do not exist (e.g. another agent's WIP test that does not
   compile yet) are dropped, and listed in OUT.dropped.txt;
 - --variant recompiles PROD_SRC's object from VARIANT_SRC (same flags, the original
   source directory added as -I so relative includes resolve) for red controls.
Nothing in the repository or in the build tree's own outputs is modified."""
import json, os, shlex, subprocess, sys, pathlib
def link_segment(argv):
    segs, cur = [], []
    for a in argv:
        if a == '&&':
            segs.append(cur); cur = []
        else:
            cur.append(a)
    segs.append(cur)
    return next(s for s in segs if '-o' in s)

args = sys.argv[1:]
build, target, out = pathlib.Path(args[0]).resolve(), args[1], pathlib.Path(args[2]).resolve()
variants = {}
extras = {}
i = 3
while i < len(args):
    if args[i] == '--variant':
        a, b = args[i+1].split('=', 1); variants[str(pathlib.Path(a).resolve())] = str(pathlib.Path(b).resolve()); i += 2
    elif args[i] == '--variant-extra':
        # SRC is a member of a static archive: the variant object is linked ahead of the archives.
        a, b = args[i+1].split('=', 1); extras[str(pathlib.Path(a).resolve())] = str(pathlib.Path(b).resolve()); i += 2
    else:
        raise SystemExit('bad arg ' + args[i])
out.parent.mkdir(parents=True, exist_ok=True)
ninja = '/opt/homebrew/bin/ninja'
lines = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands', target], text=True).splitlines()
link = lines[-1]
argv = link_segment(shlex.split(link))
dropped = []
replaced = {}
extra_objs = []
for src, var in list(variants.items()) + list(extras.items()):
    in_target = src in variants
    cmd = None
    for line in lines:
        if ' -c ' in line and ((target + '.dir') in line or not in_target):
            c = shlex.split(line)
            if os.path.realpath(c[c.index('-c') + 1]) == src:
                cmd = c; break
    if cmd is None: raise SystemExit('no compile command for ' + src)
    obj = cmd[cmd.index('-o') + 1]
    vobj = str(out) + '.' + pathlib.Path(src).name + '.variant.o'
    cmd = [c for c in cmd]
    cmd[cmd.index('-o') + 1] = vobj
    k = cmd.index('-c'); cmd[k + 1] = var
    for flag in ('-MD',):
        while flag in cmd: cmd.remove(flag)
    for flag in ('-MF', '-MT'):
        while flag in cmd:
            j = cmd.index(flag); del cmd[j:j+2]
    cmd.insert(1, '-I' + os.path.dirname(src))
    print('compile', var, flush=True)
    r = subprocess.run(cmd, cwd=str(build))
    if r.returncode != 0: raise SystemExit('variant compile failed: %d' % r.returncode)
    if in_target:
        replaced[obj] = vobj
    else:
        extra_objs.append(vobj)
new = []
for a in argv:
    if a in replaced:
        new.append(replaced[a]); continue
    if a.endswith('.o') and not os.path.isabs(a) and not (build / a).exists():
        dropped.append(a); continue
    new.append(a)
new[new.index('-o') + 1] = str(out)
if extra_objs:
    first_archive = next(k for k, a in enumerate(new) if a.endswith('.a'))
    new[first_archive:first_archive] = extra_objs
(pathlib.Path(str(out) + '.dropped.txt')).write_text('\n'.join(dropped) + '\n')
(pathlib.Path(str(out) + '.link.txt')).write_text(' '.join(shlex.quote(x) for x in new) + '\n')
r = subprocess.run(new, cwd=str(build))
print('link exit', r.returncode, 'dropped', len(dropped), flush=True)
sys.exit(r.returncode)
