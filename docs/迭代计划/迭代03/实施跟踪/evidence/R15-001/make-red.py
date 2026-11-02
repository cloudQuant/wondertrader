from pathlib import Path
h=Path('/private/tmp/wt-i03-r15-tags/red-source/WtCore/HftStraBaseCtx.h')
s=h.read_text()
a='std::string _usertag; ///< 用户自定义标签'
b='char _usertag[32]; ///< 用户自定义标签'
assert s.count(a)==1
s=s.replace(a,b)
a='_DetailInfo()\n\t\t\t: _long(false), _price(0), _volume(0), _opentime(0), _opentdate(0),\n\t\t\t  _max_profit(0), _max_loss(0), _profit(0) {}'
b='_DetailInfo() { memset(this, 0, sizeof(_DetailInfo)); }'
assert s.count(a)==1
h.write_text(s.replace(a,b))
p=Path('/private/tmp/wt-i03-r15-tags/red-source/WtCore/HftStraBaseCtx.cpp')
s=p.read_bytes()
a=b'dInfo._usertag = userTag ? userTag : "";';b=b'wt_strcpy(dInfo._usertag, userTag);'
assert s.count(a)==2
s=s.replace(a,b)
a=b'pInfo._closeprofit, dInfo._usertag.c_str(), userTag);';b=b'pInfo._closeprofit, dInfo._usertag, userTag);'
assert s.count(a)==1
p.write_bytes(s.replace(a,b))
