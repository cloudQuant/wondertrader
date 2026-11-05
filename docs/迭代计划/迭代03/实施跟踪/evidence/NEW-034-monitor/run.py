import os, subprocess, pathlib
out = pathlib.Path('docs/迭代计划/迭代03/实施跟踪/evidence/NEW-034-monitor')
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for variant in ('red', 'green'):
    for case in ('before_start', 'repeat', 'concurrent', 'self'):
        command = [f'/tmp/wt-i03-new034/{variant}/monitor', case]
        try:
            p = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5, env=env)
            result, output = str(p.returncode), p.stdout.decode(errors='replace')
        except subprocess.TimeoutExpired as exc:
            result, output = 'TIMEOUT', (exc.stdout or b'').decode(errors='replace')
        (out / f'{variant}-{case}.log').write_text(f'command={command!r}\nexit={result}\n' + output)
        print(variant, case, result)
