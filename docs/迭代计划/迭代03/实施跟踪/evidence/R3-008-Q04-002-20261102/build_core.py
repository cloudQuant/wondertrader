import hashlib,json,pathlib,shlex,subprocess
repo=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); build=pathlib.Path('/tmp/wt-i03-r42-003-20261102/build'); out=pathlib.Path('/tmp/wt-i03-order-ledger-20261102'); ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R3-008-Q04-002-20261102'
raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_wtcore'],text=True).splitlines()
test=next(x for x in raw if ' -c ' in x and x.endswith('/p02_wtcore_engines2/r42_parser_adapter_test.cpp'))
prod=next(x for x in raw if ' -c ' in x and x.endswith('/WtCore/TraderAdapter.cpp') and 'CoverageTests_wtcore.dir' in x)
link=next(x for x in raw if x.startswith(': && /usr/bin/c++') and ' -o build_x64/Debug/bin/TestUnits/CoverageTests_wtcore ' in x)
commands=[];files=[]
for line,relative,name in [(test,'src/TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_buy_test.cpp','buy.o'),(test,'src/TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_sell_test.cpp','sell.o'),(prod,'src/WtCore/TraderAdapter.cpp','core-prod.o')]:
 args=shlex.split(line); args[-1]=str(repo/relative); args[1:1]=['-I'+str(repo/'src')]; target=out/name
 for flag in ('-MT','-MF','-o'):args[args.index(flag)+1]=str(target)+('.d' if flag=='-MF' else '')
 commands.append(args);files.append(repo/relative)
args=shlex.split(link.removeprefix(': && ').removesuffix(' && :'))
for i,a in enumerate(args):
 if a.endswith('/p02_wtcore_engines2/r42_parser_adapter_test.cpp.o'):args[i]=str(out/'buy.o')
 elif a.endswith('/WtCore/TraderAdapter.cpp.o'):args[i]=str(out/'core-prod.o')
args.insert(args.index(str(out/'buy.o'))+1,str(out/'sell.o'));args[args.index('-o')+1]=str(out/'core-focused');commands.append(args)
files += [repo/'src/WtCore/TraderAdapter.h',repo/'src/TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_fixture.h']
(ev/'core-commands.json').write_text(json.dumps(commands,indent=2)); (ev/'core-sources-before.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p)+'\n' for p in files))
for i,cmd in enumerate(commands):
 r=subprocess.run(cmd,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(ev/f'core-build-{i}.log').write_text(r.stdout+f'\nEXIT={r.returncode}\n');print('core build',i,r.returncode,flush=True)
 if r.returncode:raise SystemExit(r.returncode)
(ev/'core-sources-after.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p)+'\n' for p in files));(ev/'core-binary.sha256').write_text(hashlib.sha256((out/'core-focused').read_bytes()).hexdigest()+'  core-focused\n')
