import json
import os
from pathlib import Path
import subprocess

root = Path('/tmp/wt-i03-l06-fees-20261102')
evidence = Path(__file__).resolve().parent
binary = root / 'build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore'
cases = [
    'amountBasedFeeRequiresCommodityAtInit',
    'amountBasedFeeUsesCommodityVolumeScale',
    'volumeBasedFeeDoesNotRequireCommodity',
]
results = []
for case in cases:
    work = root / ('archived-' + case)
    work.mkdir(parents=True, exist_ok=True)
    xml = evidence / (case + '.xml')
    command = [str(binary), '--gtest_filter=HisDataReplayerRuntimeTest.' + case,
               '--gtest_output=xml:' + str(xml)]
    env = os.environ.copy()
    env['LLVM_PROFILE_FILE'] = str(work / 'default.profraw')
    try:
        result = subprocess.run(command, cwd=work, env=env, capture_output=True,
                                text=True, timeout=60)
        code, output, timed_out = result.returncode, result.stdout + result.stderr, False
    except subprocess.TimeoutExpired as exc:
        code, timed_out = None, True
        output = str(exc.stdout or b'') + str(exc.stderr or b'')
    (evidence / (case + '.log')).write_text(output)
    results.append({'case': case, 'command': command, 'cwd': str(work),
                    'exit_code': code, 'timed_out': timed_out})
(evidence / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
if any(item['exit_code'] != 0 or item['timed_out'] for item in results):
    raise SystemExit(1)
