#!/Users/yunjinqi/opt/anaconda3/bin/python
import json, os, shlex, subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[4]
build = Path('/tmp/wt-i03-r58-20261102-build')
snapshot = Path('/tmp/wt-i03-r58-20261102-src')
out = Path(sys.argv[1]).resolve()
if out.exists(): raise SystemExit('output directory must not already exist')
out.mkdir(parents=True)
repo_sources = [root/'src/WtBtCore/HisDataReplayer.cpp', root/'src/WtBtCore/HisDataReplayer.h', root/'src/TestUnits/coverage/p07_btcore_replayer2/p07_tick_loader_test.cpp']
import hashlib
for source in repo_sources:
    mirror = snapshot/source.relative_to(root)
    if source.read_bytes() != mirror.read_bytes(): raise SystemExit('snapshot mismatch: '+str(source))
    print(hashlib.sha256(source.read_bytes()).hexdigest(), source)
commands=json.loads((build/'compile_commands.json').read_text())
command=next(x for x in commands if x['file'].endswith('/p07_tick_loader_test.cpp'))
args=shlex.split(command['command'])
new=[]; i=0
while i<len(args):
    if args[i] in ('-o','-MF','-MT'):
        i+=2; continue
    if 'cov_boost_include' in args[i]: i+=1; continue
    if args[i].endswith('/p07_tick_loader_test.cpp'):
        new.append(str(root/'src/TestUnits/fixtures/p04_cross_session_clock/cross_session_test.cpp')); i+=1; continue
    new.append(args[i]); i+=1
new += ['-o',str(out/'fixture.o')]
print(shlex.join(new), flush=True)
subprocess.run(new,cwd=build,check=True,stdout=(out/'compile.log').open('w'),stderr=subprocess.STDOUT)
raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_btcore'],text=True).splitlines()[-1]
assert raw.startswith(': && ') and raw.endswith(' && :')
link=shlex.split(raw[len(': && '):-len(' && :')])
for i,item in enumerate(link):
    if item.endswith('/p07_tick_loader_test.cpp.o'): link[i]=str(out/'fixture.o')
    if item=='-o': link[i+1]=str(out/'fixture')
print(shlex.join(link),file=(out/'link-command.txt').open('w'))
subprocess.run(link,cwd=build,check=True,stdout=(out/'link.log').open('w'),stderr=subprocess.STDOUT)
filter=os.environ.get('WT_P04_FILTER','P07TickLoaderTest.pureTickContinuousAndFutureDispatchOnlyWithNoDataDay')
env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/'profile-%p.profraw')
with (out/'run.log').open('w') as f:
    result=subprocess.run([str(out/'fixture'),'--gtest_filter='+filter],cwd=out,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60)
print('exit',result.returncode,'log',out/'run.log')
sys.exit(result.returncode)
