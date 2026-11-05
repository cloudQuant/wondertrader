import json, os, subprocess
from pathlib import Path
root=Path('/tmp/wt-i03-adreader-asan-20261102')
cases=json.loads((root/'final-results.json').read_text())
results=[]
for item in cases:
    case=item['case']
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
    proc=subprocess.run([str(root/'build/p05009'),'--gtest_filter=R4ReaderADTest.'+case],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env)
    output=proc.stdout.decode(errors='replace')
    (root/('tristate-'+case+'.log')).write_text(output)
    results.append({'case':case,'exit':proc.returncode,'pass':proc.returncode==0 and '[  PASSED  ] 1 test.' in output})
(root/'tristate-results.json').write_text(json.dumps(results,indent=2))
print(f'{sum(x["pass"] for x in results)}/{len(results)} PASS')
raise SystemExit(any(not x['pass'] for x in results))
