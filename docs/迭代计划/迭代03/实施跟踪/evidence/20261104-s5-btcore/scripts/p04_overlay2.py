#!/usr/bin/env python3
# Compile src/TestUnits/fixtures/p04_cross_session_clock/cross_session_test.cpp in place of
# p07_tick_loader_test.cpp using the exact ninja compile/link commands of an existing
# CoverageTests_btcore build, writing all outputs under <out>.
import shlex, subprocess, sys
from pathlib import Path
root = Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
build = Path(sys.argv[1]); out = Path(sys.argv[2])
if out.exists(): raise SystemExit('output directory must not already exist')
out.mkdir(parents=True)
cmds = subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_btcore'],text=True).splitlines()
compile_cmd = next(c for c in cmds if ' -c ' in c and c.rstrip().endswith('/p07_tick_loader_test.cpp'))
args = shlex.split(compile_cmd)
new=[]; i=0
while i < len(args):
    if args[i] in ('-o','-MF','-MT'): i+=2; continue
    if args[i] == '-MD': i+=1; continue
    if args[i].endswith('/p07_tick_loader_test.cpp'):
        new.append(str(Path(sys.argv[3]))); i+=1; continue
    new.append(args[i]); i+=1
new += ['-o', str(out/'fixture.o')]
(out/'compile-command.txt').write_text(shlex.join(new)+'\n')
subprocess.run(new, cwd=build, check=True, stdout=(out/'compile.log').open('w'), stderr=subprocess.STDOUT)
raw = cmds[-1]
assert raw.startswith(': && ') and raw.endswith(' && :'), raw[:80]
link = shlex.split(raw[len(': && '):-len(' && :')])
for i,item in enumerate(link):
    if item.endswith('/p07_tick_loader_test.cpp.o'): link[i] = str(out/'fixture.o')
    if item == '-o': link[i+1] = str(out/'fixture')
(out/'link-command.txt').write_text(shlex.join(link)+'\n')
subprocess.run(link, cwd=build, check=True, stdout=(out/'link.log').open('w'), stderr=subprocess.STDOUT)
print('built', out/'fixture')
