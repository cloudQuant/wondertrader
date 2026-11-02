import hashlib,json,os,pathlib,shlex,subprocess,sys
if len(sys.argv)!=4: raise SystemExit('usage: run_unload.py <ninja-build> <product-source> <new-output-directory>')
fixture=pathlib.Path(__file__).resolve().parent;root=fixture.parents[3]
build=pathlib.Path(sys.argv[1]).resolve();source=pathlib.Path(sys.argv[2]).resolve();out=pathlib.Path(sys.argv[3]).resolve();baseline=source!=(root/'src/ParserCTPMini/ParserCTPMini.cpp').resolve()
out.mkdir(parents=True,exist_ok=False);mod=out/'module';mod.mkdir();records=[]
def run(label,argv,cwd,env=None):
 try:
  p=subprocess.run(argv,cwd=cwd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120);status=p.returncode;output=p.stdout
 except subprocess.TimeoutExpired as exc:
  status='TIMEOUT';output=exc.stdout or '';output=output.decode(errors='replace') if isinstance(output,bytes) else output
 (out/(label+'.log')).write_text(output);records.append({'label':label,'cwd':str(cwd),'argv':argv,'exit':status});(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n');print(label,status,flush=True);return status
for mode in ('missing','null'):
 cmd=['/usr/bin/c++','-std=c++17','-dynamiclib','-I'+str(root/'src'),str(fixture/'fake_vendor.cpp'),'-o',str(mod/('libfake_'+mode+'.dylib'))]
 if mode=='null':cmd.append('-DWT_MINI_NULL_CREATOR')
 if run('fake-'+mode,cmd,root)!=0:raise SystemExit('fake build failed')
raw=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_t_ParserCTPMini'],text=True).splitlines()
prod=shlex.split(next(line for line in raw if ' -c ' in line and line.endswith('/ParserCTPMini/ParserCTPMini.cpp')));prod+=['-I'+str(root/'src/ParserCTPMini')]
if baseline:
 content=source.read_text(encoding='utf-8-sig').replace('#include "../','#include "'+str(root/'src')+'/');source=out/'baseline.cpp';source.write_text(content)
def compiled(label,src,spy):
 args=prod.copy()
 for opt,val in [('-o',out/(label+'.o')),('-MF',out/(label+'.d')),('-MT',out/(label+'.o')),('-c',src)]:args[args.index(opt)+1]=str(val)
 if spy:args.append('-Ddladdr=wt_mini_real_dladdr')
 if run(label,args,build)!=0:raise SystemExit(label+' failed')
compiled('product',source,True);compiled('fixture',fixture/'unload_check.cpp',False)
link=shlex.split(raw[-1])[2:-2];link=[x for x in link if not (x.endswith('.o') and 'CoverageTests_t_ParserCTPMini' in x)];link[link.index('-o')+1]=str(out/'focused');link[1:1]=[str(out/'product.o'),str(out/'fixture.o')]
if run('link',link,build)!=0:raise SystemExit('link failed')
bad=[]
for mode in ('missing-symbol','null-api'):
 wd=out/mode;wd.mkdir();env=os.environ.copy();marker=wd/'unload-marker';env.update({'WT_MINI_REAL_MODULE_DIR':str(mod),'WT_MINI_UNLOAD_MARKER':str(marker),'LLVM_PROFILE_FILE':str(wd/'profile.profraw')})
 status=run(mode,[str(out/'focused'),mode],wd,env);present=marker.exists();records[-1]['marker_after_exit']=present;(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
 if status!=(31 if baseline else 0):bad.append(mode)
(out/'inputs.sha256').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p) for p in (source,fixture/'unload_check.cpp',fixture/'fake_vendor.cpp',root/'src/Share/ModuleHelper.hpp'))+'\n')
if bad:raise SystemExit('Unexpected test outcome: '+', '.join(bad))
