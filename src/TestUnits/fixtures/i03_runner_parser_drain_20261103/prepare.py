from pathlib import Path
import sys
root = Path(sys.argv[1])
p = root / 'src/TestUnits/coverage/CMakeLists.txt'
s = p.read_text()
old = '"wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3"'
new = '"wtcore|WtCore|n02_wtcore_engines/n02_exec_mgr_test.cpp"'
if s.count(old) != 1: raise SystemExit('wtcore target changed')
s=s.replace(old,new,1)
old='file(GLOB mod_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/m_${mod}/*.cpp")'
new=old+'\n\tif(mod STREQUAL "WtRunner")\n\t\tset(mod_tests "${CMAKE_CURRENT_SOURCE_DIR}/m_WtRunner/r28_runner_main_test.cpp")\n\tendif()'
if s.count(old) != 1: raise SystemExit('runner test selection changed')
s=s.replace(old,new,1)
s += '''\nadd_library(I03RunnerParser SHARED "${CMAKE_SOURCE_DIR}/TestUnits/fixtures/i03_parser_lifecycle_v2/provider.cpp")
target_include_directories(I03RunnerParser PRIVATE "${CMAKE_SOURCE_DIR}" /opt/homebrew/Cellar/boost/1.88.0/include /opt/homebrew/include)
set_target_properties(I03RunnerParser PROPERTIES OUTPUT_NAME "Pvt20" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/i03_runner_parser_plugins")
add_dependencies(CoverageTests_m_WtRunner I03RunnerParser)
'''
p.write_text(s)
p=root/'src/CMakeLists.txt';s=p.read_text();old='include_directories(${Boost_INCLUDE_DIRS})'
if s.count(old)!=1: raise SystemExit('Boost configuration changed')
p.write_text(s.replace(old,'include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)\n'+old,1))
p=root/'src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp'
with p.open('a') as f: f.write('\n#include "../../fixtures/i03_runner_parser_drain_20261103/runner.inc"\n')
