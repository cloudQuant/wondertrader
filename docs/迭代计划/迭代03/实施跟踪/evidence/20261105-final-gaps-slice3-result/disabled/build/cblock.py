import json,re,sys
S='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
REPO='/Users/yunjinqi/Documents/new_projects/wondertrader/'
inv=json.load(open(S+'disabled-inventory.json'))
def locate(i):
    it=inv[i-1]
    loc=[l for l in it['locs'] if '/coverage/' in l['file']][0]
    raw=open(REPO+loc['file'],'rb').read()
    crlf=b'\r\n' in raw
    L=raw.decode('utf-8').split('\n')
    suite,name=it['test'].split('.',1)
    pat=re.compile(r'TEST(_F)?\(\s*'+re.escape(suite)+r'\s*,\s*'+re.escape(name)+r'\s*\)')
    t=[k for k,l in enumerate(L) if pat.search(l)][0]
    # comment block above
    k=t-1; 
    while k>=0 and L[k].strip()=='' : k-=1
    end=k
    inblock=False
    while k>=0:
        s=L[k].strip().rstrip('\r')
        if s.startswith('//') or s.startswith('*') or s.startswith('/*') or s.endswith('*/'):
            k-=1; continue
        break
    start=k+1
    return loc['file'],crlf,L,t,start,end
if __name__=='__main__':
    for i in [int(x) for x in sys.argv[1].split(',')]:
        f,crlf,L,t,s,e=locate(i)
        print(f'==== [{i}] {f} crlf={crlf} test@{t+1} comment {s+1}-{e+1}')
        for j in range(s,e+1): print(f'  {j+1}: {L[j].rstrip()[:200]}')
        print(f'  {t+1}: {L[t].rstrip()}')
