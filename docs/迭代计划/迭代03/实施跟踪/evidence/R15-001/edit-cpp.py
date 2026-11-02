from pathlib import Path
p=Path('src/WtCore/HftStraBaseCtx.cpp')
b=p.read_bytes()
a=b'wt_strcpy(dInfo._usertag, userTag);'
c=b'dInfo._usertag = userTag ? userTag : "";'
assert b.count(a)==2
b=b.replace(a,c)
a=b'pInfo._closeprofit, dInfo._usertag, userTag);'
c=b'pInfo._closeprofit, dInfo._usertag.c_str(), userTag);'
assert b.count(a)==1
p.write_bytes(b.replace(a,c))
