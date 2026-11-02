import os,shlex,subprocess,json,hashlib,pathlib,sys
if len(sys.argv) != 3:
 raise SystemExit('usage: run.py <existing-ninja-build-dir> <new-output-dir>')
fixture=pathlib.Path(__file__).resolve().parent
root=fixture.parents[3]
build=pathlib.Path(sys.argv[1]).resolve()
out=pathlib.Path(sys.argv[2]).resolve(); logs=[]

def run(label,cmd,cwd=build,expect=0):
 p=subprocess.run(cmd,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (out/(label+'.log')).write_text(p.stdout); logs.append({'label':label,'cwd':str(cwd),'argv':cmd,'exit':p.returncode})
 print(label,p.returncode,flush=True)
 if p.returncode != expect: raise SystemExit(label+' failed')

def rewrite(arg): return arg.replace('/tmp/wt-i02-fresh-20261101-v8-src',str(root))

out.mkdir(parents=True,exist_ok=False)
libdir=out/'module'; libdir.mkdir(exist_ok=True)
run('fake-build',['/usr/bin/c++','-std=c++17','-dynamiclib',str(fixture/'fake_module.cpp'),'-o',str(libdir/'ctpopt_path_fixture.dylib')],root)
for kind in ('ParserCTPOpt','TraderCTPOpt'):
 target='CoverageTests_t_'+kind
 raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands',target],text=True).splitlines()
 product_line=next(l for l in raw if ' -c ' in l and l.endswith('/'+kind+'/'+kind+'.cpp'))
 args=[rewrite(x) for x in shlex.split(product_line)]
 # Replace system include directory with real Cellar include roots.
 i=args.index('-isystem');
 while i < len(args)-1:
  if args[i]=='-isystem' and args[i+1]=='/opt/homebrew/include': args[i:i+2]=['-isystem','/opt/homebrew/Cellar/fmt/11.2.0/include','-isystem','/opt/homebrew/Cellar/spdlog/1.15.3/include'];break
  i+=1
 args += ['-Ddladdr=wt_path_test_dladdr','-Ddlopen=wt_path_test_dlopen','-I'+str(root/'src'/kind)]
 for variant in ('current',):
  cmd=args.copy();cmd[cmd.index('-o')+1]=str(out/(kind+'-'+variant+'.o'));cmd[cmd.index('-MF')+1]=str(out/(kind+'-'+variant+'.d'));cmd[cmd.index('-MT')+1]=str(out/(kind+'-'+variant+'.o'))
  run(kind+'-'+variant,cmd)
 # compile test fixture using same include and flags, without loader mock macros
 cmd=[x for x in args if x not in ('-Ddladdr=wt_path_test_dladdr','-Ddlopen=wt_path_test_dlopen')]
 if kind.startswith('Trader'):cmd+=['-DWT_PATH_TEST_TRADER']
 cmd[cmd.index('-o')+1]=str(out/(kind+'-test.o'));cmd[cmd.index('-MF')+1]=str(out/(kind+'-test.d'));cmd[cmd.index('-MT')+1]=str(out/(kind+'-test.o'));cmd[cmd.index('-c')+1]=str(fixture/'path_check.cpp')
 run(kind+'-test',cmd)
 line=shlex.split(raw[-1]); line=line[2:-2] # ': &&' and '&& :'
 line=[rewrite(x) for x in line]
 line=[x for x in line if not (x.endswith('.o') and 'CoverageTests_t_' in x)]
 for variant in ('current',):
  cmd=line.copy();cmd[cmd.index('-o')+1]=str(out/(kind+'-'+variant));cmd[1:1]=[str(out/(kind+'-'+variant+'.o')),str(out/(kind+'-test.o'))]
  run(kind+'-'+variant+'-link',cmd)
  for mode in ('empty','absolute','valid'):
   label=kind+'-'+variant+'-'+mode; rundir=out/label;rundir.mkdir(exist_ok=True)
   env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(rundir/'profile.profraw')
   proc=subprocess.run([str(out/(kind+'-'+variant)),mode,str(libdir)],cwd=rundir,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
   (out/(label+'.log')).write_text(proc.stdout); logs.append({'label':label,'argv':[str(out/(kind+'-'+variant)),mode,str(libdir)],'cwd':str(rundir),'exit':proc.returncode})
   expected=0
   print(label,proc.returncode,flush=True)
   if proc.returncode!=expected: raise SystemExit(label+' unexpected')
files=[root/'src/ParserCTPOpt/ParserCTPOpt.cpp',root/'src/TraderCTPOpt/TraderCTPOpt.cpp',root/'src/Share/ModuleHelper.hpp',fixture/'path_check.cpp',fixture/'fake_module.cpp']
(out/'hashes.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},indent=2)+'\n')
(out/'commands.json').write_text(json.dumps(logs,indent=2)+'\n')
