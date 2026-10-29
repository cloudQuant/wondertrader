import hashlib, json, os, pathlib, shutil, subprocess, tempfile, time
root = pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
evidence = root / 'docs/迭代计划/迭代02/复核185/evidence/11_runners'
bin_dir = root / 'build-cov/build_x64/Debug/bin/TestUnits'
cases = [
    ('uft_zero_ticks', 'CoverageTests_m_WtLatencyUFT', 'r95_ufttool_run.zero_ticks_after_successful_init_exercises_engine_and_parser_tail'),
    ('hft_zero_ticks', 'CoverageTests_m_WtLatencyHFT', 'r95_hft_run.zero_ticks_after_successful_init_exercises_engine_and_parser_tail'),
    ('uft_valid_config', 'CoverageTests_m_WtUftRunner', 'r95_uft_config.string_basefiles_and_array_modules_return_true'),
    ('porter_registered_ext_quote', 'CoverageTests_porter', 'R82PorterCapiReal.ExtParserQuoteSuccess'),
    ('porter_unknown_quote', 'CoverageTests_porter', 'R6PorterExtParserQuote.unknown_parser_leaves_the_supplied_tick_untouched'),
    ('porter_unknown_capi_quote', 'CoverageTests_porter', 'Q06PorterCapi.ParserPushQuoteIgnoresUnknownParserId'),
    ('porter_null_tick_unknown_id', 'CoverageTests_porter', 'Q06PorterCapi.CallbackRegistrationAcceptsNullAndReplaces'),
]
results = []
for key, target, filt in cases:
    binary = bin_dir / target
    if not binary.is_file():
        raise FileNotFoundError(binary)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    work = pathlib.Path(tempfile.mkdtemp(prefix=f'wtrunners_{key}_'))
    prof_dir = evidence / 'profiles'
    prof_dir.mkdir(exist_ok=True)
    xml = evidence / f'{key}.xml'
    stdout_path = evidence / f'{key}.stdout.txt'
    meta_path = evidence / f'{key}.json'
    env = os.environ.copy()
    env['LLVM_PROFILE_FILE'] = str(prof_dir / f'{key}-%p.profraw')
    command = [str(binary), f'--gtest_filter={filt}', f'--gtest_output=xml:{xml}']
    started = time.monotonic()
    timed_out = False
    try:
        cp = subprocess.run(command, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
        rc, output = cp.returncode, cp.stdout
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        rc = None
        raw = exc.stdout or b''
        output = raw.decode(errors='replace') if isinstance(raw, bytes) else raw
    elapsed = time.monotonic() - started
    stdout_path.write_text(output, encoding='utf-8')
    try:
        shutil.rmtree(work)
        cleanup = 'removed'
    except Exception as exc:
        cleanup = str(exc)
    xml_stats = None
    if xml.exists():
        import xml.etree.ElementTree as ET
        xml_root = ET.parse(xml).getroot()
        xml_stats = {k: xml_root.attrib.get(k) for k in ('tests', 'failures', 'disabled', 'errors', 'time')}
    result = {
        'case': key, 'target': target, 'binary': str(binary.relative_to(root)),
        'binary_sha256': digest, 'filter': filt, 'command': command,
        'cwd': str(work), 'cwd_initially_empty': True, 'returncode': rc,
        'timed_out': timed_out, 'timeout_seconds': 15, 'elapsed_seconds': round(elapsed, 3),
        'xml': str(xml.relative_to(root)) if xml.exists() else None,
        'xml_stats': xml_stats, 'stdout': str(stdout_path.relative_to(root)),
        'cleanup': cleanup,
    }
    meta_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    results.append(result)
summary = {'evidence_kind': 'single-filter enabled GTest; each case separate subprocess and unique empty cwd; no code/build changed', 'cases': results}
(evidence / 'safe_enabled_runs.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(summary, ensure_ascii=False, indent=2))
