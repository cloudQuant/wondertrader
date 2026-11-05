from pathlib import Path
import concurrent.futures, json, os, subprocess, sys, time
root=Path(sys.argv[1]); env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1',TSAN_OPTIONS='halt_on_error=1')
results=[('delayed',0),('sync',0),('duplicate',0),('duplicate-instrument',0),('unknown',0),('error',1),('rsp-error',1),('late-error',1),('late-rsp-error',1),('empty',1),('disconnect',1),('timeout',1),('send-failure',1),('sync-contradiction',1),('instrument-failure',1),('blocked-send',1),('blocked-after-terminal',1),('wrong-code',1),('wrong-exchange',1),('conflicting-product',1)]
def run(test):
 variant,mode,expected=test
 case=root/(variant+'-query-'+mode);case.mkdir();file=case/'fees.json';file.write_text('old-sentinel')
 t=time.monotonic(); proc=subprocess.run([str(root/(variant+'-query-probe')),mode,str(case)],capture_output=True,text=True,timeout=14,env=env)
 elapsed=time.monotonic()-t; (root/(variant+'-query-'+mode+'.log')).write_text(proc.stdout+proc.stderr)
 assert proc.returncode==expected,(variant,mode,proc.returncode,proc.stderr)
 assert all(tag not in proc.stderr for tag in ('WARNING: ThreadSanitizer','runtime error:','ERROR: AddressSanitizer')),(variant,mode,proc.stderr)
 if expected==0:
  data=json.loads(file.read_text()); assert data['SHFE.rb2510']['open']==1
 else: assert file.read_text()=='old-sentinel',(variant,mode,'prior file changed')
 if mode.startswith('blocked-'):
  assert elapsed<12 and not (case/'send-returned').exists(),(variant,mode,elapsed)
 return f'{variant} {mode} PASS exit={proc.returncode} elapsed={elapsed:.2f}'
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
 for line in pool.map(run,[(variant,mode,expected) for variant in ('normal','asan','tsan') for mode,expected in results]):print(line,flush=True)
for mode in ('delayed','sync'):
 case=root/('baseline-query-'+mode);case.mkdir();(case/'fees.json').write_text('old-sentinel')
 proc=subprocess.run([str(root/'baseline-query-probe'),mode,str(case)],capture_output=True,text=True,timeout=14,env=env)
 (root/('baseline-query-'+mode+'.log')).write_text(proc.stdout+proc.stderr)
 assert proc.returncode!=0 or (case/'fees.json').read_text()=='old-sentinel',(mode,proc.returncode)
 print('baseline',mode,'known-red exit=',proc.returncode,flush=True)
