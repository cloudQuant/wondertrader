import os, subprocess, pathlib
out=pathlib.Path('docs/迭代计划/迭代03/实施跟踪/evidence/NEW-034-monitor')
results=[]
for kind in ('monitor','monitor-tsan'):
 for case in ('concurrent','self'):
  env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1',TSAN_OPTIONS='halt_on_error=1')
  for i in range(10):
   try:
    p=subprocess.run([f'/tmp/wt-i03-new034/green/{kind}',case],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=8,env=env)
    result=str(p.returncode); output=p.stdout.decode(errors='replace')
   except subprocess.TimeoutExpired as e:
    result='TIMEOUT'; output=(e.stdout or b'').decode(errors='replace')
   results.append(f'{kind} {case} {i+1} exit={result}\n{output}')
   if result!='0': print(kind,case,i+1,result)
(out/'repeat-results.log').write_text('\n'.join(results))
print('runs=',len(results),'passes=',sum('exit=0\n' in x for x in results))
