import json,os,pathlib,subprocess
base=pathlib.Path('/tmp/wt-i03-order-ledger-20261102');ev=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/R3-008-Q04-002-20261102');names=['q04_trader_entrust.duplicate_trade_does_not_reduce_undone_twice','q04_trader_entrust.duplicate_cancellation_does_not_reduce_undone_twice','q04_trader_entrust.older_order_snapshot_does_not_undo_live_trade'];results=[]
for i,name in enumerate(names):
 wd=base/f'events-run{i}';wd.mkdir(exist_ok=True);cmd=[str(base/'focused'),'--gtest_filter='+name,'--gtest_output=xml:'+str(ev/f'events-run{i}.xml')];env=os.environ.copy();env['TMPDIR']=str(wd);env['LLVM_PROFILE_FILE']=str(wd/'profile-%p.profraw')
 r=subprocess.run(cmd,cwd=wd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30);(ev/f'events-run{i}.log').write_text(r.stdout+'\nEXIT='+str(r.returncode)+'\n');results.append({'test':name,'command':cmd,'cwd':str(wd),'exit':r.returncode});print(name,r.returncode,flush=True)
(ev/'events-results.json').write_text(json.dumps(results,indent=2))
