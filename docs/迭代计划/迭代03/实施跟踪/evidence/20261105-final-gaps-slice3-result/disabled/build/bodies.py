import json, sys, re
S='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
REPO='/Users/yunjinqi/Documents/new_projects/wondertrader/'
inv=json.load(open(S+'disabled-inventory.json'))
want=[int(x) for x in sys.argv[1].split(',')]
for i in want:
    it=inv[i-1]
    loc=[l for l in it['locs'] if '/coverage/' in l['file']] or it['locs']
    loc=loc[0]
    L=open(REPO+loc['file'],encoding='utf-8',errors='replace').read().split('\n')
    s=loc['line']-1
    suite,name=it['test'].split('.',1)
    pat=re.compile(r'TEST(_F)?\(\s*'+re.escape(suite)+r'\s*,\s*'+re.escape(name)+r'\s*\)')
    hits=[k for k,l in enumerate(L) if pat.search(l)]
    if hits: s=hits[0]
    else: print('!! not found by name', it['test'])
    # find body
    depth=0; started=False; out=[]
    for j in range(s, min(len(L), s+120)):
        line=L[j]; out.append(f"{j+1}: {line}")
        for ch in line:
            if ch=='{': depth+=1; started=True
            elif ch=='}': depth-=1
        if started and depth==0: break
    print(f"==== [{i}] {it['binary']} :: {it['test']}  ({loc['file']})")
    print('\n'.join(x[:170] for x in out))
