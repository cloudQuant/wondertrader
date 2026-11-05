import json
import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

base = Path('/tmp/wt-i03-new091-20261102')
evidence = Path(__file__).resolve().parent
binary = base / 'build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore'
filters = {
    'r131': 'R131UftOrder.PartialFillKeepsOrderUntilNextTick',
    'r161_uft': 'R161ReplayFixture.R161UftCover_PartialExitReleasesNewAvail',
    'r161_chart': 'R161ReplayFixture.R161CtaChart_MarkAndIndexInsideSchedule',
    'r161_exright': 'R161ReplayFixture.R161CtaExright_StripsSuffixAfterBarsLoaded',
    'related': 'R131UftOrder.*:R161ReplayFixture.*',
}
env = os.environ.copy()
env['DYLD_LIBRARY_PATH'] = str(base / 'build/build_x64/Debug/bin') + ':/opt/homebrew/lib:/usr/local/lib'
results = []
for name, pattern in filters.items():
    cwd = base / 'runs' / name
    cwd.mkdir(parents=True, exist_ok=True)
    xml = evidence / (name + '.xml')
    env['LLVM_PROFILE_FILE'] = str(cwd / 'profile.profraw')
    cmd = [str(binary), '--gtest_filter=' + pattern, '--gtest_output=xml:' + str(xml)]
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=60)
        code, output = p.returncode, p.stdout
    except subprocess.TimeoutExpired as ex:
        code, output = 'TIMEOUT', ex.stdout or b''
    (evidence / (name + '.log')).write_bytes(output)
    row = {'name': name, 'filter': pattern, 'command': cmd, 'cwd': str(cwd), 'exit': code}
    if xml.exists():
        root = ET.parse(xml).getroot()
        row['tests'] = int(root.attrib.get('tests', 0))
        row['failures'] = int(root.attrib.get('failures', 0))
        row['disabled'] = int(root.attrib.get('disabled', 0))
        row['skipped'] = int(root.attrib.get('skipped', 0))
    results.append(row)
(evidence / 'results.json').write_text(json.dumps(results, indent=2, ensure_ascii=False) + '\n')
print(json.dumps(results, indent=2, ensure_ascii=False))
expected = {'r131': 1, 'r161_uft': 1, 'r161_chart': 1, 'r161_exright': 1, 'related': 10}
if any(row['exit'] != 0 or row.get('tests') != expected[row['name']]
       or row.get('failures') != 0 or row.get('skipped') != 0 for row in results):
    sys.exit(1)
