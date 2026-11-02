import os,shlex,subprocess,json,sys,pathlib
root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); build=pathlib.Path('/tmp/ctpm-i03-build'); out=pathlib.Path('/tmp/wt-i03-parser-run'); mode=sys.argv[1]; (out/mode).mkdir(exist_ok=True)
configs=[('wtcore','n01_wtcore_tradepaths/n01_parser_adapter_test.cpp','WtCore'),('dtcore','q05_dtsrv/r18_parser_adapter_test.cpp','WtDtCore'),('dtservo','q05b_dtservo/r45_parseradapter_test.cpp','WtDtServo')]
records=[]
def query(t):
 p=subprocess.run(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands',t],capture_output=True,text=True,check=True)
 return p.stdout.strip().splitlines()[-1]
def run(args,tag):
 p=subprocess.run(args,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (out/mode/(tag+'.log')).write_text(p.stdout); records.append({'tag':tag,'cwd':str(build),'argv':args,'exit':p.returncode}); print(tag,p.returncode,flush=True)
 if p.returncode: (out/mode/'commands.json').write_text(json.dumps(records,indent=2));sys.exit(p.returncode)
for target,test_rel,product in configs:
 targetdir=f'TestUnits/coverage/CMakeFiles/CoverageTests_{target}.dir/'
 selected=[('product',targetdir+f'__/__/{product}/ParserAdapter.cpp.o',out/'baseline'/f'{product}-ParserAdapter.cpp' if mode=='red' else root/'src'/product/'ParserAdapter.cpp'),('test',targetdir+test_rel+'.o',root/'src/TestUnits/coverage'/test_rel)]
 for part,obj,source in selected:
  a=shlex.split(query(obj)); dest=out/mode/(target+'-'+part+'.o')
  for flag in ['-o','-MF','-MT']:
   if flag in a:a[a.index(flag)+1]=str(dest if flag=='-o' else dest.with_suffix('.d') if flag=='-MF' else dest)
  if '-c' in a:a[a.index('-c')+1]=str(source)
  a[1:1]=['-I',str(root/'src'/product)]
  run(a,target+'-'+part)
 raw=query('CoverageTests_'+target)
 assert raw.startswith(': && ') and raw.endswith(' && :'),raw[-200:]
 a=shlex.split(raw[5:-5]); a=[x for x in a if not x.endswith('.o')]
 i=a.index('-o');a[i+1]=str(out/mode/(target+'-tests'))
 # Objects must precede static libraries for correct extraction.
 a[1:1]=[str(out/mode/(target+'-product.o')),str(out/mode/(target+'-test.o'))]
 run(a,target+'-link')
(out/mode/'commands.json').write_text(json.dumps(records,indent=2))
