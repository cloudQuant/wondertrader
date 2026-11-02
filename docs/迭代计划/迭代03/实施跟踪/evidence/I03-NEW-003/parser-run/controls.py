import os,pathlib,subprocess,json
base=pathlib.Path('/tmp/wt-i03-parser-run/green');cases={'wtcore':'N01ParserAdapter.init_ext_defaults_time_check_off_and_delivers_quote','dtcore':'R18ParserAdapterTest.InitExtSubscribesEveryContractFullCode','dtservo':'R45ParserAdapterFakeTest.InitExtRegistersSpiAndSubscribesAllContracts'};rows=[]
for target,case in cases.items():
 d=base/(target+'-control');d.mkdir(exist_ok=True);a=['perl','-e','alarm 30; exec @ARGV','--',str(base/(target+'-tests')),'--gtest_filter='+case,'--gtest_output=xml:'+str(d/'result.xml')];e=os.environ.copy();e['TMPDIR']=str(d)+'/';e['LLVM_PROFILE_FILE']=str(d/'profile.profraw');p=subprocess.run(a,cwd=d,env=e,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(d/'output.log').write_text(p.stdout);rows.append({'target':target,'case':case,'cwd':str(d),'argv':a,'exit':p.returncode});print(target,p.returncode,flush=True)
(base/'controls.json').write_text(json.dumps(rows,indent=2))
