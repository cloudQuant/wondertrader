import json
import os
import pathlib
import subprocess

evidence = pathlib.Path(__file__).resolve().parent
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
local_cases = ['local-kathmandu', 'local-adelaide', 'local-year-boundary',
               'local-dst-transition', 'local-utc', 'local-legacy-independent']
other_cases = ['clock', 'local', 'callback-clock', 'callback-local', 'success',
               'boundary', 'no-api', 'not-ready', 'sdk-failure', 'snapshot-clock',
               'snapshot-local', 'snapshot-nanos', 'snapshot-negative',
               'snapshot-range', 'snapshot-alias', 'epoch-success', 'epoch-clock',
               'epoch-local', 'epoch-overflow', 'epoch-alias-date-time', 'epoch-alias-epoch']
results = []
for variant, binary, cases in (
    ('red', '/tmp/wt-i03-localtime-checked-red/confirm-probe', local_cases),
    ('green', '/tmp/wt-i03-localtime-green/confirm-probe', local_cases + other_cases),
):
    for mode in cases:
        argv = [binary, mode]
        try:
            p = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=15)
            code, output = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired as exc:
            code, output = 'TIMEOUT', str(exc.stdout or '') + str(exc.stderr or '')
        (evidence / (variant + '-' + mode + '.log')).write_text(output)
        results.append({'variant': variant, 'mode': mode, 'argv': argv, 'exit': code})
        print(variant, mode, code, flush=True)
(evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
