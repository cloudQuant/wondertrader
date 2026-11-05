#!/usr/bin/env python3
# disrun.py <phase:pre|post|postproc> <tree:main|snap> <idx,...> [--jobs N]
# pre: run DISABLED_ name with --gtest_also_run_disabled_tests --gtest_repeat=20 (in-process)
# post: run enabled name (DISABLED_ stripped) --gtest_repeat=20 (in-process)
# postproc: run enabled name 20 times, one process each
import json, os, sys, subprocess, shutil, glob, re, concurrent.futures as cf, datetime
S='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
E='/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-gaps-slice3-result/disabled/'
inv=json.load(open(S+'disabled-inventory.json'))
phase, tree, idxs = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(',') if x]
jobs = int(sys.argv[sys.argv.index('--jobs')+1]) if '--jobs' in sys.argv else 3
B = S+('slice3-result-build' if tree=='main' else 'slice3-red-build')+'/build_x64/Debug/bin'
PROV = {'CoverageTests_m_WtRunner'}
def strip(t):
    s,n=t.split('.',1)
    s=s[len('DISABLED_'):] if s.startswith('DISABLED_') else s
    n=n[len('DISABLED_'):] if n.startswith('DISABLED_') else n
    return s+'.'+n
def one(i):
    it=inv[i-1]; binp=os.path.join(B,'TestUnits',it['binary'])
    name = it['test'] if phase in ('pre','preproc') else strip(it['test'])
    tag=f"{phase}-{tree}-{i:03d}"
    d=S+'s3dis/run/'+tag; shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    if it['binary'] in PROV:
        for f in glob.glob(B+'/lib*.dylib'):
            if os.path.isfile(f) and not os.path.islink(f): shutil.copy2(f, d)
    env=dict(os.environ, LLVM_PROFILE_FILE=d+'/r-%p.profraw')
    logs=[]; rcs=[]
    if phase in ('pre','post'):
        args=[binp,'--gtest_filter='+name,'--gtest_repeat=20','--gtest_output=xml:'+d+'/result.xml']
        if phase=='pre': args.insert(1,'--gtest_also_run_disabled_tests')
        try:
            p=subprocess.run(['perl','-e','alarm 1500; exec @ARGV']+args,cwd=d,capture_output=True,text=True,errors='replace',env=env,timeout=1600)
            out=p.stdout+p.stderr; rc=p.returncode
        except subprocess.TimeoutExpired:
            out='TIMEOUT'; rc=-999
        logs.append(out); rcs.append(rc)
        npass=len(re.findall(r'^\[       OK \] '+re.escape(name), out, re.M))
        nfail=len(re.findall(r'^\[  FAILED  \] '+re.escape(name)+r' \(', out, re.M))
        nskip=len(re.findall(r'^\[  SKIPPED \] '+re.escape(name)+r' \(', out, re.M))
        cmd=' '.join(args)
    else:
        npass=nfail=nskip=0
        for k in range(20):
            args=[binp,'--gtest_filter='+name]
            if phase=='preproc': args.insert(1,'--gtest_also_run_disabled_tests')
            p=subprocess.run(['perl','-e','alarm 600; exec @ARGV']+args,cwd=d,capture_output=True,text=True,errors='replace',env=env,timeout=700)
            out=p.stdout+p.stderr; logs.append(f'##### process {k+1} rc={p.returncode}\n'+out); rcs.append(p.returncode)
            if p.returncode==0 and '[  PASSED  ] 1 test' in out: npass+=1
            elif 'SKIPPED' in out and p.returncode==0: nskip+=1
            else: nfail+=1
        rc=max(rcs, key=abs); cmd=' '.join(args)+'  (x20 separate processes)'
    log='\n'.join(logs)
    lp=E+'logs/'+tag+'.log'
    with open(lp,'w') as f:
        f.write(f"# {datetime.datetime.now().isoformat(timespec='seconds')} cwd={d}\n# cmd: {cmd}\n# rc={rcs}\n"); f.write(log)
    if os.path.exists(d+'/result.xml'): shutil.copy2(d+'/result.xml', E+'logs/'+tag+'.xml')
    for f in glob.glob(d+'/*.profraw'): os.remove(f)
    for f in glob.glob(d+'/lib*.dylib'): os.remove(f)
    return dict(idx=i, binary=it['binary'], test=name, phase=phase, tree=tree, rc=rc, ok=npass, fail=nfail, skip=nskip, log='logs/'+tag+'.log', cmd=cmd)
res=[]
with cf.ThreadPoolExecutor(jobs) as ex:
    for r in ex.map(one, idxs):
        res.append(r); print(json.dumps(r, ensure_ascii=False), flush=True)
with open(E+'runs.jsonl','a') as f:
    for r in res: f.write(json.dumps(r, ensure_ascii=False)+'\n')
