# prepare.py <snapshot-root> <green|red>
# Narrows the coverage CMake to this fixture's tests inside a physical snapshot.
# red: restores the pre-change product sources stored in baseline/.
import shutil, sys
from pathlib import Path
root = Path(sys.argv[1]); mode = sys.argv[2]
fx = root / 'src/TestUnits/fixtures/i03_exec_eager_validation_20261103'
if mode == 'red':
    for name, dest in [('WtLocalExecuter.cpp','src/WtCore'),('WtDiffExecuter.cpp','src/WtCore'),
                       ('WtArbiExecuter.cpp','src/WtCore'),('WtSelEngine.cpp','src/WtCore'),
                       ('WtRunner.cpp','src/WtRunner'),('WtRunner.h','src/WtRunner')]:
        shutil.copyfile(fx/'baseline'/name, root/dest/name)
elif mode != 'green':
    raise SystemExit('mode must be green or red')
lane = root / 'src/TestUnits/coverage/p02_wtcore_engines2/i03_exec_eager_validation_test.cpp'
shutil.copyfile(fx/'exec_eager_test.cpp', lane)
p = root / 'src/TestUnits/coverage/CMakeLists.txt'
s = p.read_text()
old = '"wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3"'
new = '"wtcore|WtCore|p02_wtcore_engines2/i03_exec_eager_validation_test.cpp"'
if s.count(old) != 1: raise SystemExit('wtcore target changed')
s = s.replace(old, new, 1)
old = 'file(GLOB mod_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/m_${mod}/*.cpp")'
new = old + '\n\tif(mod STREQUAL "WtRunner")\n\t\tset(mod_tests "${CMAKE_CURRENT_SOURCE_DIR}/m_WtRunner/r104_runner_executers_test.cpp")\n\tendif()'
if s.count(old) != 1: raise SystemExit('runner test selection changed')
s = s.replace(old, new, 1)
s += '''\nadd_library(I03SelFact SHARED "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/i03_exec_eager_validation_20261103/sel_factory.cpp")
target_include_directories(I03SelFact PRIVATE "${CMAKE_SOURCE_DIR}" /opt/homebrew/Cellar/boost/1.88.0/include /opt/homebrew/include)
set_target_properties(I03SelFact PROPERTIES OUTPUT_NAME "I03SelFact" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/i03_sel_plugins")
add_dependencies(CoverageTests_m_WtRunner I03SelFact)
'''
p.write_text(s)
p = root / 'src/CMakeLists.txt'; s = p.read_text(); old = 'include_directories(${Boost_INCLUDE_DIRS})'
if s.count(old) != 1: raise SystemExit('Boost configuration changed')
p.write_text(s.replace(old, 'include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)\n' + old, 1))
p = root / 'src/TestUnits/coverage/m_WtRunner/r104_runner_executers_test.cpp'
with p.open('a') as f: f.write('\n#include "../../fixtures/i03_exec_eager_validation_20261103/runner.inc"\n')
print('prepared', mode)
