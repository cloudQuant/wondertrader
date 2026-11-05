import json,pathlib,subprocess
out=pathlib.Path('/tmp/wt-i03-core-propagation'); root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); orig=(root/'src/WtCore/ParserAdapter.cpp').read_text(); needle='\treturn allSubmitted;\n}'; assert orig.count(needle)==1
(out/'red-parser.cpp').write_text(orig.replace(needle,'\treturn true; // Simulate the historical discarded manager result.\n}'))
commands=json.loads((out/'green-commands.json').read_text()); records=[]
for name in ['parser','core-link','runner-link']:
 item=next(v for v in commands if v['name']==name); a=item['argv'][:]
 if name=='parser':
  a[a.index('-c')+1]=str(out/'red-parser.cpp'); a[1:1]=['-I',str(root/'src/WtCore')]
  a[a.index('-o')+1]=str(out/'red-parser.o')
 else:
  a=[str(out/'red-parser.o') if x==str(out/'parser.o') else x for x in a]; a[a.index('-o')+1]=str(out/('red-'+name.split('-')[0]+'-tests'))
 p=subprocess.run(a,cwd=item['cwd'],capture_output=True,text=True);(out/('red-'+name+'.log')).write_text(p.stdout+p.stderr); records.append({'name':name,'argv':a,'cwd':item['cwd'],'exit':p.returncode}); print(name,p.returncode,flush=True)
 if p.returncode:break
for target,filt in [('core','N01ParserAdapter.manager_reports_partial_submission_and_keeps_owners'),('runner','R28ParserCommands.runner_reports_partial_submission_without_releasing_owners')]:
 a=[str(out/('red-'+target+'-tests')),'--gtest_filter='+filt,'--gtest_output=xml:'+str(out/('red-'+target+'.xml'))]; p=subprocess.run(a,cwd=str(root),capture_output=True,text=True,timeout=30);(out/('red-'+target+'-run.log')).write_text(p.stdout+p.stderr);records.append({'name':target+'-test','argv':a,'cwd':str(root),'exit':p.returncode}); print(target+'-test',p.returncode,flush=True)
(out/'red-commands.json').write_text(json.dumps(records,indent=2))
