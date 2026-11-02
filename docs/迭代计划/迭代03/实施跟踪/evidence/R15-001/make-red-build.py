from pathlib import Path
p=Path('/private/tmp/wt-i03-r15-tags/build-asan.sh')
s=p.read_text()
s=s.replace('src/WtCore/', '/private/tmp/wt-i03-r15-tags/red-source/WtCore/')
s=s.replace('src/TestUnits/', '/private/tmp/wt-i03-r15-tags/red-source/TestUnits/')
s=s.replace('-Isrc ', '-I/private/tmp/wt-i03-r15-tags/red-source ')
s=s.replace('-include src/Share/', '-include /private/tmp/wt-i03-r15-tags/red-source/Share/')
s=s.replace('/asan-', '/red-asan-')
Path('/private/tmp/wt-i03-r15-tags/build-red-asan.sh').write_text(s)
