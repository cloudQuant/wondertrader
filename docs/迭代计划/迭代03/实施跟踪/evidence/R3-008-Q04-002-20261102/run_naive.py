import json, os, pathlib, subprocess
base=pathlib.Path('/tmp/wt-i03-order-ledger-20261102'); evidence=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/R3-008-Q04-002-20261102')
names=['q04_trader_entrust.insert_failure_returns_uint_max_without_counting_undone','q04_trader_entrust.reentrant_rejection_and_negative_return_leave_other_order_unchanged','q04_trader_entrust.duplicate_rejection_does_not_consume_another_live_order']
results=[]
for i,name in enumerate(names):
 wd=base/f'naive-run{i}';wd.mkdir(exist_ok=True); cmd=[str(base/'naive'),'--gtest_filter='+name,'--gtest_output=xml:'+str(evidence/f'naive-run{i}.xml')];env=os.environ.copy();env['TMPDIR']=str(wd);env['LLVM_PROFILE_FILE']=str(wd/'profile-%p.profraw')
 r=subprocess.run(cmd,cwd=wd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30);(evidence/f'naive-run{i}.log').write_text(r.stdout+'\nEXIT='+str(r.returncode)+'\n');results.append({'test':name,'command':cmd,'cwd':str(wd),'exit':r.returncode});print(name,r.returncode,flush=True)
(evidence/'naive-results.json').write_text(json.dumps(results,indent=2))
