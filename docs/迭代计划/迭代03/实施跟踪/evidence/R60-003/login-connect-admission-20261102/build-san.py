import json,subprocess,sys
from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); out=Path('/tmp/wt-i03-r60-login-connect'); ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/login-connect-admission-20261102'
orig=json.loads((ev/'green-commands.json').read_text()); results=[]
for entry in orig:
 argv=[a.replace(str(out/'product.o'),str(out/'san-product.o')).replace(str(out/'test.o'),str(out/'san-test.o')).replace(str(out/'focused'),str(out/'focused-san')) for a in entry['argv']]
 argv[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 with (ev/('san-'+entry['kind']+'-build.log')).open('w') as f: p=subprocess.run(argv,cwd=entry['cwd'],stdout=f,stderr=subprocess.STDOUT)
 results.append({'kind':entry['kind'],'argv':argv,'cwd':entry['cwd'],'exit':p.returncode})
 if p.returncode: break
(ev/'san-commands.json').write_text(json.dumps(results,indent=2)+'\n');sys.exit(results[-1]['exit'])
