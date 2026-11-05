import hashlib,json,os,pathlib,subprocess
root=pathlib.Path.cwd();out=root/'docs/迭代计划/迭代03/实施跟踪/evidence/R22-002/owner-order-20261102';base=pathlib.Path('/tmp/wt-r22-owner-20261102');records=[];compiler='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++'
def run(args,name,timeout,env=None):
 try:p=subprocess.run(args,capture_output=True,text=True,timeout=timeout,env=env);code=p.returncode;output=p.stdout+p.stderr
 except subprocess.TimeoutExpired as e:code='TIMEOUT';output=str(e)
 (out/name).write_text('argv='+json.dumps(args,ensure_ascii=False)+'\nexit='+str(code)+'\n'+output);records.append({'argv':args,'exit':code,'log':name});print(name,code,flush=True);return code
for version in ('red','green'):
 s=base/version
 for san in ('address,undefined','thread'):
  key='tsan' if san=='thread' else 'asan';exe=base/(version+'-quote-'+key);obj=base/(version+'-quote-'+key+'.o')
  common=[compiler,'-std=c++17','-g','-O1','-fsanitize='+san,'-fprofile-instr-generate','-fno-omit-frame-pointer','-include','stdlib.h','-include',str(s/'Share/fmt_enum_compat.h'),'-DFMT_HEADER_ONLY','-DSPDLOG_FMT_EXTERNAL','-I'+str(s),'-I/opt/homebrew/Cellar/boost/1.88.0/include','-I/opt/homebrew/Cellar/fmt/11.2.0/include','-I/opt/homebrew/Cellar/spdlog/1.15.3/include']
  if run(common+['-Dmain=quote_factory_main','-c',str(s/'QuoteFactory/main.cpp'),'-o',str(obj)],version+'-quote-'+key+'-compile.log',90)!=0:continue
  cmd=common+[str(s/'WtDtCore/StateMonitor.cpp'),str(s/'WtDtCore/DataManager.cpp'),str(obj),str(root/'src/TestUnits/fixtures/r22_monitor_owner/quote_owner_probe.cpp'),'-L/tmp/wt-r22-lifecycle-20261102/cmake/build_x64/Debug/libs','-lWtDtCore','-lWTSTools','-lWTSUtils','-L/opt/homebrew/lib','-Wl,-rpath,/opt/homebrew/lib','-lboost_filesystem','-lboost_thread','/usr/local/lib/libiconv.dylib','-o',str(exe)]
  if run(cmd,version+'-quote-'+key+'-link.log',120)!=0:continue
  env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1',TSAN_OPTIONS='halt_on_error=1:exitcode=66')
  run([str(exe)],version+'-quote-'+key+'-run.log',15,env)
  run([str(exe),'idle'],version+'-quote-'+key+'-idle.log',15,env)
(out/'quote-results.json').write_text(json.dumps(records,indent=2,ensure_ascii=False)+'\n')
