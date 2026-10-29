import collections
import json
import pathlib
import xml.etree.ElementTree as ET

ROOT = pathlib.Path.cwd()
OUT = ROOT / 'docs/迭代计划/迭代02/复核185'
rows = json.loads((OUT / 'triage/04_core_engines.json').read_text())
source = json.loads(pathlib.Path('/tmp/wt-pending185-20261029T023202Z/groups/04_core_engines.json').read_text())
required = {'lane','id','decision','evidence_level','source_evidence','test_evidence','input_reachability','oracle_basis','reason','root_cause','fix','acceptance','canonical','test_change_requests','execution_refs','decision_request','external_blocker','local_remaining'}
errors = []
if len(rows) != source['count']:
    errors.append(f'count {len(rows)} != {source["count"]}')
if {(x['lane'],x['id']) for x in rows} != {(x['lane'],x['id']) for x in source['rows']}:
    errors.append('input lane/id keys differ')
for row in rows:
    if set(row) != required:
        errors.append(f'{row.get("id")}: required fields mismatch')
    if row['decision'] == 'NEEDS_DECISION' and not isinstance(row['decision_request'], dict):
        errors.append(f'{row["id"]}: missing decision_request')
    if row['decision'] != 'NEEDS_DECISION' and row['decision_request'] is not None:
        errors.append(f'{row["id"]}: unexpected decision_request')
    for ref in row['source_evidence'] + row['test_evidence']:
        try:
            file, location = ref.rsplit(':', 1)
            start_text, _, end_text = location.partition('-')
            start = int(start_text)
            end = int(end_text or start_text)
            lines = (ROOT / file).read_text(errors='replace').splitlines()
            if start < 1 or end > len(lines) or end < start:
                errors.append(f'{row["id"]}: invalid line reference {ref}; file has {len(lines)} lines')
        except Exception as exc:
            errors.append(f'{row["id"]}: invalid reference {ref}: {exc}')
    for ref in row['execution_refs']:
        if not (ROOT / ref).is_file():
            errors.append(f'{row["id"]}: missing execution reference {ref}')
    for request in row['test_change_requests']:
        if set(request) != {'file','case','issue','change','target','filter','expected_result'}:
            errors.append(f'{row["id"]}: test_change_request fields mismatch')
        if not (ROOT / request['file']).is_file():
            errors.append(f'{row["id"]}: missing test file {request["file"]}')
manifest = json.loads((OUT / 'evidence/04_core_engines/selected-runs.json').read_text())
for run in manifest:
    root = ET.parse(ROOT / run['xml']).getroot()
    tests = int(root.attrib.get('tests', '0'))
    failures = int(root.attrib.get('failures', '-1'))
    if tests != run['gtest_test_count'] or failures != 0:
        errors.append(f'{run["filter"]}: XML tests/failures mismatch')
    if run['returncode'] != 0 or run['timed_out'] or run['xml_status'] != '0 failures':
        errors.append(f'{run["filter"]}: execution status mismatch')
print('rows', len(rows), 'decisions', dict(collections.Counter(x['decision'] for x in rows)), 'levels', dict(collections.Counter(x['evidence_level'] for x in rows)))
print('safe single tests', len(manifest), 'test count', sum(x['gtest_test_count'] for x in manifest))
print('validation errors', errors)
raise SystemExit(bool(errors))
