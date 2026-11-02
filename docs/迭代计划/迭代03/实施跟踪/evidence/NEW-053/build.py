import hashlib,json,pathlib,shlex,subprocess,sys
root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); old='/tmp/wt-i02-fresh-20261101-v8-src/src'; build=pathlib.Path('/tmp/wt-i02-fresh-20261101-v8-build'); out=pathlib.Path('/tmp/wt-i03-new053'); clang='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++'
bridge=build/'TestUnits/coverage/cov_boost_include/boost'; assert bridge.is_dir() and not bridge.is_symlink()
lines=subprocess.check_output(['/Users/yunjinqi/opt/anaconda3/bin/ninja','-C',str(build),'-t','commands','CoverageTests_wtcore'],text=True).splitlines()
inputs=['WtCore/TraderAdapter.cpp','WtCore/TraderAdapter.h','TestUnits/coverage/p11_wtcore_traderadapter/p11_usertag_bounds_test.cpp','TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_misc_test.cpp','TestUnits/coverage/p11_wtcore_traderadapter/p11_trader_fixture.h','TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_fixture.h','Share/ModuleHelper.hpp','Includes/WTSCollection.hpp']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest(); before={x:sha(root/'src'/x) for x in inputs}; commands=[]
for rel,label in [(inputs[0],'adapter'),(inputs[2],'p11test'),(inputs[3],'r59test')]:
 c=[s for s in lines if ' -c '+old+'/'+rel in s and 'CoverageTests_wtcore.dir' in s]; assert len(c)==1,(rel,len(c))
 p=shlex.split(c[0].replace(old,str(root/'src'))); p[0]=clang; p[1:1]=['-include','stdlib.h','-isystem','/usr/local/include','-fsanitize=address,undefined','-fno-omit-frame-pointer']; assert '-Werror' in p
 for flag,suffix in [('-MT','.o'),('-MF','.o.d'),('-o','.o')]: p[p.index(flag)+1]=str(out/(label+suffix))
 commands.append(p)
line=lines[-1]; assert line.startswith(': && '); p=shlex.split(line[len(': && '):]); assert p[-2:]==['&&',':']; p=p[:-2]; p[0]=clang; p[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
q=[]
for v in p:
 if v in ('-Wl,-fatal_warnings','-Wl,-no_warn_duplicate_libraries'): continue
 if v.startswith('TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/') and v.endswith('.o'):
  if '/__/__/' not in v or v.endswith('/TraderAdapter.cpp.o'): continue
 q.append(v.replace(old,str(root/'src')))
p=q; p[p.index('-o')+1]=str(out/'bin/fixture-asan'); first=next(i for i,v in enumerate(p) if v.startswith('TestUnits/coverage/') and v.endswith('.o')); p[first:first]=[str(out/'p11test.o'),str(out/'r59test.o'),str(out/'adapter.o')]; commands.append(p)
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
for i,c in enumerate(commands):
 with (out/f'build-{i}.log').open('w') as f: result=subprocess.run(c,cwd=build,stdout=f,stderr=subprocess.STDOUT)
 print('build',i,'exit',result.returncode,flush=True)
 if result.returncode:sys.exit(result.returncode)
after={x:sha(root/'src'/x) for x in inputs}; (out/'source-hashes.json').write_text(json.dumps({'before':before,'after':after,'binary':sha(out/'bin/fixture-asan')},indent=2)+'\n')
if before!=after: print('SOURCE DRIFT',flush=True);sys.exit(2)
