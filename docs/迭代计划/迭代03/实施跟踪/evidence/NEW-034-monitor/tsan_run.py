import os, subprocess, pathlib
out=pathlib.Path('docs/迭代计划/迭代03/实施跟踪/evidence/NEW-034-monitor')
env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1')
for variant in ('red','green'):
    for case in ('before_start','repeat','concurrent','self'):
        cmd=[f'/tmp/wt-i03-new034/{variant}/monitor-tsan',case]
        try:
            r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=8,env=env)
            result=str(r.returncode); output=r.stdout.decode(errors='replace')
        except subprocess.TimeoutExpired as e:
            result='TIMEOUT'; output=(e.stdout or b'').decode(errors='replace')
        (out/f'{variant}-tsan-{case}.log').write_text(f'command={cmd!r}\nexit={result}\n'+output)
        print(variant,case,result)
