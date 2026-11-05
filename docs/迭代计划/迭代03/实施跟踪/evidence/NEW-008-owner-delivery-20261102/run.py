import json, pathlib, subprocess,sys
repo=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader');ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102'; out=pathlib.Path('/tmp/wt-i03-owner-delivery-20261102');mode=sys.argv[1];assert mode in ('red','green')
cases=['DiffOwnerSinkDelivery.UnregisterBeforeCallbackReportsNotDelivered','DiffOwnerSinkDelivery.RemoveBeforeCallbackReportsNotDelivered','DiffOwnerSinkDelivery.StopBeforeCallbackReportsNotDelivered','DiffOwnerSinkDelivery.ReplacementCannotReceiveOldReservation','DiffOwnerSinkDelivery.CallbackExceptionPropagatesAndReleasesReservation','DiffOwnerSinkRegistry.DisabledByDefaultAndRoutesOnlyMatchingOwner','DiffOwnerSinkRegistry.ConcurrentRemoveWaitsForOwnerCallback','DiffOwnerSinkRegistry.ReentrantUnregisterAndRemoveDoNotHoldRoutingLocks'];results=[]
for i,case in enumerate(cases):
 cwd=out/f'{mode}-run{i}';cwd.mkdir(parents=True,exist_ok=True);argv=[str(out/f'{mode}-focused'),'--gtest_filter='+case,'--gtest_output=xml:'+str(ev/f'{mode}-run{i}.xml')]
 try:
  r=subprocess.run(argv,cwd=cwd,capture_output=True,text=True,timeout=20);code=r.returncode;text=r.stdout+r.stderr
 except subprocess.TimeoutExpired as e:code='TIMEOUT';text=str(e)
 (ev/f'{mode}-run{i}.log').write_text(text+'\nexit='+str(code)+'\n');results.append({'test':case,'argv':argv,'cwd':str(cwd),'exit':code});print(mode,case,code,flush=True)
(ev/f'{mode}-results.json').write_text(json.dumps(results,indent=2)+'\n')
