# I03-NEW-025 iterative fixer: reads scan logs, fixes each reported instantiation site
# by type rule; only lines the compiler reported are touched. Prints unhandled sites.
import re, sys, glob, os, shutil, json
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/TestUnits/coverage/'
logs_dir, before_dir, record = sys.argv[1], sys.argv[2], sys.argv[3]
allowed=set(open(sys.argv[4]).read().split())   # files we hold locks for
sites=[]
for f in sorted(glob.glob(logs_dir+'/*.log')):
    t=open(f,errors='replace').read()
    for m in re.finditer(r"(/[^\s:]+):(\d+):\d+: note: in instantiation of function template specialization '([^']+)'", t):
        if m.group(1).startswith(ROOT):
            sites.append((m.group(1)[len(ROOT):], int(m.group(2)), m.group(3)))
def fix(line, types):
    a = types.split('<',1)[1]
    first, second = [x.strip() for x in a.split(',')[:2]]
    second = second.rstrip('>').strip()
    # literal side unsigned, value side signed -> drop the u suffix on the literal
    if first.startswith('unsigned') and second == 'int':
        m = re.subn(r'\((\d+)u, ', r'(\1, ', line, count=1)
        if m[1]: return m[0]
        m = re.subn(r', (\d+)u\)', r', \1)', line, count=1)
        if m[1]: return m[0]
    if first == 'int' and (second.startswith('unsigned') or second.startswith('wtp::tag')):
        m = re.subn(r'(->size\(\)|\.size\(\)), (\d+)u\)', r'\1, \2)', line, count=1)
        if m[1]: return m[0]
        # signed literal vs unsigned value (incl. enums with uint32_t underlying type)
        m = re.subn(r'\((\d+), ', r'(\1u, ', line, count=1)
        if m[1]: return m[0]
    if first.startswith('wtp::tag') and second == 'int':
        m = re.subn(r'\((W[A-Z]{2,3}_[A-Za-z_]+), ', r'(static_cast<int>(\1), ', line, count=1)
        if m[1]: return m[0]
    if first == 'unsigned long' and second == 'int' and 'MAX_INSTRUMENT_LENGTH' in line and 'static_cast' not in line:
        return re.sub(r'(MAX_INSTRUMENT_LENGTH(?: - 1)?)\)', r'static_cast<std::size_t>(\1))', line, count=1)
    if {first, second} == {'unsigned long long', 'long long'} and 'TimeUtils::makeTime(' in line and 'static_cast' not in line:
        return re.sub(r'(TimeUtils::makeTime\([^()]*\))', r'static_cast<uint64_t>(\1)', line, count=1)
    if first == 'unsigned long long' and second == 'long long' and ', timeBeforeQuote)' in line:
        return line.replace(', timeBeforeQuote)', ', static_cast<uint64_t>(timeBeforeQuote))', 1)
    return None
done=[]; unhandled=[]
byfile={}
for src,n,types in sites: byfile.setdefault(src,[]).append((n,types))
for src,items in byfile.items():
    if src not in allowed:
        unhandled += [(src,n,types,'not locked') for n,types in items]; continue
    p=ROOT+src; raw=open(p,'rb').read(); lines=raw.split(b'\n')
    bk=os.path.join(before_dir,('src/TestUnits/coverage/'+src).replace('/','__'))
    if not os.path.exists(bk): shutil.copy2(p,bk)
    changed=False
    for n,types in items:
        L=lines[n-1].decode('utf-8'); cr=L.endswith('\r'); body=L[:-1] if cr else L
        nb=fix(body,types)
        if nb is not None and (len(nb) < len(body) - 1):
            nb=None   # a fix only removes a 'u' or adds text
        if nb is None or nb==body:
            unhandled.append((src,n,types,body.strip())); continue
        lines[n-1]=(nb+('\r' if cr else '')).encode('utf-8'); changed=True
        done.append({'site':f'{src}:{n}','types':types,'old':body.strip(),'new':nb.strip()})
    if changed:
        out=b'\n'.join(lines); tmp=p+'.s1tmp'; open(tmp,'wb').write(out); shutil.copymode(p,tmp); os.replace(tmp,p)
with open(record,'a') as f:
    for d in done: f.write(json.dumps(d,ensure_ascii=False)+'\n')
print('fixed',len(done),'unhandled',len(unhandled))
for u in unhandled: print('UNHANDLED',u)
