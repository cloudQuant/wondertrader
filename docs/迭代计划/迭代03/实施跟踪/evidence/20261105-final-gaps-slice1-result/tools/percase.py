#!/usr/bin/env python3
"""percase.py BIN OUTDIR REGEX [WORKERS] : run each matching gtest case in its own process/cwd."""
import json, os, re, subprocess, sys, pathlib, concurrent.futures as cf
binp, out, rx = sys.argv[1], pathlib.Path(sys.argv[2]), re.compile(sys.argv[3])
workers = int(sys.argv[4]) if len(sys.argv) > 4 else 3
out.mkdir(parents=True, exist_ok=True)
lst = subprocess.check_output([binp, '--gtest_list_tests'], text=True, cwd=str(out)).splitlines()
cases, suite = [], None
for l in lst:
    if not l.startswith(' '):
        suite = l.split('#')[0].strip()
    else:
        name = l.strip().split('#')[0].strip()
        full = suite + name
        if not name.startswith('DISABLED_') and not suite.startswith('DISABLED_') and rx.search(full):
            cases.append(full)
def run(case):
    d = out / re.sub(r'[^A-Za-z0-9_.]', '_', case); d.mkdir(parents=True, exist_ok=True)
    if os.environ.get('RESUME') == '1' and (d / 'run.log').exists():
        log = (d / 'run.log').read_text(errors='replace')
        status = 'pass' if '[  PASSED  ] 1 test' in log else ('skipped' if 'SKIPPED' in log else 'fail')
        return {'test': case, 'rc': 'resumed', 'status': status}
    env = dict(os.environ, LLVM_PROFILE_FILE=str(d / 'p.profraw'))
    try:
        p = subprocess.run([binp, '--gtest_filter=' + case, '--gtest_output=xml:' + str(d / 'r.xml')], cwd=str(d),
                           env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180, start_new_session=True)
        rc = p.returncode; log = p.stdout.decode(errors='replace')
    except subprocess.TimeoutExpired as e:
        rc = 'TIMEOUT'; log = (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
    (d / 'run.log').write_text(log)
    status = 'pass' if rc == 0 and '[  PASSED  ] 1 test' in log else ('skipped' if rc == 0 and 'SKIPPED' in log else 'fail')
    return {'test': case, 'rc': rc, 'status': status}
res = []
with cf.ThreadPoolExecutor(workers) as ex:
    for r in ex.map(run, cases):
        res.append(r)
with open(out / 'results.jsonl', 'w') as f:
    for r in res: f.write(json.dumps(r) + '\n')
from collections import Counter
c = Counter(r['status'] for r in res)
summary = {'binary': binp, 'regex': sys.argv[3], 'cases': len(res), 'counts': dict(c), 'nonpass': [r for r in res if r['status'] != 'pass']}
(out / 'summary.json').write_text(json.dumps(summary, indent=1))
print(json.dumps({'cases': len(res), 'counts': dict(c)}))
for r in summary['nonpass']: print('NONPASS', r)
