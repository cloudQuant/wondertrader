import hashlib, json, pathlib, shlex, subprocess
repo = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
build = pathlib.Path('/tmp/wt-i03-r78-20261102/build')
out = pathlib.Path('/tmp/wt-i03-order-ledger-20261102')
evidence = repo / 'docs/迭代计划/迭代03/实施跟踪/evidence/R3-008-Q04-002-20261102'
raw = subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_uftcore'], text=True).splitlines()
testline = next(x for x in raw if ' -c ' in x and x.endswith('/q04_uftcore3/r78_uftcore_adapter_init_test.cpp'))
prodline = next(x for x in raw if ' -c ' in x and x.endswith('/WtUftCore/TraderAdapter.cpp') and 'CoverageTests_uftcore.dir' in x)
linkline = next(x for x in raw if x.startswith(': && /usr/bin/c++') and ' -o build_x64/Debug/bin/TestUnits/CoverageTests_uftcore ' in x)
commands=[]
for line, source, target in [(testline,repo/'src/TestUnits/coverage/q04_uftcore3/q04_trader_entrust_paths_test.cpp',out/'test.o'),(prodline,repo/'src/WtUftCore/TraderAdapter.cpp',out/'prod.o')]:
    args=shlex.split(line)
    args[-1]=str(source)
    for flag in ('-MT','-MF','-o'):
        idx=args.index(flag)+1
        args[idx]=str(target)+('.d' if flag=='-MF' else '')
    # Resolve quoted includes from the current repository first; all other
    # dependencies are inherited from the recorded isolated snapshot build.
    args[1:1]=['-I'+str(repo/'src')]
    commands.append(args)
args=shlex.split(linkline.removeprefix(': && ').removesuffix(' && :'))
for i,a in enumerate(args):
    if a.endswith('/q04_uftcore3/r78_uftcore_adapter_init_test.cpp.o'): args[i]=str(out/'test.o')
    elif a.endswith('/WtUftCore/TraderAdapter.cpp.o'): args[i]=str(out/'prod.o')
args[args.index('-o')+1]=str(out/'focused')
commands.append(args)
(evidence/'commands.json').write_text(json.dumps(commands,ensure_ascii=False,indent=2))
files=[repo/'src/WtUftCore/TraderAdapter.cpp',repo/'src/WtUftCore/TraderAdapter.h',repo/'src/TestUnits/coverage/q04_uftcore3/q04_trader_entrust_paths_test.cpp',repo/'src/TestUnits/coverage/q04_uftcore3/q04_trader_fixture.h']
(evidence/'source-before.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p)+'\n' for p in files))
for i,args in enumerate(commands):
    result=subprocess.run(args,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (evidence/f'build-{i}.log').write_text(result.stdout+f'\nEXIT={result.returncode}\n')
    print('build',i,'exit',result.returncode,flush=True)
    if result.returncode: raise SystemExit(result.returncode)
(evidence/'source-after.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p)+'\n' for p in files))
(evidence/'binary.sha256').write_text(hashlib.sha256((out/'focused').read_bytes()).hexdigest()+'  focused\n')
