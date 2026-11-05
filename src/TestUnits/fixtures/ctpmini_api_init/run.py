import hashlib,json,os,pathlib,shlex,subprocess,sys
if len(sys.argv)!=4: raise SystemExit('usage: run.py <ninja-build> <product-source> <new-output-directory>')
fixture=pathlib.Path(__file__).resolve().parent; root=fixture.parents[3]
build=pathlib.Path(sys.argv[1]).resolve(); source=pathlib.Path(sys.argv[2]).resolve(); out=pathlib.Path(sys.argv[3]).resolve()
baseline = source != (root/'src/ParserCTPMini/ParserCTPMini.cpp').resolve()
unload_baseline = source.name == 'unload-baseline.cpp'
connect_baseline = source.name == 'connect-baseline.cpp'
out.mkdir(parents=True,exist_ok=False); records=[]
def run(label,argv,cwd,env=None):
 try:
  p=subprocess.run(argv,cwd=cwd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120); status=p.returncode; output=p.stdout
 except subprocess.TimeoutExpired as exc:
  status='TIMEOUT';output=exc.stdout or '';output=output.decode(errors='replace') if isinstance(output,bytes) else output
 (out/(label+'.log')).write_text(output);records.append({'label':label,'cwd':str(cwd),'argv':argv,'exit':status});(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
 print(label,status,flush=True);return status
raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_t_ParserCTPMini'],text=True).splitlines()
prod=shlex.split(next(line for line in raw if ' -c ' in line and line.endswith('/ParserCTPMini/ParserCTPMini.cpp')))
prod+=['-I'+str(root/'src/ParserCTPMini')]
if baseline:
 content=source.read_text(encoding='utf-8-sig').replace('#include "../','#include "'+str(root/'src')+'/')
 source=out/'baseline.cpp';source.write_text(content)
def compiled(label,src,spies):
 args=prod.copy()
 for opt,val in [('-o',out/(label+'.o')),('-MF',out/(label+'.d')),('-MT',out/(label+'.o')),('-c',src)]:args[args.index(opt)+1]=str(val)
 if spies: args+=['-Ddladdr=wt_mini_init_dladdr','-Ddlopen=wt_mini_init_dlopen','-Ddlsym=wt_mini_init_dlsym','-Ddlclose=wt_mini_init_dlclose','-Ddlerror=wt_mini_init_dlerror']
 if run(label,args,build)!=0: raise SystemExit(label+' failed')
compiled('product',source,True);compiled('fixture',fixture/'init_check.cpp',False)
link=shlex.split(raw[-1])[2:-2];link=[x for x in link if not (x.endswith('.o') and 'CoverageTests_t_ParserCTPMini' in x)]
link[link.index('-o')+1]=str(out/'focused');link[1:1]=[str(out/'product.o'),str(out/'fixture.o')]
if run('link',link,build)!=0: raise SystemExit('link failed')
bad=[]
for mode in ('missing-library','missing-symbol','null-api','null-config','success','success-one'):
 wd=out/mode;wd.mkdir();env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(wd/'profile.profraw')
 status=run(mode,[str(out/'focused'),mode],wd,env)
 expected = (10,) if unload_baseline and mode in ('missing-symbol','null-api') else (16,) if (connect_baseline or unload_baseline) and mode in ('missing-library','missing-symbol','null-api','null-config') else (25,) if (connect_baseline or unload_baseline) and mode in ('success','success-one') else (-11, -10) if baseline and not unload_baseline and not connect_baseline and mode in ('missing-library','missing-symbol','null-api','null-config') else (21,) if baseline and not unload_baseline and not connect_baseline and mode == 'success' else (0,)
 if status not in expected: bad.append(mode)
(out/'inputs.sha256').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p) for p in (source,fixture/'init_check.cpp',root/'src/Share/ModuleHelper.hpp'))+'\n')
# K16=A (I03-NEW-003): cross-instance retry of the same failed module is allowed and isolated again.
if not baseline:
 wd=out/'cross-instance';wd.mkdir();env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(wd/'profile.profraw')
 if run('cross-instance',[str(out/'focused'),'cross-instance'],wd,env)!=0: bad.append('cross-instance')
# K53 (2026-11-06): a failure that loaded no module may be retried on the same instance.
# WT_K53_FORCE=1 also runs these modes for a supplied source (mutation red control).
if not baseline or os.environ.get('WT_K53_FORCE') == '1':
 for mode in ('retry-after-missing-library','retry-after-null-config'):
  wd=out/mode;wd.mkdir();env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(wd/'profile.profraw')
  if run(mode,[str(out/'focused'),mode],wd,env)!=0: bad.append(mode)
if bad: raise SystemExit('Unexpected test outcome: '+', '.join(bad))
