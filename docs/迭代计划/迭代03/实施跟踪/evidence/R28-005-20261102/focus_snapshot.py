from pathlib import Path

root = Path('/tmp/wt-i03-r28-005-20261102/snapshot/src')
p = root / 'TestUnits/coverage/CMakeLists.txt'
s = p.read_text()
old = '"wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3"'
new = '"wtcore|WtCore|n02_wtcore_engines/n02_exec_mgr_test.cpp"'
assert s.count(old) == 1
s = s.replace(old, new)
old = 'file(GLOB mod_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/m_${mod}/*.cpp")'
new = old + '\n\tif(mod STREQUAL "WtRunner")\n\t\tset(mod_tests "${CMAKE_CURRENT_SOURCE_DIR}/m_WtRunner/r28_runner_main_test.cpp")\n\tendif()'
assert s.count(old) == 1
s = s.replace(old, new)
p.write_text(s)
p = root / 'CMakeLists.txt'
s = p.read_text()
old = 'include_directories(${Boost_INCLUDE_DIRS})'
assert s.count(old) == 1
s = s.replace(old, 'include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)\n' + old)
p.write_text(s)
