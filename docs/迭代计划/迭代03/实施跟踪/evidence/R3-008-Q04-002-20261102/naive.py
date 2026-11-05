import difflib, hashlib, json, pathlib, subprocess
root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
out=pathlib.Path('/tmp/wt-i03-order-ledger-20261102')
evidence=root/'docs/迭代计划/迭代03/实施跟踪/evidence/R3-008-Q04-002-20261102'
build=pathlib.Path('/tmp/wt-i03-r78-20261102/build')
source=(root/'src/WtUftCore/TraderAdapter.cpp').read_text()
needle='\tint32_t ret = _trader_api->orderInsert(entrust);\n\tif(ret < 0)\n\t{\n'
assert source.count(needle)==1
changed=source.replace(needle,needle+'\t\trollbackUndoneIfNonzero(stdCode.c_str(), entrust->getVolume());\n')
candidate=out/'TraderAdapter-naive.cpp';candidate.write_text(changed)
(evidence/'naive-control.diff').write_text(''.join(difflib.unified_diff(source.splitlines(True),changed.splitlines(True),fromfile='original',tofile='naive-control')))
commands=json.loads((evidence/'commands.json').read_text())
compile=commands[1][:];compile[-1]=str(candidate);compile[1:1]=['-I'+str(root/'src/WtUftCore')]
for flag in ('-MT','-MF','-o'):
    compile[compile.index(flag)+1]=str(out/'prod-naive.o')+('.d' if flag=='-MF' else '')
link=commands[2][:];link=[str(out/'prod-naive.o') if x==str(out/'prod.o') else x for x in link];link[link.index('-o')+1]=str(out/'naive')
(evidence/'naive-commands.json').write_text(json.dumps([compile,link],indent=2))
for i,cmd in enumerate((compile,link)):
    r=subprocess.run(cmd,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (evidence/f'naive-build-{i}.log').write_text(r.stdout+f'\nEXIT={r.returncode}\n')
    print('naive build',i,r.returncode,flush=True)
    if r.returncode: raise SystemExit(r.returncode)
(evidence/'naive.sha256').write_text(hashlib.sha256((out/'naive').read_bytes()).hexdigest()+'  naive\n')
