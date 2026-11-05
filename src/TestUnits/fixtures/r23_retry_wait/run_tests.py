import os
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
cases = [('baseline', 'shutdown', 5), ('normal', 'shutdown', 0),
         ('normal', 'normal', 0), ('asan', 'shutdown', 0), ('asan', 'normal', 0),
         ('tsan', 'shutdown', 0), ('tsan', 'normal', 0)]
results = []
for variant, mode, expected in cases:
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=1'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1'
    env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
    data = root / ('data-' + variant + '-' + mode)
    try:
        p = subprocess.run([str(root / (variant + '-probe')), mode, str(data)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=15, env=env)
        result, output = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        result, output = 'TIMEOUT', str(e.stdout or '')
    (root / (variant + '-' + mode + '.log')).write_text(output)
    results.append((variant, mode, result, expected))
    print(variant, mode, result, 'expected', expected, flush=True)
(root / 'results.txt').write_text(''.join(f'{v} {m} {r} expected={e}\n' for v, m, r, e in results))
if any(result != expected for _, _, result, expected in results):
    raise SystemExit(1)
