import json, os, pathlib, shlex, subprocess, sys
root=pathlib.Path('/Users/yunjinqi/Documents/new_projects/wondertrader'); build=pathlib.Path('/tmp/ctpm-i03-build'); out=pathlib.Path('/tmp/wt-i03-servo-compat'); out.mkdir(exist_ok=True); fixture=out/'parsers/libR45DtServoParserFixture.dylib'; fixture.parent.mkdir(parents=True,exist_ok=True); records=[]
def query(target): return subprocess.run(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands',target],capture_output=True,text=True,check=True).stdout.strip().splitlines()[-1]
def run(args,tag):
 p=subprocess.run(args,cwd=build,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(out/(tag+'.log')).write_text(p.stdout); records.append({'tag':tag,'argv':args,'cwd':str(build),'exit':p.returncode}); (out/'commands.json').write_text(json.dumps(records,indent=2)); print(tag,p.returncode,flush=True)
 if p.returncode: print(p.stdout[-6000:]);sys.exit(p.returncode)
def compile_obj(target,name):
 a=shlex.split(query(target)); dest=out/(name+'.o')
 for flag in ['-o','-MF','-MT']:
  if flag in a:a[a.index(flag)+1]=str(dest if flag in ('-o','-MT') else dest.with_suffix('.d'))
 for i,arg in enumerate(a):
  if arg.startswith('-DWT_R45_DTSERVO_PARSER_FIXTURE_PATH='): a[i]='-DWT_R45_DTSERVO_PARSER_FIXTURE_PATH="'+str(fixture)+'"'
 run(a,name)
target='TestUnits/coverage/CMakeFiles/R45DtServoParserFixture.dir/__/fixtures/r45_dtservo_parser_fixture.cpp.o';compile_obj(target,'fixture')
raw=query('R45DtServoParserFixture');assert raw.startswith(': && ') and raw.endswith(' && :');a=shlex.split(raw[5:-5]);a[a.index('-o')+1]=str(fixture);a=[str(out/'fixture.o') if x.endswith('.o') else x for x in a];run(a,'fixture-link')
prefix='TestUnits/coverage/CMakeFiles/CoverageTests_dtservo.dir/'
compile_obj(prefix+'__/__/WtDtServo/ParserAdapter.cpp.o','adapter');compile_obj(prefix+'__/__/WtDtServo/WtDtRunner.cpp.o','runner');compile_obj(prefix+'__/__/WtDtServo/WtDataManager.cpp.o','data');compile_obj(prefix+'__/__/WtDtServo/WtHelper.cpp.o','helper');compile_obj(prefix+'__/__/WtDtServo/WtDtServo.cpp.o','exports');compile_obj(prefix+'q05b_dtservo/r45_parseradapter_test.cpp.o','test')
raw=query('CoverageTests_dtservo');assert raw.startswith(': && ') and raw.endswith(' && :');a=shlex.split(raw[5:-5]);a=[x for x in a if not x.endswith('.o')];a[a.index('-o')+1]=str(out/'tests');a[1:1]=[str(out/(x+'.o')) for x in ('adapter','runner','data','helper','exports','test')];run(a,'link')
filters=['ExportedInitializationStateRemainsVisibleDuringConnect','ExportedStatusReportsSuccessfulConnectSubmission','ExportedStatusReportsConnectSubmissionFailure','RunnerReportsConnectSubmissionFailureAndRetainsOwner','RunnerStartsInitializedFakeParser','ManagerReportsPartialConnectSubmission','ManagerRunsAndReleasesLiveAdapters','RunReportsConnectionCommandSubmission']
for name in filters:
 d=out/name;d.mkdir(exist_ok=True);env=os.environ.copy();env['TMPDIR']=str(d)+'/';env['LLVM_PROFILE_FILE']=str(d/'profile.profraw');args=['perl','-e','alarm 30; exec @ARGV','--',str(out/'tests'),'--gtest_filter=R45ParserAdapterFakeTest.'+name,'--gtest_output=xml:'+str(d/'result.xml')];p=subprocess.run(args,cwd=d,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);(d/'output.log').write_text(p.stdout);records.append({'tag':name,'argv':args,'cwd':str(d),'exit':p.returncode});(out/'commands.json').write_text(json.dumps(records,indent=2));print(name,p.returncode,flush=True)
 if p.returncode: print(p.stdout);sys.exit(p.returncode)
