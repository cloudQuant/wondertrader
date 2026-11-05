from pathlib import Path
import json
import os
import subprocess

root = Path('/tmp/wt-i03-r28-005-20261102')
evidence = Path(__file__).resolve().parent
binary = root / 'build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore'
cases = ['JsonNullRouterItemMustNotDisappear', 'YamlNullRouterItemMustNotDisappear',
         'JsonNullExecuterElementMustNotDisappear', 'JsonNullOnlyRuleMustNotDisappear']
results = []
for case in cases:
    work = root / 'runs' / ('probe-' + case)
    work.mkdir(parents=True, exist_ok=True)
    xml = evidence / ('probe-' + case + '.xml')
    cmd = [str(binary), '--gtest_filter=R28NullProbe.' + case, '--gtest_output=xml:' + str(xml)]
    env = dict(os.environ, LLVM_PROFILE_FILE=str(work / 'profile-%p.profraw'))
    try:
        res = subprocess.run(cmd, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        code, output = res.returncode, res.stdout
    except subprocess.TimeoutExpired as exc:
        code = 'TIMEOUT'
        output = (exc.stdout or b'').decode(errors='replace') if isinstance(exc.stdout, bytes) else (exc.stdout or '')
    (evidence / ('probe-' + case + '.log')).write_text(output)
    results.append({'case': case, 'command': cmd, 'cwd': str(work), 'exit': code})
(evidence / 'probe-results.json').write_text(json.dumps(results, indent=2) + '\n')
for result in results:
    print(result['case'], result['exit'])
