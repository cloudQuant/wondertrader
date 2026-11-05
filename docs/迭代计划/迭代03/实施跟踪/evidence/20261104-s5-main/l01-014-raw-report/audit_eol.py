import re, subprocess, difflib, sys
def split(b):
    out=[]
    for l in re.findall(rb'[^\n]*\n|[^\n]+$', b):
        if l.endswith(b'\r\n'): out.append((l[:-2],b'\r\n'))
        elif l.endswith(b'\n'): out.append((l[:-1],b'\n'))
        else: out.append((l,b''))
    return out
files=subprocess.run(['git','diff','--name-only','-z','HEAD'],capture_output=True).stdout.split(b'\0')
bad=[]
for f in files:
    if not f: continue
    p=f.decode()
    try: cur=open(p,'rb').read()
    except FileNotFoundError: continue
    head=subprocess.run(['git','show','HEAD:'+p],capture_output=True).stdout
    B=split(head); A=split(cur)
    if not B: continue
    bc=[c for c,e in B]; ac=[c for c,e in A]
    sm=difflib.SequenceMatcher(None,bc,ac,autojunk=False)
    changed=0; eq=0
    for tag,i1,i2,j1,j2 in sm.get_opcodes():
        if tag=='equal':
            for k in range(i2-i1):
                eq+=1
                if B[i1+k][1]!=A[j1+k][1] and A[j1+k][1]!=b'' and B[i1+k][1]!=b'': changed+=1
    if changed: bad.append((p,changed,eq))
for x in bad: print(*x)
print('checked',len([f for f in files if f]),'flagged',len(bad))
