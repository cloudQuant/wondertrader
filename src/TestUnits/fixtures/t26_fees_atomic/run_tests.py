import json
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
modes = ['valid', 'absolute', 'empty_terminal', 'filter', 'missing_fee', 'missing_contract',
         'nan_fee', 'nan_margin', 'commission_error', 'response_error', 'write',
         'flush', 'close', 'rename', 'openfail', 'disk_limit', 'empty_data', 'missing_margin',
         'empty_exchange', 'empty_code', 'empty_product', 'duplicate_key', 'earlier_error', 'earlier_margin_error',
         'loaded_alt', 'loaded_mixed', 'loaded_duplicate', 'loaded_product', 'loaded_alias_conflict',
         'loaded_fee_conflict', 'loaded_duplicate_margin', 'units_tiny_money', 'units_mixed_open_volume',
         'units_mixed_open_money', 'units_mixed_same', 'units_mixed_tiny', 'margin_zero', 'margin_different_short',
         'margin_long_volume', 'margin_short_volume', 'filter_unselected_invalid'] + ['nan_field_'+str(i) for i in range(6)] + ['margin_nan_'+str(i) for i in range(4)]
modes += [f'sentinel_fee_{kind}_{i}' for kind in ('d', 'f') for i in range(6)]
modes += [f'sentinel_margin_{kind}_{i}' for kind in ('d', 'f') for i in range(4)]
success = {'valid', 'absolute', 'empty_terminal', 'filter', 'loaded_alt', 'loaded_mixed', 'loaded_duplicate', 'loaded_product', 'units_tiny_money', 'margin_zero'}
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

