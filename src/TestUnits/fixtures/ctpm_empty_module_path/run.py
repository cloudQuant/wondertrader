import os,shlex,subprocess,json,hashlib,pathlib,sys
if len(sys.argv) not in (3,4): raise SystemExit('usage: run.py <existing-ninja-build-dir> <new-output-dir> [baseline-source-dir]')
fixture=pathlib.Path(__file__).resolve().parent
root=fixture.parents[3];build=pathlib.Path(sys.argv[1]).resolve();out=pathlib.Path(sys.argv[2]).resolve();baseline=pathlib.Path(sys.argv[3]).resolve() if len(sys.argv)==4 else None
out.mkdir(parents=True,exist_ok=True); records=json.loads((out/'commands.json').read_text()) if (out/'commands.json').exists() else []
def run(label,cmd,cwd=build,expect=0,env=None):
 if any(r['label']==label and r['exit']==expect for r in records):
  print(label,'reused successful run',flush=True);return
 try:
  p=subprocess.run(cmd,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180,env=env);rc=p.returncode;output=p.stdout
 except subprocess.TimeoutExpired as e:
  rc='TIMEOUT';output=(e.stdout or b'').decode(errors='replace') if isinstance(e.stdout,bytes) else (e.stdout or '')
 (out/(label+'.log')).write_text(output);records.append({'label':label,'cwd':str(cwd),'argv':cmd,'exit':rc})
 (out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
 print(label,rc,flush=True)
 if rc!=expect: raise SystemExit(label+' unexpected exit')
def rewrite(arg): return arg.replace('/tmp/wt-i02-fresh-20261101-v8-src',str(root))
libdir=out/'module';libdir.mkdir(exist_ok=True)
for name in ('libwt_ctpm_path_fake.dylib','wt_ctpm_path_fake.dylib'):
 run('fake-'+name,['/usr/bin/c++','-std=c++17','-dynamiclib',str(fixture/'fake_module.cpp'),'-o',str(libdir/name)],root)
inputs=[root/'src/Share/ModuleHelper.hpp',fixture/'path_check.cpp',fixture/'fake_module.cpp']
for kind,testfile,testcase in [('ParserCTPMini','r105_parserctpmini_api_test.cpp','R105MiniTest.injected_api_connect_login_and_do_subscribe'),('TraderCTPMini','r61_traderctpmini_api_test.cpp','R61_TraderCTPMini.connect_subscribes_topics_and_disconnect_releases_api')]:
 target='CoverageTests_t_'+kind
 raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands',target],text=True).splitlines()
 line=next(l for l in raw if ' -c ' in l and l.endswith('/'+kind+'/'+kind+'.cpp'))
 args=[rewrite(x) for x in shlex.split(line)]
 while '-isystem' in args:
  idx=args.index('-isystem')
  if args[idx+1]=='/opt/homebrew/include':
   args[idx:idx+2]=['-I/opt/homebrew/Cellar/fmt/11.2.0/include','-I/opt/homebrew/Cellar/spdlog/1.15.3/include'];break
  break
 args+=['-I'+str(root/'src'/kind)]
 source=root/'src'/kind/(kind+'.cpp');inputs.append(source)
 if baseline:
  original=(baseline/(kind+'.cpp')).read_text(encoding='utf-8-sig')
  original=original.replace('#include "../','#include "'+str(root/'src')+'/')
  tmp=out/(kind+'-baseline.cpp');tmp.write_text(original,encoding='utf-8');src=tmp
 else: src=source
 def compile(label,flags,sourcefile):
  cmd=args.copy();cmd[cmd.index('-o')+1]=str(out/(label+'.o'));cmd[cmd.index('-MF')+1]=str(out/(label+'.d'));cmd[cmd.index('-MT')+1]=str(out/(label+'.o'));cmd[cmd.index('-c')+1]=str(sourcefile);cmd+=flags;run(label,cmd)
 compile(kind+'-product-spy',['-Ddladdr=wt_ctpm_dladdr','-Ddlopen=wt_ctpm_dlopen'],src)
 compile(kind+'-product-production',[],src)
 flags=['-DWT_PATH_TEST_TRADER'] if kind.startswith('Trader') else []
 compile(kind+'-fixture',flags,fixture/'path_check.cpp')
 link=shlex.split(raw[-1])[2:-2];link=[rewrite(x) for x in link];link=[x for x in link if not (x.endswith('.o') and target in x)]
 def do_link(label,objs):
  cmd=link.copy();cmd[cmd.index('-o')+1]=str(out/label);cmd[1:1]=[str(out/o) for o in objs];run(label+'-link',cmd)
 do_link(kind+'-path',[kind+'-product-spy.o',kind+'-fixture.o'])
 for mode in ('empty','valid','absolute'):
  run_dir=out/(kind+'-'+mode);run_dir.mkdir(exist_ok=True);env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(run_dir/'profile.profraw')
  run(kind+'-'+mode,[str(out/(kind+'-path')),mode,str(libdir)],run_dir,42 if baseline and mode=='empty' else 0,env)
 if not baseline:
  line=next(l for l in raw if ' -c ' in l and l.endswith('/'+testfile))
  ta=[rewrite(x) for x in shlex.split(line)];
  for i in range(len(ta)-1):
   if ta[i]=='-isystem' and ta[i+1]=='/opt/homebrew/include':ta[i:i+2]=['-I/opt/homebrew/Cellar/fmt/11.2.0/include','-I/opt/homebrew/Cellar/spdlog/1.15.3/include'];break
  # Treat third-party GTest headers as system headers, as the CMake target does.
  # Product and test translation units still retain their existing -Werror.
  ta+=['-isystem','/opt/homebrew/Cellar/googletest/1.17.0/include']
  testsrc=root/'src/TestUnits/coverage'/('t_'+kind)/testfile;inputs.append(testsrc)
  ta[ta.index('-o')+1]=str(out/(kind+'-test.o'));ta[ta.index('-MF')+1]=str(out/(kind+'-test.d'));ta[ta.index('-MT')+1]=str(out/(kind+'-test.o'));run(kind+'-test',ta)
  do_link(kind+'-control',[kind+'-product-production.o',kind+'-test.o'])
  run_dir=out/(kind+'-control-run');run_dir.mkdir(exist_ok=True);env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(run_dir/'profile.profraw')
  run(kind+'-control',[str(out/(kind+'-control')),'--gtest_filter='+testcase,'--gtest_output=xml:'+str(run_dir/'result.xml')],run_dir,0,env)
(out/'hashes.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},indent=2)+'\n')
