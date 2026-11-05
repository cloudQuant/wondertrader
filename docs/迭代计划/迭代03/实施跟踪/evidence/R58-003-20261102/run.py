from pathlib import Path
import json
import os
import subprocess

out = Path(__file__).resolve().parent
binary = Path('/tmp/wt-i03-r58-20261102-build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore')
cases = ['optionalBasefilesAllLoaded', 'configuredSessionLoadFailuresFailInit',
         'configuredCommodityLoadFailuresFailInit', 'configuredCommodityArrayLoadFailuresFailInit',
         'adjFactorFileParsed',
         'badAdjFactorFileDoesNotBreakInit', 'requiredAndOptionalFailureDiagnosticsContainPaths']
results = []
for case in cases:
    cwd = Path('/tmp/wt-i03-r58-20261102-runs') / ('final-' + case)
    cwd.mkdir(parents=True, exist_ok=True)
    argv = [str(binary), '--gtest_filter=R43ReplayerInitBasefilesTest.' + case,
            '--gtest_output=xml:' + str(out / ('final-' + case + '.xml'))]
    try:
        proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=40,
                              env=dict(os.environ, LLVM_PROFILE_FILE=str(cwd / 'profile-%p.profraw')))
        code, output = proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as exc:
        code, output = 'TIMEOUT', str(exc)
    (out / ('final-' + case + '.log')).write_text(output)
    results.append(dict(case=case, argv=argv, cwd=str(cwd), exit=code))
    print(case, code, flush=True)
(out / 'final-results.json').write_text(json.dumps(results, indent=2) + '\n')
if any(row['exit'] != 0 for row in results):
    raise SystemExit(1)
