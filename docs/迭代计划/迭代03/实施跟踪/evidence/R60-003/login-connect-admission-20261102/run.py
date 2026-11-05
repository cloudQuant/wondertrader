import json, os, subprocess, sys
from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
out=Path('/tmp/wt-i03-r60-login-connect')
ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/login-connect-admission-20261102'
phase=sys.argv[1]
tests=sys.argv[2:]
results=[]
for name in tests:
    xml=ev/(phase+'-'+name+'.xml')
    argv=[str(out/'focused'),'--gtest_filter=R60MockerTest.'+name,'--gtest_output=xml:'+str(xml)]
    env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/(phase+'-'+name+'.profraw'))
    try:
        p=subprocess.run(argv,cwd=out,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=25,env=env,text=True)
        output=p.stdout; code=p.returncode
    except subprocess.TimeoutExpired as e:
        output=(e.stdout or b'').decode(errors='replace') if isinstance(e.stdout,bytes) else (e.stdout or '')
        code='timeout'
    (ev/(phase+'-'+name+'.log')).write_text(output)
    results.append({'argv':argv,'cwd':str(out),'exit':code})
(ev/(phase+'-run.json')).write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
