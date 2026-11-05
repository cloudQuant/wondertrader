#!/usr/bin/env python3
"""Compile extracted production request methods with local fake dependencies."""
import pathlib
import subprocess
import sys
root = pathlib.Path(__file__).resolve().parents[4]
out = pathlib.Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
src = (root / 'src/WtCore/TraderAdapter.cpp').read_text()
sigs = ['uint32_t TraderAdapter::doEntrust(WTSEntrust* entrust)',
        'bool TraderAdapter::checkCancelLimits(const char* stdCode)',
        'bool TraderAdapter::checkOrderLimits(const char* stdCode)',
        'bool TraderAdapter::isTradeEnabled(const char* stdCode) const',
        'bool TraderAdapter::doCancel(WTSOrderInfo* ordInfo)',
        'bool TraderAdapter::cancel(uint32_t localid)',
        'OrderIDs TraderAdapter::cancel(const char* stdCode, bool isBuy, double qty /* = 0 */)']
parts = []
for sig in sigs:
    start = src.index(sig)
    brace = src.index('{', start)
    depth = 0
    for pos in range(brace, len(src)):
        if src[pos] == '{': depth += 1
        elif src[pos] == '}':
            depth -= 1
            if not depth:
                parts.append(src[start:pos+1])
                break
    else: raise RuntimeError(sig)
(out / 'extracted.inc').write_text('\n'.join(parts))
subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-pthread', '-I', str(root / 'src'), '-I', str(out), str(pathlib.Path(__file__).with_name('check.cpp')), '-o', str(out / 'check')], check=True)
subprocess.run([str(out / 'check')], check=True, timeout=20)
