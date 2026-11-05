from pathlib import Path
import hashlib
import json
import os
import shlex
import subprocess
import sys

repo = Path.cwd()
evidence = repo / 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-034-fund-owner-20261102'
old = Path('/tmp/wt-i03-r42-001-20261102')
snapshot = old / 'snapshot/src'
build = old / 'build'
work = Path('/tmp/wt-i03-new034-fund-owner-20261102')
work.mkdir(exist_ok=True)
build_env = os.environ.copy()
build_env['PATH'] = '/opt/homebrew/Cellar/llvm/22.1.8/bin:/opt/homebrew/bin:/usr/bin:/bin'
for rel in ('WtCore/WtEngine.cpp', 'WtCore/WtEngine.h', 'WtCore/WtSelEngine.cpp',
            'WtCore/WtSelEngine.h', 'Includes/WTSRiskDef.hpp', 'Includes/RiskMonDefs.h'):
    current = (repo / 'src' / rel).read_bytes()
    former = (snapshot / rel).read_bytes()
    if current != former:
        raise RuntimeError(f'snapshot differs from current source: {rel}')
    print(hashlib.sha256(current).hexdigest(), rel)

commands = json.loads((build / 'compile_commands.json').read_text())
item = next(x for x in commands if x['file'].endswith('/r42_sel_ctx_test.cpp'))
compile_cmd = shlex.split(item['command'])
for flag in ('-o', '-c', '-MF', '-MT'):
    if flag not in compile_cmd:
        continue
    i = compile_cmd.index(flag)
    compile_cmd[i + 1] = str({'-o': work / 'monitor_fund_test.o',
                               '-c': evidence / 'monitor_fund_test.cpp',
                               '-MF': work / 'monitor_fund_test.d',
                               '-MT': 'monitor_fund_test.o'}[flag])
with (work / 'compile.log').open('w') as log:
    subprocess.run(compile_cmd, cwd=item['directory'], env=build_env, stdout=log, stderr=subprocess.STDOUT, check=True)

link_line = next(line for line in (repo / 'docs/迭代计划/迭代03/实施跟踪/evidence/R42-001-20261102/build-commands.txt').read_text().splitlines()
                 if line.startswith(': && ') and 'CoverageTests_wtcore.dir/p02_wtcore_engines2/r42_sel_ctx_test.cpp.o' in line)
link_cmd = shlex.split(link_line[len(': && '):].removesuffix(' && :'))
test_object = next(i for i, arg in enumerate(link_cmd) if arg.endswith('/r42_sel_ctx_test.cpp.o'))
link_cmd[test_object] = str(work / 'monitor_fund_test.o')
link_cmd[link_cmd.index('-o') + 1] = str(work / 'monitor_fund_test')
with (work / 'link.log').open('w') as log:
    subprocess.run(link_cmd, cwd=build, env=build_env, stdout=log, stderr=subprocess.STDOUT, check=True)

env = os.environ.copy()
env['ASAN_OPTIONS'] = 'detect_leaks=1:halt_on_error=1'
env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
env.pop('LLVM_PROFILE_FILE', None)
tests = [('observer', work / 'monitor_fund_test', 'FundOwner.ExistingReleaseDoesNotStopExternallyHeldMonitor'),
         ('initialization', build / 'build_x64/Debug/bin/TestUnits/CoverageTests_wtcore',
          'R42SelEngineLifecycle.SuccessfulInitializationReleasesConfigAfterDestruction')]
for name, binary, test in tests:
    directory = work / name
    directory.mkdir(exist_ok=True)
    with (evidence / (name + '.log')).open('w') as log:
        run = subprocess.run([str(binary), '--gtest_filter=' + test], cwd=directory, env=env,
                             stdout=log, stderr=subprocess.STDOUT, timeout=40)
    print(name, 'exit', run.returncode)
