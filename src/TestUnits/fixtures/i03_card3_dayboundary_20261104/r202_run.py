#!/usr/bin/env python3
"""Card 3 (3a=A) + card 9 9d=A ticker probe for BUG-R202-001.

Derived from src/TestUnits/fixtures/r202_nextday/run.py. Copies the ticker sources/headers of the
chosen source root into a copy of the old isolated snapshot, then recompiles every object of the
CoverageTests_wtcore link that depends on a ticker header (ninja -t deps: the three tickers, the
three engines, ParserAdapter and the R202 test TU), and relinks against the existing isolated build
/tmp/wt-i03-r202-001-midnight (the remaining engine objects, static libraries and dependencies come
from that older snapshot -- same limitation as the r202_nextday fixture). The script only records
per-test exit codes; it does not assert red/green itself.

usage: r202_run.py <out-dir (must not exist)> <tickers-root: path to a src/ tree>
env:   R202_TSAN=1    -> ThreadSanitizer build of all recompiled TUs
       R202_REGRESS=1 -> also build/run the existing ticker tests (r160/r170/r41/r66/n02 slicing/r181),
                         one binary per file, case by case
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[4]
fixture = Path(__file__).resolve().parent
build = Path('/tmp/wt-i03-r202-001-midnight/build')
old_snap = Path('/tmp/wt-i03-r202-001-midnight/snapshot/src')
tsan = os.environ.get('R202_TSAN') == '1'
out = Path(sys.argv[1]).resolve()
tick_root = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=False)
snap = out / 'snapshot' / 'src'
shutil.copytree(old_snap, snap, symlinks=False)
for path in ['Includes/WTSSessionInfo.hpp', 'Share/TimeUtils.hpp']:
    shutil.copyfile(root / 'src' / path, snap / path)
files = ['WtCtaTicker.cpp', 'WtCtaTicker.h', 'WtSelTicker.cpp', 'WtSelTicker.h', 'WtHftTicker.cpp', 'WtHftTicker.h']
extra = ['TickerDayBoundary.h']
REGRESS_SOURCES = ['n02_wtcore_engines/r160_hft_ticker_test.cpp', 'q01_wtcore3/r170_ticker_run_test.cpp',
                   'n02_wtcore_engines/r41_ticker_test.cpp', 'q01_wtcore3/r66_tickers_test.cpp',
                   'n02_wtcore_engines/n02_ticker_slicing_test.cpp', 'q01_wtcore3/r181_wtcore_rest_test.cpp']
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
lines = []
for name in files + extra:
    src = tick_root / 'WtCore' / name
    if not src.exists():
        lines.append(f'{name} absent-in-{tick_root}')
        continue
    shutil.copyfile(src, snap / 'WtCore' / name)
    lines.append(f'{name} {digest(src)}')
for path in ['Includes/WTSSessionInfo.hpp', 'Share/TimeUtils.hpp']:
    lines.append(f'{path} {digest(root / "src" / path)}')
# regression test sources always come from the working tree (e.g. the r66 card 3 first-tick update)
for rel in REGRESS_SOURCES:
    shutil.copyfile(root / 'src/TestUnits/coverage' / rel, snap / 'TestUnits/coverage' / rel)
    lines.append(f'TestUnits/coverage/{rel} {digest(root / "src/TestUnits/coverage" / rel)}')
source = snap / 'TestUnits/coverage/q01_wtcore3/r202_wtcore_public_surface_test.cpp'
merged = out / 'merged.cpp'
merged.write_text(source.read_text() + '\n' + (root / 'src/TestUnits/fixtures/r202_nextday/nextday.inc').read_text()
                  + '\n' + (fixture / 'c3_ticker.inc').read_text())
for p in [source, root / 'src/TestUnits/fixtures/r202_nextday/nextday.inc', fixture / 'c3_ticker.inc', merged]:
    lines.append(f'{p} {digest(p)}')
(out / 'hashes.txt').write_text('\n'.join(lines) + '\n')

commands = json.loads((build / 'compile_commands.json').read_text())
san = ['-fsanitize=thread', '-fno-omit-frame-pointer', '-Wno-error'] if tsan else []
def adapt(cmd, src, obj, extra_inc=None):
    a = [x.replace(str(old_snap), str(snap)) for x in shlex.split(cmd)]
    a[a.index('-o') + 1] = str(obj)
    a[a.index('-c') + 1] = str(src)
    if extra_inc:
        for inc in extra_inc:
            a.extend(['-I', str(inc)])
    if tsan:
        # Homebrew clang does not search /usr/local/include (rapidjson) like /usr/bin/c++ does
        a[0] = '/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++'; a[1:1] = ['-include', 'stdlib.h']; a.extend(san + ['-isystem', '/usr/local/include'])
    return a
# every object of the link that depends on a ticker header is recompiled against the copied
# ticker sources/headers (header layout changes, e.g. card 9 strand members / atomics)
deps = subprocess.check_output(['/opt/homebrew/bin/ninja', '-C', str(build), '-t', 'deps'], text=True)
dependents, cur = set(), None
for line in deps.splitlines():
    if line and not line.startswith(' ') and ': #deps' in line:
        cur = line.split(':')[0]
    elif cur and (line.strip().endswith('Ticker.h') or line.strip().endswith('TickerDayBoundary.h')):
        dependents.add(cur)
dependents = sorted(d for d in dependents if 'CoverageTests_wtcore.dir/' in d)
test_obj = 'TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/q01_wtcore3/r202_wtcore_public_surface_test.cpp.o'
by_out = {}
for x in commands:
    a = shlex.split(x['command'])
    if '-o' in a:
        by_out[a[a.index('-o') + 1]] = x
entry = by_out[test_obj]
replaced = {}
with (out / 'build.log').open('w') as log:
    log.write('recompiled dependents: ' + ' '.join(dependents) + '\n')
    subprocess.run(adapt(entry['command'], merged, out / 'test.o', [source.parent]), cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
    replaced[test_obj] = str(out / 'test.o')
    for i, dep in enumerate(dependents):
        if dep == test_obj:
            continue
        e = by_out[dep]
        objp = out / f'dep{i}.o'
        subprocess.run(adapt(e['command'], e['file'].replace(str(old_snap), str(snap)), objp), cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
        replaced[dep] = str(objp)
    link = subprocess.check_output(['/opt/homebrew/bin/ninja', '-C', str(build), '-t', 'commands', 'CoverageTests_wtcore'], text=True).splitlines()[-1]
    base_link = shlex.split(link)[2:-2]
    if tsan:
        base_link[0] = '/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++'
        base_link = [a for a in base_link if a not in ['-Wl,-no_warn_duplicate_libraries', '-Wl,-fatal_warnings']]
        base_link.append('-fsanitize=thread')
    def link_to(binary, test_object):
        la = [replaced.get(a, a) for a in base_link]
        la[la.index(str(out / 'test.o'))] = str(test_object)
        la[la.index('-o') + 1] = str(binary)
        subprocess.run(la, cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
    link_to(out / 'test', out / 'test.o')
    # directly related existing ticker tests (current sources equal the snapshot), one binary each
    regress = []
    if os.environ.get('R202_REGRESS') == '1':
        for rel in REGRESS_SOURCES:
            src = snap / 'TestUnits/coverage' / rel
            name = Path(rel).stem
            objp = out / f'{name}.o'
            subprocess.run(adapt(entry['command'], src, objp, [src.parent, snap / 'TestUnits/coverage']), cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
            link_to(out / f'rg_{name}', objp)
            regress.append(name)

tests = [f'{g}.{n}' for g in ['R202NextDay', 'R202Reentry', 'C3TickerLeapSecond'] for n in ['Cta', 'Sel', 'Hft']]
tests += [f'C3TickerReentry.{k}{n}' for k in ['WorkerQueuedInOrder', 'StrategyQueuedInOrder'] for n in ['Cta', 'Sel', 'Hft']]
tests += [f'C3TickerStop.{k}{n}' for k in ['StopWhileQueuedDrainsAndJoins', 'WorkerStopDoesNotSelfJoin'] for n in ['Cta', 'Sel', 'Hft']]
tests += ['R202CtaTicker.FullDayMidnightCloseEndsSessionAndStoresTradingDate',
          'R202CtaTicker.OrdinaryCloseStillEndsSessionAndStoresTradingDate',
          'R202SelTicker.FullDayMidnightCloseEndsSessionOnce', 'R202SelTicker.OrdinaryCloseStillEndsSession',
          'R202HftTicker.FullDayMidnightCloseEndsSessionOnce', 'R202HftTicker.OrdinaryCloseStillEndsSession',
          'R202HftTicker.DataDrivenMinuteCloseReachesStore', 'R202SelTicker.FullDaySessionKeepsExplicitMidnightCloseLabel',
          'R202HftTicker.FullDaySessionCloseLabelAndClockOffsetAreDistinct', 'R202CtaTicker.ConstructAndStopWithoutEngine']
results = []
runs = [(label, out / 'test', label) for label in tests]
for name in regress:
    listed = subprocess.check_output([str(out / f'rg_{name}'), '--gtest_list_tests'], text=True)
    suite = None
    for line in listed.splitlines():
        if not line.startswith(' '):
            suite = line.strip()
        elif line.strip():
            case = suite + line.strip().split()[0]
            runs.append((f'{name}:{case}', out / f'rg_{name}', case))
for label, binary, filt in runs:
    env = os.environ.copy(); env['LLVM_PROFILE_FILE'] = str(out / (label.replace(':', '__') + '.profraw'))
    if tsan:
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=66'
    safe = label.replace(':', '__').replace('/', '_')
    work = out / safe; work.mkdir()
    with (out / f'{safe}.log').open('w') as log:
        try:
            rc = subprocess.run([str(binary), f'--gtest_filter={filt}', f'--gtest_output=xml:{out / (safe + ".xml")}'],
                                cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=35).returncode
        except subprocess.TimeoutExpired:
            rc = 124
    results.append((label, rc))
(out / 'results.txt').write_text(''.join(f'{a} {b}\n' for a, b in results))
for a, b in results:
    print(a, b)
