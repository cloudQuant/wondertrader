import hashlib
import json
import os
import pathlib
import subprocess
import time
import xml.etree.ElementTree as ET

ROOT = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
EVIDENCE = ROOT / 'docs/迭代计划/迭代02/复核185/evidence/08_ad_storage'
BINARY = ROOT / 'build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_adstorage'
TARGET_SHA = hashlib.sha256(BINARY.read_bytes()).hexdigest()
CASES = [
    ('R21ReaderADTest.RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated', 'r21_repeat_kline'),
    ('R21ReaderADTest.EtimeBetweenTicksExcludesLaterTickFromSlice', 'r21_tick_between_boundaries'),
    ('R4ReaderADTest.FirstTickReadTakesIncrementalRangeAndKeepsLastCount', 'r4_first_count_tick_read'),
    ('P05BtReaderADTest.TickDbCacheKeyNeverMatchesSoEveryCallReopens', 'p05_tick_cache_read'),
    ('R4WriterADTest.FreshInitBuildsFourBlockFilesWithValidADHeader', 'r4_writer_fresh_init_only'),
    ('R4WriterADTest.ReloadExistingCacheClampsSizeAndRebuildsIndex', 'r4_writer_reload_init_only'),
]

results = []
for gtest_filter, slug in CASES:
    run_dir = EVIDENCE / 'runs' / slug
    run_dir.mkdir(parents=True, exist_ok=True)
    xml_path = run_dir / 'gtest.xml'
    profile_path = run_dir / 'profile-%p.profraw'
    command = [str(BINARY), f'--gtest_filter={gtest_filter}', f'--gtest_output=xml:{xml_path}']
    env = os.environ.copy()
    env['LLVM_PROFILE_FILE'] = str(profile_path)
    started = time.monotonic()
    timed_out = False
    try:
        completed = subprocess.run(command, cwd=run_dir, env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=15)
        returncode, stdout = completed.returncode, completed.stdout
    except subprocess.TimeoutExpired as exc:
        returncode, stdout, timed_out = None, exc.stdout or '', True
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors='replace')
    stdout_path = run_dir / 'stdout.txt'
    stdout_path.write_text(stdout, encoding='utf-8')
    test_count = None
    failures = None
    if xml_path.exists():
        try:
            suite = ET.parse(xml_path).getroot()
            test_count = int(suite.attrib.get('tests', '0'))
            failures = int(suite.attrib.get('failures', '0'))
        except Exception as exc:
            failures = f'xml_parse_error: {exc!r}'
    result = {
        'filter': gtest_filter,
        'command': command,
        'cwd': str(run_dir),
        'returncode': returncode,
        'timed_out': timed_out,
        'elapsed_seconds': round(time.monotonic() - started, 3),
        'target': str(BINARY.relative_to(ROOT)),
        'target_sha256': TARGET_SHA,
        'gtest_test_count': test_count,
        'gtest_failures': failures,
        'stdout': str(stdout_path.relative_to(ROOT)),
        'xml': str(xml_path.relative_to(ROOT)) if xml_path.exists() else None,
        'profile_pattern': str(profile_path.relative_to(ROOT)),
    }
    (run_dir / 'run.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    results.append(result)

(EVIDENCE / 'safe-runs.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(results, ensure_ascii=False, indent=2))
