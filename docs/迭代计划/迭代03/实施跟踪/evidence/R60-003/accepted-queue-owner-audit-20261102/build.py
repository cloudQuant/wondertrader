import json, subprocess, sys
from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); out=Path('/tmp/wt-i03-r60-queue-audit'); old=Path('/tmp/wt-i03-r60-login-connect'); ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/accepted-queue-owner-audit-20261102'
orig=json.loads((repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/login-connect-admission-20261102/final-commands.json').read_text()); results=[]
for e in orig:
 argv=[a.replace(str(old)+'/',str(out)+'/') for a in e['argv']]
 if e['kind']=='test': argv[-1]=str(out/'test.cpp')
 with (ev/(e['kind']+'-build.log')).open('w') as f: p=subprocess.run(argv,cwd=e['cwd'],stdout=f,stderr=subprocess.STDOUT)
 results.append({'kind':e['kind'],'argv':argv,'cwd':e['cwd'],'exit':p.returncode})
 if p.returncode: break
(ev/'build-commands.json').write_text(json.dumps(results,indent=2)+'\n');sys.exit(results[-1]['exit'])
