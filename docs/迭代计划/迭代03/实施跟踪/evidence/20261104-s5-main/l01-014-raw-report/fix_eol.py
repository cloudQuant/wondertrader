import re, sys, difflib
def split(b):
    lines=re.findall(rb'[^\n]*\n|[^\n]+$', b)
    out=[]
    for l in lines:
        if l.endswith(b'\r\n'): out.append((l[:-2],b'\r\n'))
        elif l.endswith(b'\n'): out.append((l[:-1],b'\n'))
        else: out.append((l,b''))
    return out
def fix(before_path, after_path, check_only=False):
    B=split(open(before_path,'rb').read()); A=split(open(after_path,'rb').read())
    bc=[c.rstrip(b'\r') if e==b'' else c for c,e in B]
    ac=[c.rstrip(b'\r') for c,e in A]
    ncr=sum(1 for c,e in B if e==b'\r\n'); nlf=sum(1 for c,e in B if e==b'\n')
    major=b'\r\n' if ncr>nlf else b'\n'
    sm=difflib.SequenceMatcher(None,bc,ac,autojunk=False)
    res=[]
    for tag,i1,i2,j1,j2 in sm.get_opcodes():
        if tag=='equal':
            for k in range(i2-i1):
                res.append(ac[j1+k]+B[i1+k][1])
        else:
            for k in range(j1,j2):
                res.append(ac[k]+(A[k][1] if A[k][1]==b'' else major))
    data=b''.join(res)
    if not check_only:
        open(after_path,'wb').write(data)
    return ncr, nlf, data.count(b'\r\n'), data.count(b'\n')-data.count(b'\r\n')
for bp,ap in zip(sys.argv[1::2],sys.argv[2::2]):
    print(ap, fix(bp,ap))
