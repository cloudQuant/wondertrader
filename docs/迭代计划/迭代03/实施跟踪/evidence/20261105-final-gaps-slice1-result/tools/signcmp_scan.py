#!/usr/bin/env python3
"""signcmp_scan.py CMDS OUTDIR [JOBS] [REGEX]
Re-run each coverage test TU compile command from `ninja -t commands` with
`-isystem /opt/homebrew/include` replaced by `-I/opt/homebrew/include`, as
-fsyntax-only, -Wno-error (all diagnostics kept). Records per-TU logs/exit codes,
and the test-source lines that instantiate a -Wsign-compare diagnostic."""
import json, os, re, shlex, subprocess, sys, pathlib, hashlib, concurrent.futures as cf
cmds_file, out = sys.argv[1], pathlib.Path(sys.argv[2]); jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 3
rx = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
STD_MODE = os.environ.get('SCAN_MODE') == 'std'   # std: unchanged flags (-isystem, -Werror), syntax only
out.mkdir(parents=True, exist_ok=True); (out / 'logs').mkdir(exist_ok=True)
ROOT = '/Users/yunjinqi/Documents/new_projects/wondertrader/src/TestUnits/coverage/'
seen = {}
for line in open(cmds_file):
    line = line.strip()
    m = re.search(r' -c (\S+)', line)
    if not m or not m.group(1).startswith(ROOT): continue
    src = m.group(1)
    if rx and not rx.search(src): continue
    seen.setdefault(src, line)   # first target's command per source
def scan(item):
    src, line = item
    a = shlex.split(line)
    b = []; i = 0; replaced = False
    while i < len(a):
        x = a[i]
        if STD_MODE is False and x == '-isystem' and i + 1 < len(a) and a[i+1] == '/opt/homebrew/include':
            b.append('-I/opt/homebrew/include'); i += 2; replaced = True; continue
        if x in ('-o', '-MF', '-MT'): i += 2; continue
        if x in ('-MD', '-MMD'): i += 1; continue
        b.append(x); i += 1
    b += ['-fsyntax-only'] if STD_MODE else ['-fsyntax-only', '-Wno-error', '-Wsign-compare']
    tag = src[len(ROOT):].replace('/', '__')
    p = subprocess.run(b, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=900)
    (out / 'logs' / (tag + '.log')).write_text(p.stdout)
    warn = re.findall(r'warning: .*\[(-W[^\],]+)', p.stdout)
    # instantiation notes inside the test sources that lead to -Wsign-compare
    sites = set(); blocks = p.stdout.split('\n')
    cur = None
    for l in blocks:
        if 'warning:' in l or 'error:' in l:
            cur = '-Wsign-compare' if '[-Wsign-compare' in l else 'other'
            mm = re.match(r'(/[^:]+):(\d+):\d+: (warning|error)', l)
            if cur == '-Wsign-compare' and mm and mm.group(1).startswith(ROOT):
                sites.add(mm.group(1)[len(ROOT):] + ':' + mm.group(2))
        elif cur == '-Wsign-compare':
            mm = re.match(r'(/[^:]+):(\d+):\d+: note: in instantiation', l)
            if mm and mm.group(1).startswith(ROOT):
                sites.add(mm.group(1)[len(ROOT):] + ':' + mm.group(2))
    return {'src': src[len(ROOT):], 'rc': p.returncode, 'replaced': replaced,
            'warnings': {w: warn.count(w) for w in sorted(set(warn))}, 'sign_compare_sites': sorted(sites)}
res = []
with cf.ThreadPoolExecutor(jobs) as ex:
    for r in ex.map(scan, sorted(seen.items())):
        res.append(r); print(json.dumps(r), flush=True)
with open(out / 'results.jsonl', 'w') as f:
    for r in res: f.write(json.dumps(r) + '\n')
from collections import Counter
wc = Counter(); sites = set()
for r in res:
    wc.update(r['warnings']); sites.update(r['sign_compare_sites'])
summary = {'tus': len(res), 'rc_nonzero': [r['src'] for r in res if r['rc'] != 0],
           'not_replaced': [r['src'] for r in res if not r['replaced']],
           'tus_with_sign_compare': sorted(r['src'] for r in res if r['warnings'].get('-Wsign-compare')),
           'warning_counts': dict(wc), 'sign_compare_sites': sorted(sites)}
(out / 'summary.json').write_text(json.dumps(summary, indent=1, ensure_ascii=False))
print('SUMMARY', json.dumps({k: (len(v) if isinstance(v, list) else v) for k, v in summary.items()}))
