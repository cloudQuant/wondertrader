#!/usr/bin/env python3
"""Rebuild one coverage test target's objects from an overlay source tree and
link it against the libraries already present in the coverage build dir.

usage: overlay_build.py <cov-build-dir> <target-name> <overlay-src> <out-dir> [-D...]

Only the target's own objects (test TUs + adapter TUs) are recompiled from the
overlay; the remaining libraries/dylibs come from <cov-build-dir> as-is.  The
exact commands are written to <out-dir>/commands.sh for evidence.
"""
import os, shlex, subprocess, sys

build, target, overlay, out = sys.argv[1:5]
extra = sys.argv[5:]
repo_src = '/Users/yunjinqi/Documents/new_projects/wondertrader/src'
os.makedirs(out, exist_ok=True)
exe = f'build_x64/Debug/bin/TestUnits/{target}'
def cmds(t):
    return subprocess.check_output(['ninja', '-t', 'commands', t], cwd=build, text=True).strip().splitlines()
link = cmds(exe)[-1]
objdir = f'TestUnits/coverage/CMakeFiles/{target}.dir/'
objs = [w for w in shlex.split(link.split('&&', 1)[1]) if w.startswith(objdir) and w.endswith('.o')]
script = ['set -e', f'cd {shlex.quote(build)}']
newlink = link
for o in objs:
    c = cmds(o)[-1].replace(repo_src, overlay.rstrip('/'))
    newo = os.path.join(out, 'obj', o[len(objdir):])
    os.makedirs(os.path.dirname(newo), exist_ok=True)
    c = c.replace(' -o ' + o, ' -o ' + newo).replace(' -MT ' + o, ' -MT ' + newo).replace(' -MF ' + o + '.d', ' -MF ' + newo + '.d')
    c = c.replace(' -c ', ' ' + ' '.join(extra) + ' -c ', 1) if extra else c
    script.append(c)
    newlink = newlink.replace(' ' + o + ' ', ' ' + newo + ' ')
newexe = os.path.join(out, target)
newlink = newlink.replace(' -o ' + exe + ' ', ' -o ' + newexe + ' ')
script.append(newlink)
open(os.path.join(out, 'commands.sh'), 'w').write('\n'.join(script) + '\n')
rc = subprocess.call(['bash', os.path.join(out, 'commands.sh')], stdout=open(os.path.join(out, 'build.log'), 'w'), stderr=subprocess.STDOUT)
print('build exit', rc, '->', newexe)
sys.exit(rc)
