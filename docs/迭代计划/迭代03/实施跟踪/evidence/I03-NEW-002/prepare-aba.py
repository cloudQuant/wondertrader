from pathlib import Path
p=Path('/private/tmp/wt-i03-new002/aba-source/WtCore/HftStraBaseCtx.h'); s=p.read_text()
a='\t\tif (_trader == expected && _trader_binding_epoch == epoch)'
b='\t\t(void)epoch;\n\t\tif (_trader == expected)'
assert s.count(a)==1
p.write_text(s.replace(a,b))
s=Path('/private/tmp/wt-i03-new002/build-normal.sh').read_text()
s=s.replace('src/WtCore/','/private/tmp/wt-i03-new002/aba-source/WtCore/').replace('src/TestUnits/','/private/tmp/wt-i03-new002/aba-source/TestUnits/').replace('-Isrc ','-I/private/tmp/wt-i03-new002/aba-source ').replace('-include src/Share/','-include /private/tmp/wt-i03-new002/aba-source/Share/').replace('/final-','/aba-')
Path('/private/tmp/wt-i03-new002/build-aba.sh').write_text(s)
