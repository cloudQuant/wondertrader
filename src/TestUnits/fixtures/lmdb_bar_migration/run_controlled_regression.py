import json
import os
from pathlib import Path
import subprocess
import sys

out = Path(sys.argv[1])
variant = sys.argv[2]
modes = (
    'success', 'reopen', 'packed', 'big-packed', 'same', 'alias', 'unsafe',
    'limit', 'missing', 'identity', 'time', 'conflict', 'missing-stage',
    'extra-stage', 'crash', 'source-after-batch', 'interrupted-batch',
    'badkey', 'badvalue', 'map-full', 'map-full-partial', 'concurrent-prepare', 'unprepared', 'prepare-twice',
    'concurrent-same', 'concurrent-different', 'stat-under', 'stat-over',
    'evidence-revoked-between-batches', 'legacy',
)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
           UBSAN_OPTIONS='halt_on_error=1')
results = []
for mode in modes:
    argv = [str(out / f'{variant}-test'), mode, str(out / f'{variant}-{mode}')]
    try:
        proc = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=30)
        result, output = proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as exc:
        result, output = 'TIMEOUT', str(exc)
    (out / f'{variant}-{mode}.log').write_text(output)
    results.append({'mode': mode, 'argv': argv, 'exit': result})
    print(variant, mode, result, flush=True)
(out / f'{variant}-results.json').write_text(json.dumps(results, indent=2) + '\n')
if any(row['exit'] != 0 for row in results):
    sys.exit(1)
