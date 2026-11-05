from pathlib import Path
root=Path('/tmp/wt-i03-r42-001-20261102/snapshot/src')
p=root/'TestUnits/coverage/CMakeLists.txt'
b=p.read_bytes()
old=b'wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3'
new=b'wtcore|WtCore|p02_wtcore_engines2/r42_sel_ctx_test.cpp'
assert b.count(old)==1
p.write_bytes(b.replace(old,new))
p=root/'CMakeLists.txt'
b=p.read_bytes()
needle=b'include_directories(${Boost_INCLUDE_DIRS})'
assert b.count(needle)==1
p.write_bytes(b.replace(needle,needle+b'\r\ninclude_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)'))
