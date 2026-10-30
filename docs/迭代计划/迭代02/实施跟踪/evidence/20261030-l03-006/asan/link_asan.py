from pathlib import Path
import shlex
import subprocess

root = Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
build = root / 'build-cov'
command_file = Path('/tmp/wt-i02-l03-asan/ninja-commands.txt')
line = next(x for x in command_file.read_text().splitlines()
            if x.startswith(': && /usr/bin/c++') and ' -o build_x64/Debug/bin/TestUnits/CoverageTests ' in x)
command = line.split('&&', 1)[1].rsplit('&&', 1)[0].strip()
args = shlex.split(command)

filtered = []
i = 0
while i < len(args):
    arg = args[i]
    if arg.startswith('TestUnits/coverage/CMakeFiles/CoverageTests.dir/') and arg.endswith('.o'):
        i += 1
        continue
    if arg == '-o':
        filtered.extend(('-o', '/tmp/wt-i02-l03-asan/CoverageTests_l03_asan'))
        i += 2
        continue
    filtered.append(arg)
    i += 1
objs = [
    '/tmp/wt-i02-l03-asan/l03_commodity_info_test.asan.o',
    '/tmp/wt-i02-l03-asan/l03_contract_info_test.asan.o',
    '/tmp/wt-i02-l03-asan/WTSBaseDataMgr.asan.o',
]
filtered[1:1] = ['-fsanitize=address', '-fno-omit-frame-pointer', *objs]
print('LINK', flush=True)
print(' '.join(shlex.quote(a) for a in filtered), flush=True)
result = subprocess.run(filtered, cwd=build)
raise SystemExit(result.returncode)
