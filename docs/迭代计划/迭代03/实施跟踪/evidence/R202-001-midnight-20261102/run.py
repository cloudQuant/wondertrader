import json
import pathlib
import subprocess
import xml.etree.ElementTree as ET

root = pathlib.Path('/tmp/wt-i03-r202-001-midnight')
binary = root / 'build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore'
tests = [
    'R202CtaTicker.FullDayMidnightCloseEndsSessionAndStoresTradingDate',
    'R202SelTicker.FullDayMidnightCloseEndsSessionOnce',
    'R202HftTicker.FullDayMidnightCloseEndsSessionOnce',
    'R202CtaTicker.OrdinaryCloseStillEndsSessionAndStoresTradingDate',
    'R202SelTicker.OrdinaryCloseStillEndsSession',
    'R202HftTicker.OrdinaryCloseStillEndsSession',
]
results = []
for name in tests:
    directory = root / 'runs' / name
    directory.mkdir(parents=True, exist_ok=True)
    command = [str(binary), '--gtest_filter=' + name, '--gtest_output=xml:result.xml']
    try:
        run = subprocess.run(command, cwd=directory, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=40)
        output = run.stdout
        code = run.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b'') + b'\nTIMEOUT after 40 seconds\n'
        code = 'TIMEOUT'
    (directory / 'output.log').write_bytes(output)
    result = {'test': name, 'command': command, 'cwd': str(directory), 'exit': code}
    xml = directory / 'result.xml'
    if xml.exists():
        parsed = ET.parse(xml).getroot()
        result.update({key: parsed.attrib.get(key) for key in ('tests', 'failures', 'disabled', 'errors')})
        result['skipped_cases'] = len(parsed.findall('.//skipped'))
    results.append(result)
(root / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
print(json.dumps(results, ensure_ascii=False, indent=2))
