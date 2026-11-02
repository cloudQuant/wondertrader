import json,shlex,subprocess,sys
from pathlib import Path
out=Path('/tmp/wt-i03-r60-lifecycle');build=Path('/tmp/ctpm-i03-build')
lines=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','CoverageTests_t_TraderMocker'],text=True).splitlines()
test=next(l for l in lines if '/t6_tradermocker_reachable_test.cpp.o ' in l and ' -c ' in l)
link=next(l for l in lines if '-o build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderMocker ' in l)
a=shlex.split(test);a[a.index('-o')+1]=str(out/'t6.o');a[a.index('-MF')+1]=str(out/'t6.d')
b=shlex.split(link.replace(': && ','').replace(' && :',''));b=[x for x in b if not (x.endswith('.o') and 'CoverageTests_t_TraderMocker.dir' in x)];i=b.index('-o');b[i:i]=[str(out/'t6.o'),str(out/'product.o')];b[b.index('-o')+1]=str(out/'t6-focused')
results=[]
for kind,cmd in [('test',a),('link',b)]:
 with (out/('t6-'+kind+'-build.log')).open('w') as f:p=subprocess.run(cmd,cwd=build,stdout=f,stderr=subprocess.STDOUT)
 results.append({'kind':kind,'argv':cmd,'cwd':str(build),'exit':p.returncode})
 if p.returncode: break
(out/'t6-commands.json').write_text(json.dumps(results,indent=2)+'\n');sys.exit(results[-1]['exit'])
