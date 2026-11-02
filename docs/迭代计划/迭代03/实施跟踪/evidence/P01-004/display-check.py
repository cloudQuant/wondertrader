import json, subprocess, os
from pathlib import Path
p=Path('/tmp/wt-i03-p01-004'); build='/tmp/wt-i02-fresh-20261101-v8-build'
links=[]
for variant, fname, source, old in [('normal','commands.json','ctx.o','previous-ctx.o'),('asan','asan-commands.json','ctx-asan.o','previous-ctx-asan.o')]:
 cmd=json.loads((p/fname).read_text())[3]
 cmd=[x.replace('/'+source,'/'+old).replace('/focused-asan','/previous-focused-asan').replace('/focused','/previous-focused') for x in cmd]
 # Avoid applying both output replacements to the ASan filename.
 cmd=[x.replace('/previous-previous-focused-asan','/previous-focused-asan') for x in cmd]
 proc=subprocess.run(cmd,cwd=build,capture_output=True,text=True); (p/f'previous-{variant}-link.log').write_text(proc.stdout+proc.stderr); links.append({'variant':variant,'command':cmd,'exit':proc.returncode}); print('link',variant,proc.returncode,flush=True)
 if proc.returncode: raise SystemExit(proc.returncode)
(p/'previous-link-commands.json').write_text(json.dumps(links,indent=2))
tests=['ExpiredPositionCodesHaveBoundedLogDisplay','ExpiredSignalCodesHaveBoundedLogDisplay','LongRecoveredPositionCodesKeepTheirFullIdentity','LongRecoveredSignalCodesKeepTheirFullIdentity','LongDirectTickSubscriptionsKeepTheirFullIdentity','MalformedSnapshotsDoNotPublishAnyStateOrSubscriptions']
for variant in ['normal','asan','red-normal','red-asan']:
 for name in tests:
  if variant.startswith('red') and name.startswith('Malformed'): continue
  folder=p/'display-tests'/variant/name; folder.mkdir(parents=True,exist_ok=True)
  binary={'normal':'focused','asan':'focused-asan','red-normal':'previous-focused','red-asan':'previous-focused-asan'}[variant]
  env=os.environ.copy(); env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1'
  try:
   result=subprocess.run([str(p/binary),'--gtest_filter=R42CtaPersistTest.'+name,'--gtest_output=xml:result.xml'],cwd=folder,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30); code=result.returncode; out=result.stdout
  except subprocess.TimeoutExpired as e: code=124; out=str(e)
  (folder/'output.log').write_text(out); print(variant,name,code,flush=True)
