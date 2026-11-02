import os, subprocess
from pathlib import Path
p=Path('/tmp/wt-i03-p01-004')
cases=['R42CtaPersistTest.OnInitLoadsPersistedStateAndPreparesOutputs','R42CtaPersistTest.SaveDataRoundTripAcrossContexts','R42CtaPersistTest.MaximumSupportedDetailAndSignalTagsRestoreAndExecute','R81CtaTest.ConditionRecoveryRejectsWholeMalformedSnapshotAndRestoresValidSnapshot','R71CtaTest.LoadDataOptionalDefaultsAndZeroVolumeQueries']
for case in cases:
 folder=p/'display-controls'/case; folder.mkdir(parents=True,exist_ok=True)
 env=os.environ.copy(); env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1'
 r=subprocess.run([str(p/'focused'),'--gtest_filter='+case,'--gtest_output=xml:result.xml'],cwd=folder,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
 (folder/'output.log').write_text(r.stdout); print(case,r.returncode,flush=True)
