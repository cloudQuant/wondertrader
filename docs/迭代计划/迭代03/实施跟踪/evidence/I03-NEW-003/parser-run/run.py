import os,pathlib,subprocess,json,sys
base=pathlib.Path('/tmp/wt-i03-parser-run'); mode=sys.argv[1]; filters={'wtcore':'N01ParserAdapter.run_reports_connection_command_submission','dtcore':'R18ParserAdapterTest.RunReportsConnectionCommandSubmission','dtservo':'R45ParserAdapterFakeTest.RunReportsConnectionCommandSubmission'}; records=[]
for target,case in filters.items():
 d=base/mode/(target+'-run');d.mkdir(exist_ok=True);args=['perl','-e','alarm 30; exec @ARGV','--',str(base/mode/(target+'-tests')),'--gtest_filter='+case,'--gtest_output=xml:'+str(d/'result.xml')];env=os.environ.copy();env['TMPDIR']=str(d)+'/';env['LLVM_PROFILE_FILE']=str(d/'profile.profraw');p=subprocess.run(args,cwd=d,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);(d/'output.log').write_text(p.stdout);records.append({'target':target,'case':case,'cwd':str(d),'argv':args,'exit':p.returncode});print(target,p.returncode,flush=True)
(base/mode/'results.json').write_text(json.dumps(records,indent=2))
