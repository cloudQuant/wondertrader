#!/Users/yunjinqi/opt/anaconda3/bin/python
"""Replay three next-day negative probes against an existing isolated build."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

root=Path(__file__).resolve().parents[4]
fixture=Path(__file__).resolve().parent
build=Path('/tmp/wt-i03-r202-001-midnight/build')
old_snap=Path('/tmp/wt-i03-r202-001-midnight/snapshot/src')
tsan=os.environ.get('R202_TSAN')=='1'
out=Path(sys.argv[1]).resolve()
out.mkdir(parents=True,exist_ok=False)
snap=out/'snapshot'/'src'
shutil.copytree(old_snap,snap)
for path in ['Includes/WTSSessionInfo.hpp','Share/TimeUtils.hpp']:
 shutil.copyfile(root/'src'/path,snap/path)
source=snap/'TestUnits/coverage/q01_wtcore3/r202_wtcore_public_surface_test.cpp'
merged=out/'merged.cpp'
merged.write_text(source.read_text()+'\n'+(fixture/'nextday.inc').read_text())
files=['WtCtaTicker.cpp','WtCtaTicker.h','WtSelTicker.cpp','WtSelTicker.h','WtHftTicker.cpp','WtHftTicker.h']
lines=[]
for name in files:
 cur=root/'src/WtCore'/name
 old=old_snap/'WtCore'/name
 digest=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
 lines.append(f'{name} current={digest(cur)} snapshot={digest(old)} equal={digest(cur)==digest(old)}')
 if cur.read_bytes()!=old.read_bytes(): raise RuntimeError(f'ticker drift: {name}')
session=root/'src/Includes/WTSSessionInfo.hpp'; oldsession=old_snap/'Includes/WTSSessionInfo.hpp'
lines.append(f'WTSSessionInfo.hpp current={digest(session)} snapshot={digest(oldsession)} equal={digest(session)==digest(oldsession)}')
(out/'hashes.txt').write_text('\n'.join(lines)+'\n')
commands=json.loads((build/'compile_commands.json').read_text())
entry=next(x for x in commands if x['file'].endswith('q01_wtcore3/r202_wtcore_public_surface_test.cpp'))
args=[a.replace(str(old_snap),str(snap)) for a in shlex.split(entry['command'])]; args[args.index('-o')+1]=str(out/'test.o'); args[args.index('-c')+1]=str(merged)
args.extend(['-I',str(source.parent)])
if tsan:
 args[0]='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++';args[1:1]=['-include','stdlib.h'];args.extend(['-fsanitize=thread','-fno-omit-frame-pointer','-Wno-error'])
with (out/'build.log').open('w') as log:
 subprocess.run(args,cwd=build,check=True,stdout=log,stderr=subprocess.STDOUT)
 for name in ['WtCtaTicker.cpp','WtSelTicker.cpp','WtHftTicker.cpp']:
  entry=next(x for x in commands if x['file'].endswith('/WtCore/'+name) and 'CoverageTests_wtcore.dir/' in x['command'])
  tickargs=[a.replace(str(old_snap),str(snap)) for a in shlex.split(entry['command'])]
  tickargs[tickargs.index('-o')+1]=str(out/(name+'.o'))
  if tsan:
   tickargs[0]='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++';tickargs[1:1]=['-include','stdlib.h'];tickargs.extend(['-fsanitize=thread','-fno-omit-frame-pointer','-Wno-error'])
  subprocess.run(tickargs,cwd=build,check=True,stdout=log,stderr=subprocess.STDOUT)
 link=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_wtcore'],text=True).splitlines()[-1]
 args=shlex.split(link)[2:-2]
 if tsan:
  args[0]='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++';args=[a for a in args if a not in ['-Wl,-no_warn_duplicate_libraries','-Wl,-fatal_warnings']];args.append('-fsanitize=thread')
 oldobj='TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/q01_wtcore3/r202_wtcore_public_surface_test.cpp.o'
 args[args.index(oldobj)]=str(out/'test.o');args[args.index('-o')+1]=str(out/'test')
 for name in ['WtCtaTicker.cpp','WtSelTicker.cpp','WtHftTicker.cpp']:
  obj='TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/__/__/WtCore/'+name+'.o'
  args[args.index(obj)]=str(out/(name+'.o'))
 subprocess.run(args,cwd=build,check=True,stdout=log,stderr=subprocess.STDOUT)
results=[]
tests=[f'{g}.{n}' for g in ['R202NextDay','R202Reentry'] for n in ['Cta','Sel','Hft']]
tests.extend(['R202CtaTicker.FullDayMidnightCloseEndsSessionAndStoresTradingDate',
 'R202CtaTicker.OrdinaryCloseStillEndsSessionAndStoresTradingDate',
 'R202SelTicker.FullDayMidnightCloseEndsSessionOnce','R202SelTicker.OrdinaryCloseStillEndsSession',
 'R202HftTicker.FullDayMidnightCloseEndsSessionOnce','R202HftTicker.OrdinaryCloseStillEndsSession'])
if tsan: tests=[f'R202NextDay.{n}' for n in ['Cta','Sel','Hft']]
for label in tests:
 env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/f'{label}.profraw')
 if tsan: env['TSAN_OPTIONS']='halt_on_error=1:exitcode=66'
 work=out/label;work.mkdir()
 with (out/f'{label}.log').open('w') as log:
  try:
   rc=subprocess.run([str(out/'test'),f'--gtest_filter={label}'],cwd=work,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=35).returncode
  except subprocess.TimeoutExpired:rc=124
 results.append((label,rc))
(out/'results.txt').write_text(''.join(f'{a} {b}\n' for a,b in results))
print(results)
for label,rc in results:
 log=(out/f'{label}.log').read_text()
 if tsan:
  if rc not in (-6,66) or 'WARNING: ThreadSanitizer: data race' not in log:
   raise RuntimeError(f'{label}: expected TSan race observation, got {rc}')
 elif label.startswith(('R202NextDay.','R202Reentry.')):
  if rc!=1 or '[  FAILED  ] 1 test' not in log:
   raise RuntimeError(f'{label}: expected known-red failure, got {rc}')
 elif rc!=0 or '[  PASSED  ] 1 test.' not in log or '[  SKIPPED ]' in log:
  raise RuntimeError(f'{label}: positive control did not pass, got {rc}')
print('Expected observations verified; negative probes are NOT product passes.')
