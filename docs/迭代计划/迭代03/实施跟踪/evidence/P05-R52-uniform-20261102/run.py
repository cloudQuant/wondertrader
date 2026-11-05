import json, os, pathlib, subprocess
root = pathlib.Path('/tmp/wt-i03-p05-r52-uniform')
out = pathlib.Path(__file__).resolve().parent
cases = [
 'Q03RdmKlineTest.day_range_and_count_handle_first_cached_bar_boundaries',
 'Q03RdmKlineTest.day_window_out_of_range_returns_null',
 'Q03RdmKlineTest.day_dsb_raw_v2_full_range_roundtrip',
 'R52RdmTest.R52ExrightRealtimeRangeCopiesFirstSingleAndMultipleBars',
 'R52RdmTest.R52ExrightRealtimeRangeSkipsAnEmptyRealtimeBlock',
 'R52RdmTest.R52ExrightRealtimeRangeBoundsBeforeHitAndAfterLast',
 'R52RdmTest.R52ExrightRealtimeCountBoundsBeforeHitAndAfterLast',
]
env = os.environ.copy()
env['ASAN_OPTIONS'] = 'abort_on_error=1:detect_leaks=0'
env['UBSAN_OPTIONS'] = 'print_stacktrace=1:halt_on_error=1'
results = []
for i, case in enumerate(cases):
    cwd = root / ('run-%d' % i)
    cwd.mkdir(exist_ok=True)
    command = [str(root / 'build/bounds'), '--gtest_filter=' + case, '--gtest_output=xml:' + str(out / ('result-%d.xml' % i))]
    try:
        p = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        code, text = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        code, text = 'TIMEOUT', (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
    (out / ('run-%d.log' % i)).write_text(text)
    results.append({'case': case, 'command': command, 'cwd': str(cwd), 'exit': code})
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
raise SystemExit(0 if all(x['exit'] == 0 for x in results) else 1)
