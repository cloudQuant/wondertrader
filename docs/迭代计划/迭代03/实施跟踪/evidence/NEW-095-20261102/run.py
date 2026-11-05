import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

base = Path('/tmp/wt-i03-new095-20261102')
evidence = Path(__file__).resolve().parent
binary = base / 'build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore'
filters = {
    'm5': 'R180LoaderFixture.R180His_LoaderM5AutoTransThenDsbReuse',
    'corrupt': 'R180LoaderFixture.R180His_CorruptAutoTransDsbRebuiltFromLoader',
    'unbars': 'R180LoaderFixture.R180His_CheckUnbarsMinute5UsesUnbarsCache',
    'bar_replay': 'R210Fixture.R210His_BarReplaySkipsEmptyL2Lists',
    'raw_v2': 'R210Fixture.R210His_UncompressedV2BarBlockHeaderStripped',
    'related': 'R180LoaderFixture.*:R180ReplayFixture.*:R210Fixture.*',
}
env = os.environ.copy()
env['DYLD_LIBRARY_PATH'] = str(base / 'build/build_x64/Debug/bin') + ':/opt/homebrew/lib:/usr/local/lib'
rows = []
for name, pattern in filters.items():
    cwd = base / 'runs' / name
    cwd.mkdir(parents=True, exist_ok=True)
    env['LLVM_PROFILE_FILE'] = str(cwd / 'profile.profraw')
    xml = evidence / (name + '.xml')
    cmd = [str(binary), '--gtest_filter=' + pattern, '--gtest_output=xml:' + str(xml)]
    try:
        proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=90)
        code, output = proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as exc:
        code, output = 'TIMEOUT', exc.stdout or b''
    (evidence / (name + '.log')).write_bytes(output)
    row = {'name': name, 'filter': pattern, 'command': cmd, 'cwd': str(cwd), 'exit': code}
    if xml.exists():
        root = ET.parse(xml).getroot()
        for field in ('tests', 'failures', 'disabled', 'skipped'):
            row[field] = int(root.attrib.get(field, 0))
    rows.append(row)
(evidence / 'results.json').write_text(json.dumps(rows, indent=2, ensure_ascii=False) + '\n')
print(json.dumps(rows, indent=2, ensure_ascii=False))