for variant in ('normal', 'asan'):
    for mode in modes:
        case = root / (variant + '-' + mode)
        case.mkdir()
        path = case / 'fees.json'
        path.write_text('old-sentinel')
        path.chmod(0o600)
        if mode.startswith('loaded_'):
            (case / 'commodities.json').write_text('{}')
            (case / 'contracts.json').write_text(json.dumps({'CZCE': {'SR2510': {
                'name': 'sugar', 'product': 'SR', 'altcode': 'SR510',
                'maxmarketqty': 100, 'maxlimitqty': 100, 'minmarketqty': 1, 'minlimitqty': 1,
                'opendate': 20170101, 'expiredate': 20251231,
                'longmarginratio': 0.15, 'shortmarginratio': 0.15}}}))
        run = subprocess.run([str(root / (variant + '-probe')), mode, str(case)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, env=env, timeout=20)
        (root / (variant + '-' + mode + '.log')).write_text(run.stdout + run.stderr)
        if mode in success:
            assert run.returncode == 0, (variant, mode, run.returncode, run.stderr)
            data = json.loads(path.read_text())
            if mode.startswith('loaded_'):
                assert set(data) == {'CZCE.SR2510'} and data['CZCE.SR2510']['open'] == 0.25
                assert data['CZCE.SR2510']['margin'] == 0.15
            else:
                assert set(data) == ({'SHFE.rb2510'} if mode == 'filter' else {'SHFE.au2512', 'SHFE.rb2510'}), (variant, mode, data)
                rb = data['SHFE.rb2510']
                if mode == 'units_tiny_money':
                    assert rb['open'] == 5e-7 and rb['byvolume'] is False
                else:
                    assert rb['open'] == 0.0 and rb['byvolume'] is True
                    assert rb['close'] == 0.0 and rb['closetoday'] == 0.0
                if mode == 'margin_zero':
                    assert rb['margin'] == 0.0
            if mode != 'filter' and not mode.startswith('loaded_'):
                assert data['SHFE.au2512']['open'] == -0.25
                assert data['SHFE.au2512']['margin'] == 0.2
            assert (path.stat().st_mode & 0o777) == 0o600, 'existing file mode changed'
        else:
            assert run.returncode == 1, (variant, mode, run.returncode, run.stderr)
            assert path.read_text() == 'old-sentinel', (variant, mode, 'old file changed')
            assert 'fees dumped into' not in run.stderr, (variant, mode, 'false success log')
        assert not list(case.glob('*.wtfees.*.tmp')), (variant, mode, 'temp remains')
        print(variant, mode, 'PASS', flush=True)
    for mode in ('valid', 'write'):
        case = root / (variant + '-new-' + mode)
        case.mkdir()
        run = subprocess.run([str(root / (variant + '-probe')), mode, str(case)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, env=env, timeout=20)
        path = case / 'fees.json'
        if mode == 'valid':
            assert run.returncode == 0 and len(json.loads(path.read_text())) == 2
        else:
            assert run.returncode == 1 and not path.exists()
        assert not list(case.glob('*.wtfees.*.tmp'))
        print(variant, 'new-' + mode, 'PASS', flush=True)
    case = root / (variant + '-destination-directory')
    case.mkdir(); (case / 'fees.json').mkdir(); (case / 'fees.json' / 'sentinel').write_text('unchanged')
    run = subprocess.run([str(root / (variant + '-probe')), 'valid', str(case)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=env, timeout=20)
    assert run.returncode == 1 and (case / 'fees.json' / 'sentinel').read_text() == 'unchanged'
    assert not list(case.glob('*.wtfees.*.tmp'))
    print(variant, 'destination-directory', 'PASS', flush=True)
    case = root / (variant + '-readonly')
    case.mkdir(); path = case / 'fees.json'; path.write_text('old-sentinel'); path.chmod(0o400)
    assert not os.access(path, os.W_OK), 'permission control is ineffective for this user'
    run = subprocess.run([str(root / (variant + '-probe')), 'valid', str(case)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=env, timeout=20)
    assert run.returncode == 1 and path.read_text() == 'old-sentinel'
    assert (path.stat().st_mode & 0o777) == 0o400
    assert not list(case.glob('*.wtfees.*.tmp'))
    print(variant, 'readonly', 'PASS', flush=True)

consumer = subprocess.run([str(root / 'consumer-key-probe')], capture_output=True, text=True, timeout=20)
assert consumer.returncode == 0
consumer_key = consumer.stdout.strip()
exported = json.loads((root / 'normal-valid' / 'fees.json').read_text())
assert consumer_key == 'SHFE.rb' and consumer_key not in exported and 'SHFE.rb2510' in exported
(root / 'consumer-key-result.txt').write_text('consumer product key=' + consumer_key + '\n' +
    'exported contract keys=' + ','.join(sorted(exported)) + '\n')
print('consumer-key mismatch observed', consumer_key, 'vs', ','.join(sorted(exported)), flush=True)

for mode in ('units_mixed_open_volume', 'units_mixed_open_money', 'units_mixed_same',
             'units_mixed_tiny', 'nan_field_3', 'nan_field_4', 'nan_field_5', 'units_tiny_money',
             'margin_long_volume', 'margin_short_volume', 'margin_different_short', 'margin_nan_1', 'margin_nan_2', 'margin_nan_3',
             'sentinel_fee_d_1', 'sentinel_fee_f_4', 'sentinel_margin_d_1', 'sentinel_margin_f_0'):
    case = root / ('unit-baseline-' + mode)
    case.mkdir(); (case / 'fees.json').write_text('old-sentinel')
    run = subprocess.run([str(root / 'unit-baseline-probe'), mode, str(case)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=env, timeout=20)
    (root / ('unit-baseline-' + mode + '.log')).write_text(run.stdout + run.stderr)
    path = case / 'fees.json'
    if mode == 'units_tiny_money':
        assert run.returncode == 0 and json.loads(path.read_text())['SHFE.rb2510']['open'] != 5e-7
    else:
        assert run.returncode == 0 and path.read_text() != 'old-sentinel'
    print('unit-baseline', mode, 'old-loss-observed', run.returncode, flush=True)

# A controlled red build must not be interpreted as product success. It preserves
# the previous bytes and compiles the prior implementation separately.
for mode in ('missing_fee', 'missing_contract', 'response_error', 'empty_terminal', 'earlier_error'):
    case = root / ('baseline-' + mode)
    case.mkdir(); (case / 'fees.json').write_text('old-sentinel')
    run = subprocess.run([str(root / 'baseline-probe'), mode, str(case)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, env=env, timeout=20)
    (root / ('baseline-' + mode + '.log')).write_text(run.stdout + run.stderr)
    # Failures or false success both disprove the new contract in the old code.
    path = case / 'fees.json'
    unchanged = path.exists() and path.read_text(errors='replace') == 'old-sentinel'
    if mode == 'empty_terminal':
        observed = run.returncode != 0
    else:
        observed = run.returncode != 1 or not unchanged
    assert observed, ('baseline unexpectedly met new contract', mode, run.returncode)
    print('baseline', mode, 'old-failure-observed', run.returncode, flush=True)
