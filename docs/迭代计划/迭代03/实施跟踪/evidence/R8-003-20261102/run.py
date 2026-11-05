import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

root = Path('/tmp/wt-i03-r8-003-20261102')
out = Path(__file__).resolve().parent
exe = root / 'build/build_x64/Debug/bin/TestUnits/CoverageTests_dtservo'
cases = [
    'R8WtDtRunnerTest.MissingDataSectionStillCompletesInitialization',
    'R8WtDtRunnerTest.ConfiguredUnavailableReaderRejectsStartupAndQueriesSafely',
    'R8WtDtRunnerTest.DataSectionWithoutStoreRemainsValidNoReaderMode',
    'R8WtDtRunnerTest.ConfiguredAvailableReaderCompletesInitializationWhenPresent',
    'R45ServoApiTest.AllQueryExportsReturnZeroBeforeInitialize',
    'R8ExportNoReaderProbe.NoStoreInitializedQueriesReturnZeroNoCallbacks',
    'R8ExportNoReaderProbe.UnavailableStoreFailsInitializationAndQueriesReturnZeroNoCallbacks',
]
results = []
for i, case in enumerate(cases):
    work = root / ('case-' + str(i))
    work.mkdir(exist_ok=True)
    xml = out / ('result-' + str(i) + '.xml')
    env = dict(os.environ, LLVM_PROFILE_FILE=str(work / 'test.profraw'))
    cmd = [str(exe), '--gtest_filter=' + case, '--gtest_output=xml:' + str(xml)]
    try:
        p = subprocess.run(cmd, cwd=work, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=30, text=True)
        code, log = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        code, log = 124, (e.stdout or b'').decode(errors='replace') + '\nTIMEOUT\n'
    (out / ('result-' + str(i) + '.log')).write_text(log)
    counts = {}
    if xml.exists():
        r = ET.parse(xml).getroot()
        counts = {k: int(r.attrib.get(k, '0')) for k in ('tests', 'failures', 'disabled', 'skipped', 'errors')}
    results.append({'case': case, 'command': cmd, 'cwd': str(work), 'exit': code, 'xml_counts': counts})
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
raise SystemExit(not all(r['exit'] == 0 and r['xml_counts'].get('tests') == 1
                         and r['xml_counts'].get('failures') == 0
                         and r['xml_counts'].get('skipped') == 0 for r in results))
