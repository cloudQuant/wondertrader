import json,subprocess,sys
from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); out=Path('/tmp/wt-i03-r60-login-connect'); ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/login-connect-admission-20261102'
orig=json.loads((ev/'red-commands.json').read_text()); results=[]
for entry in orig[1:]:
 argv=[a.replace(str(out/'product.o'),str(out/'red-product.o')) for a in entry['argv']]
 with (ev/('red2-'+entry['kind']+'-build.log')).open('w') as f:
  p=subprocess.run(argv,cwd=entry['cwd'],stdout=f,stderr=subprocess.STDOUT)
 results.append({'kind':entry['kind'],'argv':argv,'cwd':entry['cwd'],'exit':p.returncode})
 if p.returncode: break
(ev/'red2-commands.json').write_text(json.dumps(results,indent=2)+'\n');sys.exit(results[-1]['exit'])
