from pathlib import Path
p=Path('/private/tmp/wt-i03-r15-tags/red-index-source/WtCore/HftStraBaseCtx.h')
s=p.read_text()
a='boost::circular_buffer<OrderTag> _orders;'
b='boost::circular_buffer<OrderTag> _orders{16};'
assert s.count(a)==1
s=s.replace(a,b)
a='bool\t\t\t_ud_modified;'
b='bool\t\t\t_ud_modified = false;'
assert s.count(a)==1
p.write_text(s.replace(a,b))
build=Path('/private/tmp/wt-i03-r15-tags/build-normal.sh').read_text()
build=build.replace('src/WtCore/','/private/tmp/wt-i03-r15-tags/red-index-source/WtCore/')
build=build.replace('src/TestUnits/','/private/tmp/wt-i03-r15-tags/red-index-source/TestUnits/')
build=build.replace('-Isrc ','-I/private/tmp/wt-i03-r15-tags/red-index-source ')
build=build.replace('-include src/Share/','-include /private/tmp/wt-i03-r15-tags/red-index-source/Share/')
build=build.replace('/final-','/red-index-')
Path('/private/tmp/wt-i03-r15-tags/build-red-index.sh').write_text(build)
