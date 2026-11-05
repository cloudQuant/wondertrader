import hashlib,json,os,pathlib,subprocess
root=pathlib.Path.cwd()
out=root/'docs/迭代计划/迭代03/实施跟踪/evidence/R22-002/lifecycle-20261102'
base=pathlib.Path('/tmp/wt-r22-lifecycle-20261102')
compiler='/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++'
libs='/tmp/wt-i03-start-cmake/build_x64/Debug/libs'
probe=root/'src/TestUnits/fixtures/r22_monitor_lifecycle/probe.cpp'
records=[]
def run(args,filename,timeout,env=None):
 try:
  p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout,env=env)
  code=p.returncode; output=p.stdout
 except subprocess.TimeoutExpired as e:
  code='TIMEOUT';output=(e.stdout or b'').decode(errors='replace') if isinstance(e.stdout,bytes) else (e.stdout or '')
 (out/filename).write_text('argv='+json.dumps(args,ensure_ascii=False)+'\nexit='+str(code)+'\n'+output)
 records.append({'argv':args,'exit':code,'log':filename})
 print(filename,code,flush=True)
 return code
for version in ('red','green'):
 s=base/'frozen'/version
 for san in ('address,undefined','thread'):
  name=version+'-'+('tsan' if san=='thread' else 'asan')+'-frozen'
  args=[compiler,'-std=c++17','-g','-O1','-fsanitize='+san,'-fprofile-instr-generate','-fno-omit-frame-pointer','-include','stdlib.h','-include',str(s/'Share/fmt_enum_compat.h'),'-DFMT_HEADER_ONLY','-DSPDLOG_FMT_EXTERNAL','-I'+str(s),'-I/opt/homebrew/Cellar/boost/1.88.0/include','-I/opt/homebrew/Cellar/fmt/11.2.0/include','-I/opt/homebrew/Cellar/spdlog/1.15.3/include']
  if version=='red':args+=['-DR22_OLD_BASELINE']
  args += [str(s/'WtDtCore/StateMonitor.cpp'),str(s/'WtDtCore/DataManager.cpp'),str(probe),'-L'+libs,'-lWtDtCore','-lWTSTools','-lWTSUtils','-L/opt/homebrew/lib','-Wl,-rpath,/opt/homebrew/lib','-lboost_filesystem','-lboost_thread','/usr/local/lib/libiconv.dylib','-o',str(base/name)]
  if run(args,name+'-build.log',120)!=0: continue
  env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1',TSAN_OPTIONS='halt_on_error=1:exitcode=66')
  for case in ('repeat','concurrent','runstop','multiple','self','self-external-join','queries','exception'):
   run([str(base/name),case],name+'-'+case+'.log',12,env)
(out/'verify-results.json').write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n')
paths=[probe,root/'src/WtDtCore/StateMonitor.cpp',root/'src/WtDtCore/StateMonitor.h',root/'src/Share/TimeUtils.hpp',root/'src/WtDtCore/DataManager.cpp']
for version in ('red','green'):
 s=base/'frozen'/version
 paths += [s/'WtDtCore/StateMonitor.cpp',s/'WtDtCore/StateMonitor.h',s/'WtDtCore/DataManager.cpp',s/'Share/TimeUtils.hpp']
 for san in ('asan','tsan'):
  path=base/(version+'-'+san+'-frozen')
  if path.exists():paths.append(path)
(out/'hashes.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
