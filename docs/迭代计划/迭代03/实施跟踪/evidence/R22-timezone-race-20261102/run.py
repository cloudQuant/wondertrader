import json
import os
import pathlib
import subprocess

out = pathlib.Path(__file__).resolve().parent
root = pathlib.Path('/tmp/wt-i03-timezone-race')
results = []

def run(name, binary, mode, tz, epoch='1704067200', sanitizer=None):
    env = dict(os.environ, TZ=tz)
    if sanitizer == 'tsan':
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
    if sanitizer == 'asan':
        env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    argv = [str(root / binary), mode, epoch]
    try:
        p = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=15)
        code, text = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as exc:
        code = 'TIMEOUT'
        text = str(exc.stdout or '') + str(exc.stderr or '')
    (out / (name + '.log')).write_text(text)
    results.append(dict(name=name, argv=argv, tz=tz, exit=code))
    print(name, code, flush=True)

run('baseline-tsan-race', 'baseline-tsan', 'race', 'Asia/Shanghai', sanitizer='tsan')
for i in range(3):
    run('current-tsan-race-' + str(i), 'current-tsan', 'race', 'Asia/Shanghai', sanitizer='tsan')
for tz, stamp, label in [
    ('UTC', '1704067200', 'utc'), ('Asia/Shanghai', '1704067200', 'shanghai'),
    ('America/New_York', '1704067200', 'ny-winter'), ('America/New_York', '1720000000', 'ny-summer'),
    ('Asia/Kolkata', '1704067200', 'kolkata'), ('America/St_Johns', '1704067200', 'st-johns'),
    ('Pacific/Kiritimati', '1704067200', 'kiritimati'), ('Pacific/Honolulu', '1704067200', 'honolulu'),
    ('Asia/Shanghai', '1735689599', 'shanghai-new-year'),
    ('America/New_York', '1704067200', 'ny-prior-year'),
]:
    run('normal-' + label, 'current-asan', 'normal', tz, stamp, 'asan')
    run('baseline-normal-' + label, 'baseline-tsan', 'normal', tz, stamp, 'tsan')
run('cache', 'current-asan', 'cache', 'America/New_York', sanitizer='asan')
for stage in range(1, 5):
    run('failure-' + str(stage), 'current-asan', str(stage), 'Asia/Shanghai', sanitizer='asan')
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
