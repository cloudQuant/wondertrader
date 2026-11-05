from pathlib import Path
import sys
root = Path(sys.argv[1])
cmake = root / 'src/TestUnits/coverage/CMakeLists.txt'
s = cmake.read_text()
old = 'wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3'
new = 'wtcore|WtCore|q01_wtcore3/r7_vwap_v2_local_integration_test.cpp+n01_wtcore_tradepaths/r220_arbi_serialization_test.cpp+p02_wtcore_engines2/p02_diff_executer_test.cpp+n02_wtcore_engines/n02_diff_concurrency_test.cpp'
if old not in s: raise SystemExit('wtcore test target changed; update isolated preparation')
cmake.write_text(s.replace(old,new,1))
cmake = root / 'src/CMakeLists.txt'
s = cmake.read_text()
needle = 'include_directories(${Boost_INCLUDE_DIRS})'
if needle not in s: raise SystemExit('Boost include configuration changed')
cmake.write_text(s.replace(needle, needle+'\ninclude_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)', 1))
for test,fixture in [('q01_wtcore3/r7_vwap_v2_local_integration_test.cpp','local.inc'),('n01_wtcore_tradepaths/r220_arbi_serialization_test.cpp','arbi.inc'),('p02_wtcore_engines2/p02_diff_executer_test.cpp','diff.inc')]:
 p=root/'src/TestUnits/coverage'/test
 with p.open('a') as f: f.write('\n#include "../../fixtures/i03_fractional_targets_20261103/'+fixture+'"\n')
p = root / 'src/TestUnits/coverage/p02_wtcore_engines2/p02_diff_executer_test.cpp'
with p.open('a') as f: f.write('\n#include "../../fixtures/i03_diff_batch_atomic_20261103/atomic.inc"\n')
for test, fixtures in [
    ('q01_wtcore3/r7_vwap_v2_local_integration_test.cpp', ('pure.inc', 'local.inc')),
    ('n01_wtcore_tradepaths/r220_arbi_serialization_test.cpp', ('arbi.inc',)),
]:
 p = root / 'src/TestUnits/coverage' / test
 with p.open('a') as f:
  for fixture in fixtures:
   f.write('\n#include "../../fixtures/i03_execution_groups_20261103/' + fixture + '"\n')
