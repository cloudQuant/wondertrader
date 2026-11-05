import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

root = Path('/tmp/wt-i03-r14-003-20261102')
binary_dir = root / 'build-physical/build_x64/Debug/bin/TestUnits'
evidence = Path(__file__).resolve().parent
cases = {
    'QuoteFactory': [
        'r14_quote_entry.valid_syntax_without_basefiles_fails_before_runner_start',
        'r14_quote_entry.non_object_basefiles_fails_before_runner_start',
        'r14_quote_initialize.valid_basefiles_object_remains_accepted',
    ],
    'WtUftRunner': [
        'r14_uft_config.missing_basefiles_is_reported_and_rejected_before_run',
        'r14_uft_config.non_object_basefiles_is_reported_and_rejected',
        'r14_uft_config.object_basefiles_passes_validation_and_reaches_env_check',
        'r14_uft_entry.missing_basefiles_returns_nonzero_without_running_engine',
    ],
    'WtLatencyHFT': [
        'r14_lazy_hft_tool.init_rejects_missing_basefiles_and_run_does_not_start',
        'r14_lazy_hft_tool.init_rejects_non_object_basefiles',
        'r14_lazy_hft_tool.init_accepts_object_basefiles_control_without_running',
    ],
    'WtLatencyUFT': [
        'r14_lazy_uft_tool.init_rejects_missing_basefiles_before_run',
        'r14_lazy_uft_tool.init_rejects_array_basefiles_before_run',
        'r14_lazy_uft_tool.init_rejects_scalar_basefiles_before_run',
        'r14_lazy_uft_tool.init_accepts_object_basefiles_without_entering_run',
    ],
}
results = []
for module, names in cases.items():
    for index, name in enumerate(names):
        label = f'{module}-{index + 1}'
        cwd = root / 'runs' / label
        cwd.mkdir(parents=True, exist_ok=False)
        xml = evidence / f'{label}.xml'
        command = [str(binary_dir / f'CoverageTests_m_{module}'), f'--gtest_filter={name}', f'--gtest_output=xml:{xml}']
        env = os.environ.copy()
        env['LLVM_PROFILE_FILE'] = str(cwd / '%p.profraw')
        try:
            proc = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=30)
            code, output = proc.returncode, proc.stdout
        except subprocess.TimeoutExpired as exc:
            code = 'TIMEOUT'
            output = (exc.stdout or b'').decode(errors='replace') if isinstance(exc.stdout, bytes) else (exc.stdout or '')
        (evidence / f'{label}.log').write_text(output)
        item = {'label': label, 'case': name, 'command': command, 'cwd': str(cwd), 'exit': code}
        if xml.exists():
            report = ET.parse(xml).getroot()
            item['xml'] = {k: report.get(k) for k in ('tests', 'failures', 'disabled', 'errors')}
        results.append(item)
        print(label, name, code, item.get('xml'), flush=True)
(evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
if any(r['exit'] != 0 or r.get('xml', {}).get('tests') != '1' or r['xml'].get('failures') != '0' for r in results):
    raise SystemExit(1)
