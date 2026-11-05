from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import hashlib, json, os, shlex, shutil, subprocess

repo = Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
base = Path('/tmp/i03_groups_20261103')
build = base / 'build'
out = Path('/tmp/i03-groups-root-20261103')
out.mkdir(exist_ok=True)
names = ['src/WtCore/WtLocalExecuter.cpp', 'src/WtCore/WtLocalExecuter.h',
         'src/WtCore/WtArbiExecuter.cpp', 'src/WtCore/WtArbiExecuter.h',
         'src/Share/ExecutionGroups.hpp']
hashes = {}
for name in names:
    current = (repo / name).read_bytes()
    assert current == (base / 'snapshot' / name).read_bytes(), name
    hashes[name] = hashlib.sha256(current).hexdigest()
(out / 'verified-inputs.json').write_text(json.dumps(hashes, indent=2) + '\n')
def commands(target):
    return subprocess.check_output(['/opt/homebrew/bin/ninja', '-t', 'commands', target], cwd=build, text=True).splitlines()
binary = 'build_x64/Debug/bin/TestUnits/CoverageTests_wtcore'
link = shlex.split(commands(binary)[-1])
assert link[:2] == [':', '&&'] and link[-2:] == ['&&', ':']
link = link[2:-2]
compiles = []
for stem in ('WtLocalExecuter', 'WtArbiExecuter'):
    old = 'TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/__/__/WtCore/' + stem + '.cpp.o'
    args = [shlex.split(x) for x in commands(old)]
    args = [x for x in args if '-c' in x and x[-1].endswith('/' + stem + '.cpp')][-1]
    obj = str(out / (stem + '.cpp.o'))
    for flag in ('-o', '-MT'):
        args[args.index(flag) + 1] = obj
    args[args.index('-MF') + 1] = obj + '.d'
    link[link.index(old)] = obj
    compiles.append((stem, args))
link[link.index('-o') + 1] = str(out / 'CoverageTests_wtcore')
(out / 'commands.json').write_text(json.dumps({'compile': compiles, 'link': link}, indent=2) + '\n')
def compile_one(item):
    stem, args = item
    with (out / (stem + '.build.log')).open('w') as log:
        subprocess.run(args, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
with ThreadPoolExecutor(max_workers=2) as pool:
    list(pool.map(compile_one, compiles))
with (out / 'link.log').open('w') as log:
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
for name, expected in hashes.items():
    assert hashlib.sha256((repo / name).read_bytes()).hexdigest() == expected, name
test_filter = (repo / 'src/TestUnits/fixtures/i03_execution_groups_20261103/filter.txt').read_text().strip()
env = dict(os.environ, LLVM_PROFILE_FILE=str(out / 'test.profraw'))
shutil.copy2(build / 'build_x64/Debug/bin/libWtExeFact.dylib', out / 'libWtExeFact.dylib')
with (out / 'test.log').open('w') as log:
    subprocess.run([str(out / 'CoverageTests_wtcore'), '--gtest_filter=' + test_filter,
                    '--gtest_output=xml:' + str(out / 'test.xml')], cwd=out, env=env,
                   stdout=log, stderr=subprocess.STDOUT, timeout=90, check=True)
print('Independent Local/Arbi compile, relink, and focused tests passed:', out)
