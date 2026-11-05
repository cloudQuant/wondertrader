from pathlib import Path
import hashlib
import json
import shlex
import subprocess

repo = Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
base = Path('/tmp/i03_diff_batch_atomic_20261103')
build = base / 'build'
out = Path('/tmp/i03-diff-batch-root-20261103')
out.mkdir(exist_ok=True)
inputs = ['src/WtCore/WtDiffExecuter.cpp', 'src/WtCore/WtDiffExecuter.h',
          'src/WtCore/TraderAdapter.cpp', 'src/WtCore/TraderAdapter.h',
          'src/TestUnits/coverage/n02_wtcore_engines/n02_diff_concurrency_test.cpp']
hashes = {}
for name in inputs:
    current = (repo / name).read_bytes()
    frozen = (base / 'snapshot' / name).read_bytes()
    if current != frozen:
        raise RuntimeError('Current/frozen input mismatch: ' + name)
    hashes[name] = hashlib.sha256(current).hexdigest()
(out / 'verified-inputs.json').write_text(json.dumps(hashes, indent=2) + '\n')
obj = 'TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/__/__/WtCore/WtDiffExecuter.cpp.o'
binary = 'build_x64/Debug/bin/TestUnits/CoverageTests_wtcore'
def commands(target):
    return subprocess.check_output(['/opt/homebrew/bin/ninja', '-t', 'commands', target], cwd=build, text=True).splitlines()
compiles = [shlex.split(line) for line in commands(obj)]
compile_args = [args for args in compiles if '-c' in args and args[-1].endswith('/WtCore/WtDiffExecuter.cpp')][-1]
new_obj = out / 'WtDiffExecuter.cpp.o'
for flag in ('-o', '-MT'):
    compile_args[compile_args.index(flag) + 1] = str(new_obj)
compile_args[compile_args.index('-MF') + 1] = str(new_obj) + '.d'
link_args = shlex.split(commands(binary)[-1])
if link_args[:2] != [':', '&&'] or link_args[-2:] != ['&&', ':']:
    raise RuntimeError('Unexpected generated link wrapper')
link_args = link_args[2:-2]
link_args[link_args.index(obj)] = str(new_obj)
new_binary = out / 'CoverageTests_wtcore'
link_args[link_args.index('-o') + 1] = str(new_binary)
(out / 'commands.json').write_text(json.dumps({'compile': compile_args, 'link': link_args}, indent=2) + '\n')
with (out / 'build.log').open('w') as log:
    subprocess.run(compile_args, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(link_args, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
for name, expected in hashes.items():
    if hashlib.sha256((repo / name).read_bytes()).hexdigest() != expected:
        raise RuntimeError('Input changed during build: ' + name)
print('Independent WtDiffExecuter compile and relink completed:', new_binary)
