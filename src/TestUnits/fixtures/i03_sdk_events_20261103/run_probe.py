# 迭代03 卡14 BUG-T26-011：逐 mode 独立进程运行 CTPLoader 探针并判定（本机 fake，非真实 SDK）。
from pathlib import Path
import concurrent.futures, json, os, subprocess, sys, time
root = Path(sys.argv[1])
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', TSAN_OPTIONS='halt_on_error=1')
FILES = ('fees.json', 'contracts.json', 'commodities.json')
# mode -> (期望退出码, 成功时应发布的文件, stderr 必含, stderr 禁含, 最大耗时秒)
CASES = {
 'fee-reverse':          (0, ('fees.json', 'contracts.json', 'commodities.json'), ('sent, awaiting confirmation', 'CTP fee batch closed: 3 sent, 3 confirmed'), ('succeed',), 12),
 'fee-empty-commission': (1, (), ('Empty response for request',), ('fees dumped', 'contracts dumped'), 12),
 'fee-empty-margin':     (1, (), ('Empty response for request',), ('fees dumped', 'contracts dumped'), 12),
 'fee-empty-instrument': (1, (), ('Empty instrument response',), ('Quering commission', 'contracts dumped'), 5),
 'plain-ok':             (0, ('contracts.json', 'commodities.json'), ('sent, awaiting confirmation', 'confirmed with 1 rows'), ('succeed',), 5),
 'plain-sync':           (0, ('contracts.json', 'commodities.json'), ('confirmed with 1 rows',), (), 5),
 'plain-stale':          (0, ('contracts.json', 'commodities.json'), ('not in flight', 'confirmed with 1 rows'), (), 5),
 'plain-error':          (1, (), ('failed, no files published',), ('contracts dumped',), 5),
 'plain-empty':          (1, (), ('empty response without SDK contract',), ('contracts dumped',), 5),
 'plain-rsp-error':      (1, (), ('failed by error response',), ('contracts dumped',), 5),
 'plain-send-failure':   (1, (), ('send failed',), ('contracts dumped',), 5),
}
def run(variant, mode):
    expected, published, must, mustnot, limit = CASES[mode]
    case = root / f'{variant}-{mode}'; case.mkdir()
    for name in FILES: (case / name).write_text('old-sentinel')
    t = time.monotonic()
    proc = subprocess.run([str(root / f'{variant}-probe'), mode, str(case)], capture_output=True, text=True, timeout=20, env=env)
    elapsed = time.monotonic() - t
    log = proc.stdout + proc.stderr
    (root / f'{variant}-{mode}.log').write_text(log)
    problems = []
    if proc.returncode != expected: problems.append(f'exit={proc.returncode} expected={expected}')
    for tag in ('WARNING: ThreadSanitizer', 'runtime error:', 'ERROR: AddressSanitizer'):
        if tag in log: problems.append('sanitizer:' + tag)
    for name in FILES:
        changed = (case / name).read_text() != 'old-sentinel'
        if changed != (name in published): problems.append(f'{name} changed={changed}')
    if 'fees.json' in published and not problems:
        if json.loads((case / 'fees.json').read_text())['SHFE.rb2510']['open'] != 1: problems.append('fees content')
    if 'contracts.json' in published and 'rb2510' not in (case / 'contracts.json').read_text(): problems.append('contracts content')
    problems += [f'missing:{m}' for m in must if m not in log]
    problems += [f'forbidden:{m}' for m in mustnot if m in log]
    if elapsed > limit: problems.append(f'elapsed={elapsed:.2f}>{limit}')
    return variant, mode, problems, proc.returncode, elapsed
variants = sys.argv[2].split(',')
failed = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    for variant, mode, problems, rc, elapsed in pool.map(lambda vm: run(*vm), [(v, m) for v in variants for m in CASES]):
        status = 'PASS' if not problems else ('RED' if variant == 'baseline' else 'FAIL')
        if problems and variant != 'baseline': failed += 1
        print(f'{variant} {mode} {status} exit={rc} elapsed={elapsed:.2f} {"; ".join(problems)}', flush=True)
print(f'non-baseline failures: {failed}')
sys.exit(1 if failed else 0)
