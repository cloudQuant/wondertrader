import hashlib, json, os, pathlib, subprocess, time, xml.etree.ElementTree as ET
root = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
evidence = root / 'docs/迭代计划/迭代02/复核185/evidence/04_core_engines'
binary = root / 'build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore'
target_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
cases = [
    ('R160EventNotifier.WorkGuardKeepsWorkerAliveForEveryPublishLambda', 'r160_inproc_publish'),
    ('R211Engine.IncompatibleRiskMonModuleIsRejected', 'r211_incompatible_risk_plugin'),
    ('r31_exe_fact.loadfactories_with_real_dylib_reaches_creator_and_registers', 'r31_valid_plugin_factory'),
    ('R42ParserAdapterTest.AdapterManagerAddGetRunRelease', 'r42_adapter_manager_release'),
    ('P02DiffExecuterTest.LoadDataToleratesMissingEmptyAndBrokenFiles', 'p02_diff_syntax_invalid_positive'),
]
results = []
for filt, slug in cases:
    run_dir = evidence / 'runs' / slug
    run_dir.mkdir(parents=True, exist_ok=True)
    xml_path = run_dir / 'gtest.xml'
    profile_path = run_dir / 'profile-%p.profraw'
    cmd = [str(binary), f'--gtest_filter={filt}', f'--gtest_output=xml:{xml_path}']
    env = os.environ.copy()
    env['LLVM_PROFILE_FILE'] = str(profile_path)
    started = time.monotonic()
    try:
        p = subprocess.run(cmd, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        returncode, stdout = p.returncode, p.stdout
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        returncode, stdout, timed_out = None, (exc.stdout or ''), True
        if isinstance(stdout, bytes): stdout = stdout.decode(errors='replace')
    (run_dir / 'stdout.txt').write_text(stdout, encoding='utf-8')
    count = None
    xml_status = None
    if xml_path.exists():
        try:
            doc = ET.parse(xml_path).getroot()
            count = int(doc.attrib.get('tests', '0'))
            xml_status = doc.attrib.get('failures', '0') + ' failures'
        except Exception as exc:
            xml_status = 'parse_error: ' + repr(exc)
    meta = {
        'filter': filt, 'command': cmd, 'cwd': str(run_dir), 'returncode': returncode,
        'timed_out': timed_out, 'elapsed_seconds': round(time.monotonic()-started, 3),
        'target': str(binary.relative_to(root)), 'target_sha256': target_sha,
        'gtest_test_count': count, 'xml_status': xml_status,
        'stdout': str((run_dir/'stdout.txt').relative_to(root)),
        'xml': str(xml_path.relative_to(root)) if xml_path.exists() else None,
        'profile_pattern': str(profile_path.relative_to(root)),
    }
    (run_dir / 'run.json').write_text(json.dumps(meta, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    results.append(meta)
(evidence / 'selected-runs.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print(json.dumps(results, ensure_ascii=False, indent=2))
