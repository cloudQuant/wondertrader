import os, shlex, subprocess, pathlib, hashlib
root='/Users/yunjinqi/Documents/new_projects/wondertrader/src'
old='/tmp/wt-i02-fresh-20261101-v8-src/src'
build='/tmp/wt-i02-fresh-20261101-v8-build'
out='/tmp/wt-i03-l04-contract'
cmds=subprocess.check_output(['/Users/yunjinqi/opt/anaconda3/bin/ninja','-C',build,'-t','commands','CoverageTests'],text=True).splitlines()
def run(args):
 print(shlex.join(args),flush=True)
 subprocess.run(args,cwd=build,check=True)
def compile_one(name, target):
 line=next(x for x in cmds if target+'.o -c ' in x)
 args=shlex.split(line)
 args=[x.replace(old,root) for x in args]
 for flag in ('-MT','-MF'):
  ix=args.index(flag); del args[ix:ix+2]
 ix=args.index('-o'); args[ix+1]=out+'/'+name+'.o'
 if name=='manager': args.insert(1,'-I'+build+'/TestUnits/coverage/cov_boost_include')
 run(args)
compile_one('test','l03_contract_info_test.cpp')
compile_one('manager','WTSBaseDataMgr.cpp')
line=next(x for x in cmds if ' -o build_x64/Debug/bin/TestUnits/CoverageTests ' in x)
args=shlex.split(line)[2:-2]
ix=next(i for i,x in enumerate(args) if x.endswith('/l03_includes/l03_contract_info_test.cpp.o'))
args[ix]=out+'/test.o'
for variant in ('red','green'):
 a=args.copy()
 ix=a.index('-o'); a[ix+1]=out+'/'+variant
 if variant=='green':
  ix=a.index('/opt/homebrew/lib/libgtest_main.a'); a.insert(ix,out+'/manager.o')
 run(a)
for rel in ('Includes/WTSMarcos.h','Includes/WTSContractInfo.hpp','WTSTools/WTSBaseDataMgr.h','WTSTools/WTSBaseDataMgr.cpp','TestUnits/coverage/l03_includes/l03_contract_info_test.cpp'):
 for label,base in (('current',root),('v8',old)):
  p=pathlib.Path(base)/rel
  print(label,rel,hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else 'missing')
