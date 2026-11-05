import json, subprocess, sys
from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
out=Path('/tmp/wt-i03-r60-login-connect')
ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/login-connect-admission-20261102'
phase=sys.argv[1]
orig=json.loads((repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/admission/commands.json').read_text())
results=[]
for entry in orig:
    argv=[a.replace('/tmp/wt-i03-r60-lifecycle/', str(out)+'/') for a in entry['argv']]
    kind=entry['kind']
    with (ev/(phase+'-'+kind+'-build.log')).open('w') as f:
        p=subprocess.run(argv,cwd=entry['cwd'],stdout=f,stderr=subprocess.STDOUT)
    results.append({'kind':kind,'argv':argv,'cwd':entry['cwd'],'exit':p.returncode})
    if p.returncode: break
(ev/(phase+'-commands.json')).write_text(json.dumps(results,indent=2)+'\n')
sys.exit(results[-1]['exit'])
