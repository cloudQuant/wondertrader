import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time


ROOT = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
EVIDENCE = ROOT / 'docs/迭代计划/迭代02/复核185/evidence/03_core_orders'
BINARY = ROOT / 'build-cov/build_x64/Debug/bin/TestUnits/CoverageTests'
CASE = 'R56BaseDataMgr.ContractRulesAutoCreateCommodityAndWindows'
PREFIX = 'r56_contract_rules_missing_session'

run_cwd = pathlib.Path(tempfile.mkdtemp(prefix='wt-coreorders-r56-'))
xml = EVIDENCE / (PREFIX + '.xml')
profile = EVIDENCE / (PREFIX + '.profraw')
command = [str(BINARY), '--gtest_filter=' + CASE,
           '--gtest_output=xml:' + str(xml)]
env = os.environ.copy()
env['LLVM_PROFILE_FILE'] = str(profile)
start = time.monotonic()
try:
    proc = subprocess.run(command, cwd=run_cwd, env=env, capture_output=True,
                          text=True, timeout=15)
    stdout, stderr, returncode = proc.stdout, proc.stderr, proc.returncode
    timed_out = False
except subprocess.TimeoutExpired as exc:
    stdout = exc.stdout.decode(errors='replace') if isinstance(exc.stdout, bytes) else (exc.stdout or '')
    stderr = exc.stderr.decode(errors='replace') if isinstance(exc.stderr, bytes) else (exc.stderr or '')
    returncode, timed_out = None, True
elapsed = time.monotonic() - start
(EVIDENCE / (PREFIX + '.stdout.log')).write_text(stdout)
(EVIDENCE / (PREFIX + '.stderr.log')).write_text(stderr)
metadata = {
    'command': command,
    'cwd': str(run_cwd),
    'binary': str(BINARY),
    'binary_sha256': hashlib.sha256(BINARY.read_bytes()).hexdigest(),
    'returncode': returncode,
    'timed_out': timed_out,
    'timeout_seconds': 15,
    'elapsed_seconds': round(elapsed, 6),
    'gtest_filter': CASE,
    'gtest_xml': str(xml),
    'llvm_profile_file': str(profile),
    'stdout_log': str(EVIDENCE / (PREFIX + '.stdout.log')),
    'stderr_log': str(EVIDENCE / (PREFIX + '.stderr.log')),
    'running_test_count': None,
    'passed_test_count': None,
    'failed_test_count': None,
}
match = re.search(r'\[==========\] Running (\d+) test', stdout)
if match:
    metadata['running_test_count'] = int(match.group(1))
match = re.search(r'\[  PASSED  \] (\d+) test', stdout)
if match:
    metadata['passed_test_count'] = int(match.group(1))
match = re.search(r'\[  FAILED  \] (\d+) test', stdout)
if match:
    metadata['failed_test_count'] = int(match.group(1))
(EVIDENCE / (PREFIX + '.json')).write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + '\n')
shutil.rmtree(run_cwd, ignore_errors=True)
print(json.dumps(metadata, ensure_ascii=False, indent=2))
print(stdout)
if stderr:
    print('STDERR:\n' + stderr)
