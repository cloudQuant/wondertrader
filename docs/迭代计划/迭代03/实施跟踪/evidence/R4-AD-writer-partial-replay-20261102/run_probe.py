import json
import os
from pathlib import Path
import subprocess

root = Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
evidence = root / 'docs/迭代计划/迭代03/实施跟踪/evidence/R4-AD-writer-partial-replay-20261102'
source = root / 'docs/迭代计划/迭代03/实施跟踪/evidence/R4-AD-init-retry-20261102/bar-commands.json'
build = Path('/tmp/wt-i03-ad-tick-alignment-20261102/build')
output = Path('/tmp/wt-i03-ad-partial-replay')
output.mkdir(exist_ok=True)
commands = json.loads(source.read_text())
writer_commands = json.loads((root / 'docs/迭代计划/迭代03/实施跟踪/evidence/R4-AD-init-retry-20261102/green-commands.json').read_text())
replacements = {
    '/tmp/wt-i03-ad-init-review/green-writer.o': str(output / 'writer.o'),
    '/tmp/wt-i03-ad-init-review/green-writer.d': str(output / 'writer.d'),
    '/tmp/wt-i03-ad-init-review/bar-failure-test.o': str(output / 'probe.o'),
    '/tmp/wt-i03-ad-init-review/bar-failure-test.d': str(output / 'probe.d'),
    '/tmp/wt-i03-ad-init-review/bar-failure': str(output / 'probe'),
    str(root / 'docs/迭代计划/迭代03/实施跟踪/evidence/R4-AD-init-retry-20261102/bar_failure_probe.cpp'): str(evidence / 'partial_replay_probe.cpp'),
}
commands.insert(0, writer_commands[0])
commands = [[replacements.get(arg, arg) for arg in cmd] for cmd in commands]
(evidence / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
for i, cmd in enumerate(commands):
    p = subprocess.run(cmd, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (evidence / f'build-{i}.log').write_text(p.stdout)
    if p.returncode:
        raise SystemExit(f'build {i} failed: {p.returncode}')
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1', TZ='UTC')
cases = ['ManualSyncPartialSuccessRetryDuplicatesCommittedDestination', 'ManualAsyncAcceptedEnqueueStillDuplicatesOnRetry', 'ManualCrashAfterExternalEffectCanDuplicateOnRestart', 'ManualSyncDumperReentryCanDeadlock']
results = {}
for case in cases:
    p = subprocess.run([str(output / 'probe'), '--gtest_filter=R4WriterADTest.' + case], cwd=build, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    (evidence / f'{case}.log').write_text(p.stdout)
    results[case] = p.returncode
    print(case, p.returncode, flush=True)
(evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
