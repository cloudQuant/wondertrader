import json,pathlib,subprocess
out=pathlib.Path('/tmp/wt-i03-core-propagation');root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader');orig=(root/'src/WtDtPorter/WtDtRunner.cpp').read_text();needle='_initial_parser_commands_submitted.store(allSubmitted);';assert orig.count(needle)==1
(out/'red-dt-runner.cpp').write_text(orig.replace(needle,'(void)allSubmitted; _initial_parser_commands_submitted.store(true); // Simulate discarded failures.'))
commands=json.loads((out/'dt-commands.json').read_text());records=[]
for name in ['dt-WtDtRunner','dtporter-link']:
 item=next(v for v in commands if v['name']==name);a=item['argv'][:]
 if name=='dt-WtDtRunner':
  a[a.index('-c')+1]=str(out/'red-dt-runner.cpp');a[1:1]=['-I',str(root/'src/WtDtPorter')];a[a.index('-o')+1]=str(out/'red-dt-runner.o')
 else:
  a=[str(out/'red-dt-runner.o') if x==str(out/'dt-WtDtRunner.o') else x for x in a];a[a.index('-o')+1]=str(out/'red-dt-tests')
 p=subprocess.run(a,cwd=item['cwd'],capture_output=True,text=True);(out/('red-'+name+'.log')).write_text(p.stdout+p.stderr);records.append({'name':name,'argv':a,'cwd':item['cwd'],'exit':p.returncode});print(name,p.returncode,flush=True)
 if p.returncode: break
else:
 a=[str(out/'red-dt-tests'),'--gtest_filter=I03DtPorterStart.partial_submission_does_not_release_owners','--gtest_output=xml:'+str(out/'red-dt.xml')];p=subprocess.run(a,cwd=str(root),capture_output=True,text=True,timeout=30);(out/'red-dt-run.log').write_text(p.stdout+p.stderr);records.append({'name':'red-dt-test','argv':a,'cwd':str(root),'exit':p.returncode});print('red-dt-test',p.returncode,flush=True)
(out/'red-dt-commands.json').write_text(json.dumps(records,indent=2))
