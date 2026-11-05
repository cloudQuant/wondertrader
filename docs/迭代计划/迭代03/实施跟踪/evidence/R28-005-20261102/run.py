from pathlib import Path
import json
import os
import shutil
import subprocess
import xml.etree.ElementTree as ET

base = Path('/tmp/wt-i03-r28-005-20261102')
evidence = Path(__file__).resolve().parent
bin_dir = base / 'build/build_x64/Debug/bin/TestUnits'
cases = {
    'CoverageTests_wtcore': [
        'N02ExecMgrTest.LoadRouterRulesRejectsNullAndNonArray',
        'N02ExecMgrTest.LoadRouterRulesAcceptsStringAndArrayExecuters',
        'N02ExecMgrTest.RouterRuleWithoutExecuterNodeIsRejected',
        'N02ExecMgrTest.MalformedRouterRulesDoNotPartiallyReplaceExistingRoutes',
        'N02ExecMgrTest.RouterRuleWithoutStrategyIsRejected',
    ],
    'CoverageTests_m_WtRunner': [
        'r28_wtrunner_config.router_rule_without_executer_key_fails_configuration',
        'r28_wtrunner_config.router_rule_without_strategy_fails_configuration',
        'r28_wtrunner_config.router_rule_with_non_string_strategy_json_fails_configuration',
        'r28_wtrunner_config.routers_section_reaches_load_router_rules',
    ],
}
records = []
for exe, tests in cases.items():
    for index, case in enumerate(tests):
        label = exe + '-' + str(index)
        work = base / 'runs' / label
        work.mkdir(parents=True, exist_ok=True)
        if exe == 'CoverageTests_m_WtRunner':
            shutil.copy2(bin_dir.parent / 'libWtDataStorage.dylib', work / 'libWtDataStorage.dylib')
        xml = evidence / (label + '.xml')
        cmd = [str(bin_dir / exe), '--gtest_filter=' + case, '--gtest_output=xml:' + str(xml)]
        env = dict(os.environ, LLVM_PROFILE_FILE=str(work / 'profile-%p.profraw'))
        try:
            res = subprocess.run(cmd, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
            exitcode, out = res.returncode, res.stdout
        except subprocess.TimeoutExpired as exc:
            exitcode, out = 'TIMEOUT', (exc.stdout or b'').decode(errors='replace') if isinstance(exc.stdout, bytes) else (exc.stdout or '')
        (evidence / (label + '.log')).write_text(out)
        counts = None
        if xml.exists():
            root = ET.parse(xml).getroot()
            counts = {key: root.get(key) for key in ('tests', 'failures', 'disabled', 'skipped', 'errors')}
        records.append({'case': case, 'command': cmd, 'cwd': str(work), 'exit': exitcode, 'xml': counts})
(evidence / 'results.json').write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n')
for r in records:
    print(r['case'], r['exit'], r['xml'])
if any(r['exit'] != 0 or not r['xml'] or r['xml']['tests'] != '1' or r['xml']['failures'] != '0' or r['xml']['skipped'] not in ('0', None) for r in records):
    raise SystemExit(1)
