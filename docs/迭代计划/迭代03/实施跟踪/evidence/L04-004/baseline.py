import pathlib, subprocess, shlex
out=pathlib.Path('/tmp/wt-i03-l04-contract'); root='/Users/yunjinqi/Documents/new_projects/wondertrader/src'; old='/tmp/wt-i02-fresh-20261101-v8-src/src'; build='/tmp/wt-i02-fresh-20261101-v8-build'
s=pathlib.Path(root+'/WTSTools/WTSBaseDataMgr.cpp').read_text()
block='''\t\t\t// Contract identities also enter AD's fixed-width exchange/code keys.\n\t\t\t// Reject before creating a rules-derived commodity or registering a contract.\n\t\t\tif (exchg.size() >= MAX_EXCHANGE_LENGTH || code.size() >= MAX_INSTRUMENT_LENGTH ||\n\t\t\t\tcontractExchg == nullptr || strlen(contractExchg) >= MAX_EXCHANGE_LENGTH)\n\t\t\t{\n\t\t\t\tWTSLogger::error("Contract identity exceeds storage field capacity: {}.{}", exchg.c_str(), code.c_str());\n\t\t\t\tallValid = false;\n\t\t\t\tcontinue;\n\t\t\t}\n'''
assert s.count(block)==1
s=s.replace(block,'').replace('#include "../Includes/WTSMarcos.h"\n','',1)
base=out/'baseline_manager.cpp'; base.write_text(s)
cmds=subprocess.check_output(['/Users/yunjinqi/opt/anaconda3/bin/ninja','-C',build,'-t','commands','CoverageTests'],text=True).splitlines()
line=next(x for x in cmds if 'WTSBaseDataMgr.cpp.o -c ' in x)
a=[x.replace(old,root) for x in shlex.split(line)]
for flag in ('-MT','-MF'):
 i=a.index(flag); del a[i:i+2]
a[a.index('-o')+1]=str(out/'baseline_manager.o'); a[a.index('-c')+1]=str(base); a[1:1]=['-I'+root+'/WTSTools','-I'+build+'/TestUnits/coverage/cov_boost_include']
subprocess.run(a,cwd=build,check=True)
line=next(x for x in cmds if ' -o build_x64/Debug/bin/TestUnits/CoverageTests ' in x)
a=shlex.split(line)[2:-2]; a[next(i for i,x in enumerate(a) if x.endswith('/l03_includes/l03_contract_info_test.cpp.o'))]=str(out/'test.o'); a[a.index('-o')+1]=str(out/'baseline'); a.insert(a.index('/opt/homebrew/lib/libgtest_main.a'),str(out/'baseline_manager.o'))
subprocess.run(a,cwd=build,check=True)
