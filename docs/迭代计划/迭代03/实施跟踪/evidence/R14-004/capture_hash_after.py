from pathlib import Path
import hashlib,json
base=Path('/Users/yunjinqi/Documents/new_projects/wondertrader')
snap=Path('/tmp/wt-r14-resume/source')
files=['src/QuoteFactory/main.cpp','src/WtBtRunner/WtBtRunner.cpp','src/WtBtCore/HisDataReplayer.cpp','src/WtDataStorage/WtDataWriter.cpp','src/TestUnits/coverage/m_QuoteFactory/r14_quotefactory_main_test.cpp','src/TestUnits/coverage/m_QuoteFactory/r14_quotefactory_cli_valid_control_test.cpp','src/TestUnits/coverage/m_WtBtRunner/r14_wtbtrunner_cli_test.cpp','src/Share/ModuleHelper.hpp','src/TestUnits/coverage/CMakeLists.txt']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
out={f:{'workspace':sha(base/f),'snapshot':sha(snap/f)} for f in files}
Path('/tmp/wt-r14-resume/input-after.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
