import json, os, pathlib, subprocess
base=pathlib.Path('/tmp/wt-i03-order-ledger-20261102')
evidence=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/R3-008-Q04-002-20261102')
names=['q04_trader_entrust.insert_failure_returns_uint_max_without_counting_undone','q04_trader_entrust.duplicate_rejection_does_not_consume_another_live_order','q04_trader_entrust.reentrant_rejection_and_negative_return_leave_other_order_unchanged','q04_trader_entrust.four_direction_actions_are_assembled_correctly']
results=[]
for i,name in enumerate(names):
    wd=base/f'run{i}';wd.mkdir(exist_ok=True)
    xml=evidence/f'run{i}.xml'; cmd=[str(base/'focused'),'--gtest_filter='+name,'--gtest_output=xml:'+str(xml)]
    env=os.environ.copy();env['TMPDIR']=str(wd);env['LLVM_PROFILE_FILE']=str(wd/'profile-%p.profraw')
    try:
        r=subprocess.run(cmd,cwd=wd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
        output=r.stdout; code=r.returncode
    except subprocess.TimeoutExpired as e:
        output=(e.stdout or b'').decode(errors='replace') if isinstance(e.stdout,bytes) else e.stdout or '';code='TIMEOUT'
    (evidence/f'run{i}.log').write_text(output+'\nEXIT='+str(code)+'\n')
    results.append({'test':name,'command':cmd,'cwd':str(wd),'exit':code})
    print(name,code,flush=True)
(evidence/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2))
