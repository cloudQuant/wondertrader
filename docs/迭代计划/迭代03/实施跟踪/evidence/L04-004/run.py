import os, pathlib, subprocess, json
p=pathlib.Path('/tmp/wt-i03-l04-contract')
results={}
for mode in ('red','green'):
 tmp=p/('tmp_'+mode); tmp.mkdir(exist_ok=True)
 env=dict(os.environ,TMPDIR=str(tmp)+'/',LLVM_PROFILE_FILE=str(p/(mode+'-%p.profraw')))
 cmd=[str(p/mode),'--gtest_filter=l03_contract.storage_width_contract_limits_preserve_valid_edges_and_reject_before_rules_registration:l03_contract.oversized_configured_identity_is_rejected_before_registration','--gtest_color=no']
 try:
  r=subprocess.run(cmd,env=env,cwd=p,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
  (p/(mode+'.log')).write_text(r.stdout)
  results[mode]={'command':cmd,'exit':r.returncode,'tail':'\n'.join(r.stdout.splitlines()[-15:])}
 except subprocess.TimeoutExpired as e:
  results[mode]={'timeout':True,'stdout':str(e.stdout)}
print(json.dumps(results,indent=2))
(p/'results.json').write_text(json.dumps(results,indent=2))
