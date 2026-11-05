import subprocess, sys, os, signal, time, json, pathlib
binp=sys.argv[1]; cases=sys.argv[2:]
res=[]
for c in cases:
    d=pathlib.Path(c.replace('/','_')); d.mkdir(exist_ok=True)
    # new session: canary sleep + test share one process group, isolated from us
    script='sleep 60 & echo $! > canary.pid; "$0" --gtest_filter="$1" > run.log 2>&1; echo "test_rc=$?" > rc.txt; wait'
    p=subprocess.Popen(['/bin/bash','-c',script,binp,c],cwd=str(d),start_new_session=True,
                       env=dict(os.environ,LLVM_PROFILE_FILE=str(d.resolve()/'p.profraw')))
    t0=time.time()
    while p.poll() is None and time.time()-t0<50:
        time.sleep(0.2)
        if (d/'rc.txt').exists(): break
    time.sleep(0.5)
    canary=int((d/'canary.pid').read_text()) if (d/'canary.pid').exists() else None
    alive=None
    if canary:
        try: os.kill(canary,0); alive=True
        except ProcessLookupError: alive=False
    shell_rc=p.poll()
    rc=(d/'rc.txt').read_text().strip() if (d/'rc.txt').exists() else 'none'
    res.append({'case':c,'canary_alive':alive,'shell_rc':shell_rc,'test':rc})
    try: os.killpg(p.pid, signal.SIGKILL)
    except Exception: pass
    p.wait()
    print(json.dumps(res[-1]),flush=True)
