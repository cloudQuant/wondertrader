#!/Users/yunjinqi/opt/anaconda3/bin/python
"""Controlled true-engine probes; test-only thread joins are not a product fix."""
import hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[4]
fixture=Path(__file__).resolve().parent
old=Path('/tmp/wt-i03-r202-001-midnight/snapshot/src')
build=Path('/tmp/wt-i03-r202-001-midnight/build')
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
snap=out/'snapshot'/'src';shutil.copytree(old,snap)
source=snap/'TestUnits/coverage/q01_wtcore3/r202_wtcore_public_surface_test.cpp'
engine=snap/'WtCore/WtSelEngine.cpp';orig=engine.read_text()
assert (root/'src/WtCore/WtSelEngine.cpp').read_bytes()==engine.read_bytes()
(out/'engine.sha256').write_text(hashlib.sha256(engine.read_bytes()).hexdigest()+'\n')
merged=out/'merged.cpp';merged.write_text(source.read_text()+'\n'+(fixture/'schedule.inc').read_text())
commands=json.loads((build/'compile_commands.json').read_text())
def compile_entry(suffix, target, override=None):
 entry=next(x for x in commands if x['file'].endswith(suffix) and 'CoverageTests_wtcore.dir/' in x['command'])
 args=[a.replace(str(old),str(snap)) for a in shlex.split(entry['command'])]
 args[args.index('-o')+1]=str(target)
 if override:args[args.index('-c')+1]=str(override)
 return args
with (out/'build.log').open('w') as log:
 args=compile_entry('q01_wtcore3/r202_wtcore_public_surface_test.cpp',out/'test.o',merged)
 args.extend(['-I',str(source.parent)])
 subprocess.run(args,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
 link=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_wtcore'],text=True).splitlines()[-1]
 base=shlex.split(link)[2:-2]
 oldtest='TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/q01_wtcore3/r202_wtcore_public_surface_test.cpp.o'
 oldengine='TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/__/__/WtCore/WtSelEngine.cpp.o'
 results=[]
 for mode in ['original','joined','key_fixed']:
  text=orig
  if mode!='original':
   target='\t\t}));\t\n\n\t\ttInfo->_last_exe_time = now;'
   assert text.count(target)==1
   text=text.replace(target,'\t\t}));\t\n\t\tthrd->join(); // TEST-ONLY: isolates identity, not a safe production fix\n\n\t\ttInfo->_last_exe_time = now;')
  if mode=='key_fixed':
   target='SelContextPtr ctx = getContext(tInfo->_id);';assert text.count(target)==1
   text=text.replace(target,'SelContextPtr ctx = getContext(v.first);')
  engine.write_text(text)
  obj=out/(mode+'.o');subprocess.run(compile_entry('/WtCore/WtSelEngine.cpp',obj),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
  args=base.copy();args[args.index(oldtest)]=str(out/'test.o');args[args.index(oldengine)]=str(obj);args[args.index('-o')+1]=str(out/mode)
  subprocess.run(args,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
  env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/(mode+'-%p.profraw'))
  with (out/(mode+'.log')).open('w') as runlog:
   try: rc=subprocess.run([str(out/mode),'--gtest_filter=R13RealSchedule.TriggeredDailyTargetsRegisteredContext'],cwd=out,env=env,stdout=runlog,stderr=subprocess.STDOUT,timeout=20).returncode
   except subprocess.TimeoutExpired:rc=124
  results.append((mode,rc))
env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/'monthly-%p.profraw')
with (out/'monthly.log').open('w') as runlog:
 try: monthly_rc=subprocess.run([str(out/'original'),'--gtest_filter=R13RealSchedule.MonthlyOnRequestedCalendarDay'],cwd=out,env=env,stdout=runlog,stderr=subprocess.STDOUT,timeout=20).returncode
 except subprocess.TimeoutExpired:monthly_rc=124
results.append(('monthly',monthly_rc))
(out/'results.txt').write_text(''.join(f'{m} {v}\n' for m,v in results));print(results)
if results[0][1] not in (-6,134) or results[1][1]!=1 or results[2][1]!=0 or results[3][1]!=1:sys.exit(1)
print('Expected red/controlled observations only; not a product fix.')
