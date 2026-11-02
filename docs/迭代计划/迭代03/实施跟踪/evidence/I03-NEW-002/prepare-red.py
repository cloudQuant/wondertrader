from pathlib import Path
p=Path('/private/tmp/wt-i03-new002/build-normal.sh'); s=p.read_text()
s=s.replace('src/WtCore/','/private/tmp/wt-i03-new002/red-source/WtCore/')
s=s.replace('src/TestUnits/','/private/tmp/wt-i03-new002/red-source/TestUnits/')
s=s.replace('-Isrc ','-I/private/tmp/wt-i03-new002/red-source ')
s=s.replace('-include src/Share/','-include /private/tmp/wt-i03-new002/red-source/Share/')
s=s.replace('/final-','/red-')
Path('/private/tmp/wt-i03-new002/build-red.sh').write_text(s)
