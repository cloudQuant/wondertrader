import json,os,pathlib,subprocess
base=pathlib.Path('/tmp/wt-i03-new053');binary=base/'bin/fixture-asan'; d=base/'run'/'group_no_profile';d.mkdir(parents=True,exist_ok=True); env=os.environ.copy(); env.pop('LLVM_PROFILE_FILE',None);env.update({'TMPDIR':str(d)+'/','ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','LSAN_OPTIONS':'exitcode=23:report_objects=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'});filt='P11TraderTest.init_rejects_overlong_ids_and_keeps_empty_id_accepted:P11TraderTest.init_ext_rejects_long_ids_before_touching_api_or_files:P11TraderTest.longest_representable_id_produces_a_terminated_user_tag:R59Misc.init_ext_guards_and_run_without_api';cmd=[str(binary),'--gtest_filter='+filt,'--gtest_output=xml:'+str(d/'result.xml')]
try:
 with (d/'output.log').open('w') as f:r=subprocess.run(cmd,cwd=d,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=45)
 code=r.returncode
except subprocess.TimeoutExpired:code='TIMEOUT'
(base/'group-no-profile-result.json').write_text(json.dumps({'case_filter':filt,'command':cmd,'cwd':str(d),'LLVM_PROFILE_FILE':'unset','asan_options':env['ASAN_OPTIONS'],'lsan_options':env['LSAN_OPTIONS'],'ubsan_options':env['UBSAN_OPTIONS'],'exit':code},indent=2)+'\n');print('group_no_profile exit',code)
