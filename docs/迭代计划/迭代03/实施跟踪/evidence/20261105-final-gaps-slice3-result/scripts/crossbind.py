import re,sys,subprocess,collections
# usage: crossbind.py bind.log test_image_name
log=open(sys.argv[1],errors='replace').read().splitlines()
img=sys.argv[2]
pat=re.compile(r'<([^/>]+)/bind#\d+> -> 0x[0-9a-f]+ <([^/>]+)/(__Z[^>]*)>')
cnt=collections.Counter(); ta=collections.Counter(); samples={}
for l in log:
    m=pat.search(l)
    if not m: continue
    src,dst,sym=m.groups()
    if src==dst or '3wtp' not in sym: continue
    if img not in (src,dst): continue
    cnt[(src,dst)]+=1
    if '3wtp13TraderAdapter' in sym or '3wtp13ParserAdapter' in sym or '3wtp15ActionPolicyMgr' in sym:
        ta[(src,dst)]+=1; samples.setdefault((src,dst),sym)
print('cross-image wtp binds involving',img)
for k,v in sorted(cnt.items()): print(' ',k[0],'->',k[1],v)
print('of which TraderAdapter/ParserAdapter/ActionPolicyMgr:')
for k,v in sorted(ta.items()):
    d=subprocess.run(['c++filt',samples[k]],capture_output=True,text=True).stdout.strip()
    print(' ',k[0],'->',k[1],v,'e.g.',d[:120])
