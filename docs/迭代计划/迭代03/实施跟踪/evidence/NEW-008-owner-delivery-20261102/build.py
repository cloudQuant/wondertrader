import hashlib, json, pathlib, shlex, subprocess, sys
repo=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
build=pathlib.Path('/tmp/wt-i03-r42-003-20261102/build')
out=pathlib.Path('/tmp/wt-i03-owner-delivery-20261102')
ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102'
mode=sys.argv[1]
assert mode in ('red','green','normal')
raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_wtcore'],text=True).splitlines()
test=next(x for x in raw if ' -c ' in x and x.endswith('/p02_wtcore_engines2/r42_parser_adapter_test.cpp'))
prod=next(x for x in raw if ' -c ' in x and x.endswith('/WtCore/TraderAdapter.cpp') and 'CoverageTests_wtcore.dir' in x)
link=next(x for x in raw if x.startswith(': && /usr/bin/c++') and ' -o build_x64/Debug/bin/TestUnits/CoverageTests_wtcore ' in x)
testfile=repo/'src/TestUnits/coverage/n02_wtcore_engines/diff_owner_sink_registry_test.cpp'
prodfile=out/'red/src/WtCore/TraderAdapter.cpp' if mode=='red' else repo/'src/WtCore/TraderAdapter.cpp'
headerfile=out/'red/src/WtCore/TraderAdapter.h' if mode=='red' else repo/'src/WtCore/TraderAdapter.h'
commands=[]
for line,source,name in [(test,testfile,f'{mode}-test.o'),(prod,prodfile,f'{mode}-prod.o')]:
 args=shlex.split(line);args[-1]=str(source);extra=['-I'+str(repo/'src'),'-I'+str(repo/'src/WtCore')]
 if mode=='red':extra.insert(0,'-I'+str(out/'red/src'))
 if mode!='normal':extra.insert(0,'-DWT_OWNER_ROUTING_TEST_HOOKS')
 args[1:1]=extra;target=out/name
 for flag in ('-MT','-MF','-o'):args[args.index(flag)+1]=str(target)+('.d' if flag=='-MF' else '')
 commands.append(args)
args=shlex.split(link.removeprefix(': && ').removesuffix(' && :'))
for i,a in enumerate(args):
 if a.endswith('/p02_wtcore_engines2/r42_parser_adapter_test.cpp.o'):args[i]=str(out/f'{mode}-test.o')
 elif a.endswith('/WtCore/TraderAdapter.cpp.o'):args[i]=str(out/f'{mode}-prod.o')
args[args.index('-o')+1]=str(out/f'{mode}-focused');commands.append(args)
files=[prodfile,testfile,headerfile]
def hashes():return ''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p)+'\n' for p in files)
(ev/f'{mode}-commands.json').write_text(json.dumps(commands,indent=2));(ev/f'{mode}-source-before.sha256').write_text(hashes())
for i,cmd in enumerate(commands):
 r=subprocess.run(cmd,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (ev/f'{mode}-build-{i}.log').write_text(r.stdout+f'\nEXIT={r.returncode}\n');print(mode,'build',i,r.returncode,flush=True)
 if r.returncode:raise SystemExit(r.returncode)
(ev/f'{mode}-source-after.sha256').write_text(hashes())
(ev/f'{mode}-binary.sha256').write_text(hashlib.sha256((out/f'{mode}-focused').read_bytes()).hexdigest()+'\n')
