#!/usr/bin/env python3
import json, os, collections
S='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
E='/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-gaps-slice3-result/disabled/'
inv=json.load(open(S+'disabled-inventory.json'))
res={}
for l in open(S+'pre/run-disabled-asan/results.jsonl'):
    r=json.loads(l); res[(r['binary'],r['test'])]=r
fin={}
for l in open(S+'s3dis/final.txt'):
    i,c,r=l.rstrip('\n').split('|',2); fin[int(i)]=(c,r)
runs=collections.defaultdict(list)
if os.path.exists(E+'runs.jsonl'):
    for l in open(E+'runs.jsonl'):
        r=json.loads(l); runs[r['idx']].append(r)
def ev(i):
    it=inv[i-1]; r=res[(it['binary'],it['test'])]
    parts=[f"诊断 {r['status']}"]
    for x in runs.get(i,[]):
        parts.append(f"{x['phase']}/{x['tree']} {x['ok']}/20 OK rc={x['rc']} `{x['log']}`")
    return '；'.join(parts)
rows=[]
for i,it in enumerate(inv,1):
    c,r=fin[i]
    rows.append(f"| {i} | {it['binary'].replace('CoverageTests','CT')} | `{it['test']}` | {c} | {r.replace('|','/')} | {ev(i)} |")
cnt=collections.Counter(fin[i][0] for i in fin)
print(json.dumps(cnt, ensure_ascii=False))
open(S+'s3dis/table.md','w').write('\n'.join(rows)+'\n')
