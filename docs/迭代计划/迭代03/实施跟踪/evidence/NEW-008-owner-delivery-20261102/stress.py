import json,os,pathlib,subprocess
repo=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader');ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102';out=pathlib.Path('/tmp/wt-i03-owner-delivery-20261102');cases=[x['test'] for x in json.loads((ev/'green-results.json').read_text())][:4];results=[]
for key in ('green','tsan'):
 for case in cases:
  for i in range(10):
   try:
    r=subprocess.run([str(out/f'{key}-focused'),'--gtest_filter='+case],cwd=out,capture_output=True,text=True,env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1'),timeout=20);code=r.returncode;output=r.stdout+r.stderr
   except subprocess.TimeoutExpired as e:code='TIMEOUT';output=str(e)
   results.append({'mode':key,'test':case,'iteration':i,'exit':code})
   if code!=0:(ev/f'stress-failure-{key}-{case.split(".")[-1]}-{i}.log').write_text(output+'\nexit='+str(code)+'\n')
(ev/'stress-results.json').write_text(json.dumps(results,indent=2)+'\n');print('total',len(results),'passed',sum(x['exit']==0 for x in results),'failures',sum(x['exit']!=0 for x in results))
