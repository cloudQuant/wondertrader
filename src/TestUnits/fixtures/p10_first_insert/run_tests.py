#!/usr/bin/env python
"""Run each test in a separate process with a hard deadline."""
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
names = ('Tick', 'Transaction', 'OrderDetail', 'OrderQueue', 'Minute1', 'Minute5')
results = []
for variant in ('normal', 'tsan'):
    for name in names:
        env = os.environ.copy()
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
        command = [str(root / (variant + '-test')), '--gtest_filter=FirstInsertFixture.' + name]
        try:
            process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     env=env, timeout=20, text=True)
            result, output = process.returncode, process.stdout
        except subprocess.TimeoutExpired as exc:
            result = 'TIMEOUT'
            output = exc.stdout or ''
            if isinstance(output, bytes):
                output = output.decode(errors='replace')
        (root / (variant + '-' + name + '.log')).write_text(output)
        results.append((variant, name, result))
        print(variant, name, result, flush=True)
(root / 'results.txt').write_text(''.join(f'{v} {n} {r}\n' for v, n, r in results))
if any(result != 0 for _, _, result in results):
    sys.exit(1)
