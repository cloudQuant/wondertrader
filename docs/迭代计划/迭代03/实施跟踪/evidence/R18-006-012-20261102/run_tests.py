from pathlib import Path
import os, subprocess, json
root=Path('/tmp/wt-i03-r18-006-012')
exe=root/'build/build_x64/Debug/bin/TestUnits/CoverageTests_dtcore'
names=['InitExtReturnsFalseWhenApiInitFails','InitReleasesRetainedConfigWhenNoApiWasCreated','ReleaseReturnsRetainedConfigWhenModuleLoadFails','InitExtSubscribesEveryContractFullCode','ReleaseIsIdempotent','ModuleConfigReferenceOnFailedAndSuccessfulInit']
results=[]
for i,name in enumerate(names):
    out=root/'isolated'/str(i);out.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy();env['LLVM_PROFILE_FILE']=str(out/'profile.profraw')
    cmd=[str(exe),'--gtest_filter=R18ParserAdapterTest.'+name,'--gtest_output=xml:'+str(out/'result.xml')]
    try:
        p=subprocess.run(cmd,cwd=out,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=45)
        rc=p.returncode; log=p.stdout
    except subprocess.TimeoutExpired as e:
        rc='timeout';log=(e.stdout or b'').decode(errors='replace') if isinstance(e.stdout,bytes) else (e.stdout or '')
    (out/'result.log').write_text(log)
    results.append({'name':name,'argv':cmd,'cwd':str(out),'exit':rc})
(root/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(results,ensure_ascii=False,indent=2))
