import subprocess, sys, os, json, concurrent.futures as cf, shutil, glob
# percase.py <binary path> <outdir> <filter> [provision_bin_dir]
binp, out, filt = sys.argv[1], sys.argv[2], sys.argv[3]
prov = sys.argv[4] if len(sys.argv) > 4 else ''
os.makedirs(out, exist_ok=True)
lst = subprocess.run([binp, '--gtest_list_tests', '--gtest_filter=' + filt], capture_output=True, text=True, env=dict(os.environ, LLVM_PROFILE_FILE='/dev/null')).stdout
cases = []; suite = None
for line in lst.splitlines():
    if not line.strip(): continue
    if not line.startswith(' '):
        suite = line.strip().split()[0]
    else:
        name = line.strip().split()[0]
        if name.startswith('DISABLED_') or suite.startswith('DISABLED_'): continue
        cases.append(suite + name)
def run(c):
    d = os.path.join(out, c.replace('/', '_'))
    os.makedirs(d, exist_ok=True)
    if prov:
        for f in glob.glob(os.path.join(prov, 'lib*.dylib')):
            if os.path.isfile(f) and not os.path.islink(f): shutil.copy2(f, d)
    env = dict(os.environ, LLVM_PROFILE_FILE=os.path.join(d, 'r.profraw'))
    try:
        p = subprocess.run(['perl', '-e', 'alarm 600; exec @ARGV', binp, '--gtest_filter=' + c], cwd=d, capture_output=True, text=True, env=env, timeout=650)
        log = p.stdout + p.stderr; rc = p.returncode
    except subprocess.TimeoutExpired:
        log = 'TIMEOUT'; rc = -999
    open(os.path.join(d, 'log.txt'), 'w').write(log)
    st = 'pass' if rc == 0 and '[  PASSED  ] 1 test' in log else ('skipped' if 'SKIPPED' in log and rc == 0 else 'fail')
    return c, st, rc
res = []
with cf.ThreadPoolExecutor(4) as ex:
    for r in ex.map(run, cases): res.append(r)
summ = {'pass': 0, 'fail': 0, 'skipped': 0}
for c, st, rc in res: summ[st] += 1
json.dump({'binary': binp, 'filter': filt, 'summary': summ, 'cases': [{'test': c, 'status': s, 'rc': r} for c, s, r in res]}, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
print(summ)
for c, s, r in res:
    if s == 'fail': print('FAIL', c, r)
