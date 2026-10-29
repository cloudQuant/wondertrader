import hashlib
import json
import os
import pathlib
import subprocess
import time

root = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
evidence = root / 'docs/迭代计划/迭代02/复核185/evidence/13_execution_share'
binary = root / 'build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_mqshare'
test_filter = 'R19TempDir.r19_second_init_master_preserves_first_domain_data'
run_dir = evidence / 'r19-004-current-cwd'
run_dir.mkdir(parents=True, exist_ok=True)
stdout_path = evidence / 'r19-004-current.stdout.txt'
xml_path = evidence / 'r19-004-current.xml'
profile_path = evidence / 'r19-004-current.profraw'
started = time.time()
sha256 = hashlib.sha256(binary.read_bytes()).hexdigest()
env = os.environ.copy()
env['LLVM_PROFILE_FILE'] = str(profile_path)
try:
    completed = subprocess.run(
        [str(binary), f'--gtest_filter={test_filter}', f'--gtest_output=xml:{xml_path}'],
        cwd=run_dir,
        env=env,
        capture_output=True,
        text=True,
        timeout=15,
    )
    stdout = completed.stdout + completed.stderr
    stdout_path.write_text(stdout, encoding='utf-8')
    outcome = {
        'command': [str(binary), f'--gtest_filter={test_filter}', f'--gtest_output=xml:{xml_path}'],
        'cwd': str(run_dir),
        'target_sha256': sha256,
        'returncode': completed.returncode,
        'timed_out': False,
        'elapsed_seconds': round(time.time() - started, 3),
        'gtest_filter': test_filter,
        'xml': str(xml_path),
        'stdout': str(stdout_path),
        'profile': str(profile_path) if profile_path.exists() else None,
        'stdout_tail': stdout[-4000:],
    }
except subprocess.TimeoutExpired as error:
    stdout_path.write_text((error.stdout or '') + (error.stderr or ''), encoding='utf-8')
    outcome = {
        'command': [str(binary), f'--gtest_filter={test_filter}', f'--gtest_output=xml:{xml_path}'],
        'cwd': str(run_dir),
        'target_sha256': sha256,
        'returncode': None,
        'timed_out': True,
        'elapsed_seconds': round(time.time() - started, 3),
        'gtest_filter': test_filter,
        'xml': str(xml_path) if xml_path.exists() else None,
        'stdout': str(stdout_path),
        'profile': str(profile_path) if profile_path.exists() else None,
        'stdout_tail': 'timeout',
    }
(evidence / 'r19-004-current.json').write_text(json.dumps(outcome, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(outcome, ensure_ascii=False, indent=2))
