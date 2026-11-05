import json, os, pathlib, shlex, subprocess, sys
root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); build=pathlib.Path('/tmp/ctpm-i03-build'); out=pathlib.Path('/tmp/wt-i03-parser-correction'); rec=[]
def query(target):
 p=subprocess.run(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands',target],text=True,capture_output=True,check=True); return p.stdout.strip().splitlines()[-1]
def run(args, name):
 p=subprocess.run(args,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True); (out/(name+'.log')).write_text(p.stdout); rec.append({'name':name,'cwd':str(build),'argv':args,'exit':p.returncode}); (out/'commands.json').write_text(json.dumps(rec,indent=2)); print(name,p.returncode,flush=True)
 if p.returncode: sys.exit(p.returncode)
def obj(target,src,dest):
 a=shlex.split(query(target)); o=out/(dest+'.o')
 for flag in ['-o','-MF','-MT']:
  if flag in a:a[a.index(flag)+1]=str(o if flag!='-MF' else o.with_suffix('.d'))
 a[a.index('-c')+1]=str(root/src)
 run(a,dest);return str(o)
n='TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/'
m='TestUnits/coverage/CMakeFiles/CoverageTests_m_WtRunner.dir/'
prod=obj(n+'__/__/WtCore/ParserAdapter.cpp.o','src/WtCore/ParserAdapter.cpp','parser')
test=obj(n+'n01_wtcore_tradepaths/n01_parser_adapter_test.cpp.o','src/TestUnits/coverage/n01_wtcore_tradepaths/n01_parser_adapter_test.cpp','n01-test')
runner=obj(m+'__/__/WtRunner/WtRunner.cpp.o','src/WtRunner/WtRunner.cpp','runner')
rtest=obj(m+'m_WtRunner/r28_runner_main_test.cpp.o','src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp','r28-test')
main=obj(m+'__/__/WtRunner/main.cpp.o','src/WtRunner/main.cpp','main')
cfg=obj('WTSUtils/CMakeFiles/WTSUtils.dir/WTSCfgLoader.cpp.o','src/WTSUtils/WTSCfgLoader.cpp','cfg')
for name, target, objects in [('core','CoverageTests_wtcore',[prod,test]),('runner','CoverageTests_m_WtRunner',[prod,runner,rtest,main,cfg])]:
 raw=query(target); assert raw.startswith(': && ') and raw.endswith(' && :');a=shlex.split(raw[5:-5]); a=[x for x in a if not x.endswith('.o')]; a[a.index('-o')+1]=str(out/(name+'-tests')); a[1:1]=objects;run(a,name+'-link')
