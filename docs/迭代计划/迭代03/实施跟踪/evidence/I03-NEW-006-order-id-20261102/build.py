import json,pathlib,shlex,subprocess,sys
root=pathlib.Path('/tmp/wt-i03-time-orderid-20261102');src=root/'snapshot'; cmds=[]
for group in ('uft','core'):
 build=pathlib.Path('/tmp/wt-i03-r78-20261102/build' if group=='uft' else '/tmp/wt-i03-r42-003-20261102/build');target='CoverageTests_uftcore' if group=='uft' else 'CoverageTests_wtcore'
 lines=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands',target],text=True).splitlines()
 testcase='/q04_uftcore3/r78_uftcore_adapter_init_test.cpp' if group=='uft' else '/p02_wtcore_engines2/r42_parser_adapter_test.cpp'
 product='/WtUftCore/TraderAdapter.cpp' if group=='uft' else '/WtCore/TraderAdapter.cpp'
 testpath='TestUnits/coverage/q04_uftcore3/q04_trader_entrust_paths_test.cpp' if group=='uft' else 'TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_buy_test.cpp'
 productpath='WtUftCore/TraderAdapter.cpp' if group=='uft' else 'WtCore/TraderAdapter.cpp'
 testline=next(s for s in lines if ' -c ' in s and s.endswith(testcase));prodline=next(s for s in lines if ' -c ' in s and s.endswith(product) and target+'.dir' in s)
 link=next(s for s in lines if s.startswith(': && /usr/bin/c++') and f' -o build_x64/Debug/bin/TestUnits/{target} ' in s)
 for label,line,path in [('test',testline,testpath),('prod',prodline,productpath)]:
  a=shlex.split(line);a[-1]=str(src/path);a[1:1]=['-I'+str(src)];out=root/(group+'-'+label+'.o')
  for f in ('-MT','-MF','-o'):a[a.index(f)+1]=str(out)+('.d' if f=='-MF' else '')
  cmds.append((group+'-'+label,str(build),a))
 a=shlex.split(link.removeprefix(': && ').removesuffix(' && :'))
 for i,v in enumerate(a):
  if v.endswith(testcase+'.o'):a[i]=str(root/(group+'-test.o'))
  elif v.endswith(product+'.o'):a[i]=str(root/(group+'-prod.o'))
 a[a.index('-o')+1]=str(root/(group+'-probe'));cmds.append((group+'-link',str(build),a))
(root/'commands.json').write_text(json.dumps(cmds,indent=2))
for label,cwd,a in cmds:
 p=subprocess.run(a,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(root/(label+'.log')).write_text(p.stdout+'\nEXIT='+str(p.returncode)+'\n');print(label,p.returncode,flush=True)
 if p.returncode:sys.exit(p.returncode)
