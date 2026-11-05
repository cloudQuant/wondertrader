import json
import os
import pathlib
import subprocess
import sys

evidence = pathlib.Path(sys.argv[1]).resolve()
evidence.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
results = []
for variant in ('red', 'green'):
    if variant == 'red':
        modes = ['clock', 'timezone', 'utc', 'local', 'mktime', 'callback-clock', 'callback-timezone',
                 'callback-utc', 'callback-local', 'callback-mktime', 'success', 'boundary',
                 'no-api', 'not-ready', 'sdk-failure']
    else:
        # The checked snapshot now converts the sampled instant directly with
        # localtime_r, and no longer calls time/gmtime/mktime for a cached offset.
        modes = ['clock', 'local', 'callback-clock', 'callback-local', 'success', 'boundary',
                 'no-api', 'not-ready', 'sdk-failure']
    if variant == 'green':
        modes += ['snapshot-clock', 'snapshot-local', 'snapshot-nanos',
                  'snapshot-negative', 'snapshot-range', 'snapshot-alias',
                  'local-kathmandu', 'local-adelaide', 'local-year-boundary',
                  'local-dst-transition', 'local-utc', 'local-legacy-independent',
                  'epoch-success', 'epoch-clock', 'epoch-local', 'epoch-overflow',
                  'epoch-alias-date-time', 'epoch-alias-epoch']
    for mode in modes:
        argv = ['/tmp/wt-i03-ctpopt-time-' + variant + '/confirm-probe', mode]
        try:
            p = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=15)
            code, output = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired as exc:
            code = 'TIMEOUT'
            output = str(exc.stdout or '') + str(exc.stderr or '')
        (evidence / (variant + '-' + mode + '.log')).write_text(output)
        results.append({'variant': variant, 'mode': mode, 'argv': argv, 'exit': code})
        print(variant, mode, code, flush=True)
(evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
