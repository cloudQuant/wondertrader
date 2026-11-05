"""Recompile the four current AD translation units using an existing Ninja build."""
import pathlib
import shlex
import subprocess
import sys

build = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
root = pathlib.Path(__file__).resolve().parents[4] / 'src'
cache = (build / 'CMakeCache.txt').read_text()
old = next(line.split('=', 1)[1] for line in cache.splitlines()
           if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL='))
commands = subprocess.check_output([
    '/Users/yunjinqi/opt/anaconda3/bin/ninja', '-C', str(build), '-t',
    'commands', 'WtDataStorageAD'], text=True).splitlines()
for name in ('WtBtDtReaderAD', 'WtDataReaderAD', 'WtDataWriterAD', 'WtRdmDtReaderAD'):
    line = next(line for line in commands if name + '.cpp.o -c ' in line)
    args = [arg.replace(old, str(root)) for arg in shlex.split(line)]
    for flag in ('-MT', '-MF'):
        index = args.index(flag)
        del args[index:index + 2]
    args[args.index('-o') + 1] = str(out / (name + '.o'))
    print(name, flush=True)
    subprocess.run(args, cwd=build, check=True)
