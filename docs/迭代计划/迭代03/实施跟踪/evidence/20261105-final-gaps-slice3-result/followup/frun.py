#!/usr/bin/env python3
# frun.py <tag> <binary> <filter> [repeat]  : one process, cwd in scratch, log + xml to followup/logs
import os, sys, subprocess, json, re, shutil, glob, datetime
S='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
F='/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-gaps-slice3-result/followup/'
B=S+'slice3-result-build/build_x64/Debug/bin/TestUnits/'
tag, binary, filt = sys.argv[1], sys.argv[2], sys.argv[3]
rep = int(sys.argv[4]) if len(sys.argv) > 4 else 1
d=S+'s3fu/run/'+tag; shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
args=[B+binary,'--gtest_filter='+filt,'--gtest_output=xml:'+d+'/result.xml']
if rep>1: args.append('--gtest_repeat=%d'%rep)
env=dict(os.environ, LLVM_PROFILE_FILE=d+'/r-%p.profraw')
try:
    p=subprocess.run(['perl','-e','alarm 900; exec @ARGV']+args,cwd=d,capture_output=True,text=True,errors='replace',env=env,timeout=950)
    out=p.stdout+p.stderr; rc=p.returncode
except subprocess.TimeoutExpired:
    out='TIMEOUT'; rc=-999
ok=len(re.findall(r'^\[       OK \] ', out, re.M)); fl=len(re.findall(r'^\[  FAILED  \] \S+ \(', out, re.M))
with open(F+'logs/'+tag+'.log','w') as f:
    f.write(f"# {datetime.datetime.now().isoformat(timespec='seconds')} cwd={d}\n# cmd: {' '.join(args)}\n# rc={rc}\n"); f.write(out)
if os.path.exists(d+'/result.xml'): shutil.copy2(d+'/result.xml', F+'logs/'+tag+'.xml')
for x in glob.glob(d+'/*.profraw'): os.remove(x)
r=dict(tag=tag,binary=binary,filter=filt,repeat=rep,rc=rc,ok=ok,fail=fl,log='logs/'+tag+'.log')
open(F+'runs.jsonl','a').write(json.dumps(r,ensure_ascii=False)+'\n')
print(json.dumps(r,ensure_ascii=False))
