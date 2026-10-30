from pathlib import Path
import shlex
import subprocess

root = Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
command_file = Path('/tmp/wt-i02-l03-asan/ninja-commands.txt')
out = Path('/tmp/wt-i02-l03-asan')
sources = {
    'WTSBaseDataMgr.cpp.o': out / 'WTSBaseDataMgr.asan.o',
    'l03_commodity_info_test.cpp.o': out / 'l03_commodity_info_test.asan.o',
    'l03_contract_info_test.cpp.o': out / 'l03_contract_info_test.asan.o',
}
for line in command_file.read_text().splitlines():
    suffix = next((key for key in sources if key in line and ' -c ' in line), None)
    if suffix is None:
        continue
    args = shlex.split(line)
    cleaned = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == '-MD':
            i += 1
            continue
        if arg in ('-MF', '-MT'):
            i += 2
            continue
        if arg == '-o':
            cleaned.extend(('-o', str(sources[suffix])))
            i += 2
            continue
        if arg in ('-fprofile-instr-generate', '-fcoverage-mapping'):
            i += 1
            continue
        cleaned.append(arg)
        i += 1
    cleaned.insert(1, '-fsanitize=address')
    cleaned.insert(2, '-fno-omit-frame-pointer')
    print('COMPILE', suffix, flush=True)
    print(' '.join(shlex.quote(a) for a in cleaned), flush=True)
    result = subprocess.run(cleaned, cwd=root)
    if result.returncode:
        raise SystemExit(result.returncode)
print('ASAN_OBJECTS_COMPILED')
