"""I03-NEW-008 / card 13d fee consumer pipeline fixture.

usage: python3 -I run.py <new-output-dir> <coverage-build-dir>

<coverage-build-dir> is a configured and built Ninja coverage tree of this source root
(it must contain the CoverageTests_btcore objects). The fixture TU is compiled with the
recorded compile command of l06_replayer_runtime_test.cpp (dependency-file options are
dropped so nothing is written into the build tree) and linked into a private copy of the
CoverageTests_btcore link line. Each case runs in its own process; every case must pass.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

if len(sys.argv) != 3:
    raise SystemExit('usage: run.py <new-output-dir> <coverage-build-dir>')
root = Path(__file__).resolve().parents[4]
export = root / 'src/TestUnits/fixtures/i03_fee_consumer_pipeline'
output = Path(sys.argv[1]).resolve()
build = Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=False)
ninja = shutil.which('ninja') or '/opt/homebrew/bin/ninja'
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()


def compile_database():
    db = build / 'compile_commands.json'
    if db.exists():
        return json.loads(db.read_text())
    return json.loads(subprocess.check_output([ninja, '-C', str(build), '-t', 'compdb'], text=True))


def strip_depfile(args):
    out, i = [], 0
    while i < len(args):
        if args[i] in ('-MT', '-MF'):
            i += 2
            continue
        if args[i] in ('-MD', '-MMD'):
            i += 1
            continue
        out.append(args[i])
        i += 1
    return out


commands = compile_database()
entry = next(c for c in commands if c['file'].endswith('l06_wtbtcore/l06_replayer_runtime_test.cpp'))
# The build must have been compiled from the same replayer/fee sources as this tree.
build_src = Path(entry['file']).resolve().parents[3]
for suffix in ['WtBtCore/HisDataReplayer.cpp', 'WtBtCore/HisDataReplayer.h', 'Share/CodeHelper.hpp', 'Share/FeeSchedule.hpp']:
    assert sha(root / 'src' / suffix) == sha(build_src / suffix), suffix
    print(suffix, sha(root / 'src' / suffix), flush=True)

for src, target in [(export / 'exported-fees.json', 'fees-exported.json'),
                    (export / 'exported-commodities.json', 'commodities.json'),
                    (export / 'exported-contracts.json', 'contracts.json'),
                    (root / 'dist/common/sessions.json', 'sessions.json')]:
    shutil.copyfile(src, output / target)
# The fake CTP export has empty session names, which the base-data loader skips.
# Give its two commodities a known session so the controls isolate fee-key behavior
# rather than missing commodity metadata.
commodity_path = output / 'commodities.json'
commodities = json.loads(commodity_path.read_text())
for item in commodities['SHFE'].values():
    item['session'] = 'SD0930'
commodity_path.write_text(json.dumps(commodities))
# Version-2 envelope carrying the exported contract rates unchanged.
exported = json.loads((export / 'exported-fees.json').read_text())
contracts = {key: {k: value[k] for k in ('open', 'close', 'closetoday', 'byvolume')} for key, value in exported.items()}
(output / 'fees-v2-contract-volume.json').write_text(json.dumps({'version': 2, 'contracts': contracts, 'products': {}}))
amount = {'open': 0.01, 'close': 0.01, 'closetoday': 0.01, 'byvolume': False}
(output / 'fees-amount-contract.json').write_text(json.dumps({'version': 2, 'contracts': {'SHFE.au2512': amount}, 'products': {}}))
(output / 'fees-amount-product.json').write_text(json.dumps({'SHFE.au': amount}))

compile_cmd = strip_depfile(shlex.split(entry['command']))
compile_cmd[compile_cmd.index('-o') + 1] = str(output / 'consumer.o')
compile_cmd[compile_cmd.index('-c') + 1] = str(export / 'consumer_test.cpp')
subprocess.run(compile_cmd, cwd=build, check=True)
line = subprocess.check_output([ninja, '-C', str(build), '-t', 'commands', 'CoverageTests_btcore'], text=True).splitlines()[-1]
assert line.startswith(': && ') and line.endswith(' && :')
link = shlex.split(line[5:-5])
link[link.index('-o') + 1] = str(output / 'consumer-test')
idx = next(i for i, part in enumerate(link) if part.endswith('l06_replayer_runtime_test.cpp.o'))
link.insert(idx + 1, str(output / 'consumer.o'))
subprocess.run(link, cwd=build, check=True)
env = dict(os.environ, WT_FEE_FIXTURE=str(output), LLVM_PROFILE_FILE=str(output / '%p.profraw'))
names = ['UnmigratedExportedContractKeysAreAnObservableMissingFee',
         'VersionedContractVolumeFeeIsUsedForThePhysicalContract',
         'ContractAmountFeeRequiresPhysicalContractMetadata',
         'LegacyProductAmountFeeStillWorks']
outcomes = []
for name in names:
    result = subprocess.run([str(output / 'consumer-test'), '--gtest_filter=FeeConsumerPipeline.' + name],
                            cwd=output, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
    (output / (name + '.log')).write_text(result.stdout)
    outcomes.append((name, result.returncode))
print(outcomes, flush=True)
assert [status for _, status in outcomes] == [0] * len(names), 'unexpected result; inspect logs'
