import json, os, subprocess
from pathlib import Path
repo=Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); out=Path('/tmp/wt-i03-r60-queue-audit'); ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/R60-003/accepted-queue-owner-audit-20261102'; results=[]
for name in ['audit_accepted_queue_without_stop_control','audit_accepted_queue_stop_does_not_lose_results','release_reentered_from_connect_callback_does_not_terminate']:
 argv=[str(out/'focused'),'--gtest_filter=R60MockerTest.'+name,'--gtest_output=xml:'+str(ev/(name+'.xml'))]
 env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/(name+'.profraw'))
 try:
  p=subprocess.run(argv,cwd=out,env=env,capture_output=True,text=True,timeout=20);rc=p.returncode;stdout=p.stdout;stderr=p.stderr
 except subprocess.TimeoutExpired as e:
  rc='timeout';stdout='';stderr=str(e)
 (ev/(name+'.log')).write_text(stdout+stderr);results.append({'argv':argv,'cwd':str(out),'exit':rc})
(ev/'run.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
