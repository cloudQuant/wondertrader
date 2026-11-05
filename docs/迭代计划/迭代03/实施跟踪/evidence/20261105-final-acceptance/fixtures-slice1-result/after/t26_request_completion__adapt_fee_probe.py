from pathlib import Path
import sys
# 2026-11-05: t26_fees_atomic/fee_probe.cpp now carries the request registration this
# script used to splice in (CTraderSpiQueueTestAccess::arm, the same-request data row
# before an empty terminal frame for card 14 / 14c, equal staged filter margins and the
# K02=B completion result) behind WT_T26_PROBE_ARM_REQUESTS, maintained with the current
# CTraderSpi. The adapted probe is that source built for the current code; the old text
# needles no longer exist after that update.
source = Path('src/TestUnits/fixtures/t26_fees_atomic/fee_probe.cpp').read_text()
assert source.count('#ifdef WT_T26_PROBE_ARM_REQUESTS') >= 1
Path(sys.argv[1]).write_text('#define WT_T26_PROBE_ARM_REQUESTS 1\n' + source)
source = Path('src/TestUnits/fixtures/t26_fees_atomic/run_tests.py').read_text()
needle = "\nfor mode in ('units_mixed_open_volume'"
assert source.count(needle) == 1
Path(sys.argv[2]).write_text(source.split(needle)[0] + '\n')
