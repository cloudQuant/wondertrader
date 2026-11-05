import json,pathlib,subprocess,sys
repo=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader');ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102';out=pathlib.Path('/tmp/wt-i03-owner-delivery-20261102');build=pathlib.Path('/tmp/wt-i03-r42-003-20261102/build');mode=sys.argv[1];assert mode in ('address,undefined','thread');key='asan' if mode.startswith('address') else 'tsan';compiler='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++';cmds=json.loads((ev/'green-commands.json').read_text())
for i,args in enumerate(cmds):
 args[0]=compiler;args[1:1]=['-I/usr/local/include','-include','stdlib.h','-fsanitize='+mode,'-fno-omit-frame-pointer'];args[:]=[x.replace('/green-test.o','/'+key+'-test.o').replace('/green-prod.o','/'+key+'-prod.o').replace('/green-focused','/'+key+'-focused') for x in args];args[:]=[x for x in args if x not in ('-Werror','-Wl,-no_warn_duplicate_libraries','-Wl,-fatal_warnings')]
(ev/(key+'-commands.json')).write_text(json.dumps(cmds,indent=2))
for i,args in enumerate(cmds):
 try:r=subprocess.run(args,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180);code=r.returncode;output=r.stdout
 except subprocess.TimeoutExpired as e:code='TIMEOUT';output=str(e)
 (ev/f'{key}-build-{i}.log').write_text(output+'\nEXIT='+str(code)+'\n');print(key,'build',i,code,flush=True)
 if code!=0:sys.exit(1)
