import json,os,pathlib,subprocess,sys
repo=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader');ev=repo/'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-008-owner-delivery-20261102';out=pathlib.Path('/tmp/wt-i03-owner-delivery-20261102');key=sys.argv[1];assert key in ('asan','tsan');cases=[x['test'] for x in json.loads((ev/'green-results.json').read_text())];results=[];env=dict(os.environ,TSAN_OPTIONS='halt_on_error=1',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
for i,case in enumerate(cases):
 cwd=out/f'{key}-run{i}';cwd.mkdir(parents=True,exist_ok=True);argv=[str(out/f'{key}-focused'),'--gtest_filter='+case]
 try:r=subprocess.run(argv,cwd=cwd,env=env,capture_output=True,text=True,timeout=20);code=r.returncode;text=r.stdout+r.stderr
 except subprocess.TimeoutExpired as e:code='TIMEOUT';text=str(e)
 (ev/f'{key}-run{i}.log').write_text(text+'\nexit='+str(code)+'\n');results.append({'test':case,'argv':argv,'cwd':str(cwd),'exit':code});print(key,case,code,flush=True)
(ev/f'{key}-results.json').write_text(json.dumps(results,indent=2)+'\n')
