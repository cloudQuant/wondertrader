import json,shlex,subprocess,sys
from pathlib import Path
out=Path('/tmp/wt-i03-r60-lifecycle'); build=Path('/tmp/ctpm-i03-build'); repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
lines=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_t_TraderMocker'],text=True).splitlines()
product=next(l for l in lines if '/__/__/TraderMocker/TraderMocker.cpp.o ' in l and ' -c ' in l)
test=next(l for l in lines if '/r60_tradermocker_async_test.cpp.o ' in l and ' -c ' in l)
link=next(l for l in lines if '-o build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderMocker ' in l)
commands=[]
for line,kind in [(product,'product'),(test,'test')]:
 a=shlex.split(line); i=a.index('-o');a[i+1]=str(out/(kind+'-red.o'));i=a.index('-MF');a[i+1]=str(out/(kind+'.d'))
 
 if kind == "product":
  a[-1]=str(out/"red-current-header.cpp");a.insert(1, "-I/Users/yunjinqi/Documents/new_projects/wondertrader/src/TraderMocker")
 commands.append((kind,a))
a=shlex.split(link.replace(': && ','').replace(' && :',''));a=[x for x in a if not (x.endswith('.o') and 'CoverageTests_t_TraderMocker.dir' in x)];i=a.index('-o');a[i:i]=[str(out/'test-red.o'),str(out/'product-red.o')];a[a.index('-o')+1]=str(out/'focused-red');commands.append(('link',a))
results=[]
for kind,a in commands:
 with (out/(kind+'-red-build.log')).open('w') as f:
  p=subprocess.run(a,cwd=build,stdout=f,stderr=subprocess.STDOUT)
 results.append({'kind':kind,'argv':a,'cwd':str(build),'exit':p.returncode})
 if p.returncode: break
(out/'commands-red.json').write_text(json.dumps(results,indent=2)+'\n');sys.exit(results[-1]['exit'])
