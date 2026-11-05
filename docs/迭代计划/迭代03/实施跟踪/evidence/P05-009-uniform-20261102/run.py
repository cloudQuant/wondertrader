import os, subprocess, json, pathlib, hashlib, sys, xml.etree.ElementTree as ET
out=pathlib.Path(__file__).resolve().parent
root=pathlib.Path('/tmp/wt-i03-p05-009-uniform')
binary=root/'build/p05009'
cases=['P05ReadBarsBufferHandlesEmptySingleAndClosedRange','P05ReadBarsBufferRejectsMalformedValuesWithoutPublishingPartial','P05ReadBarsBufferRejectsQueryErrorsWithoutPublishingPartial','P05ReadBarsBufferRejectsInvalidIdentityAndPeriod','P05ReadBarsBufferDistinguishesEmptyFromDatabaseOpenFailure']
env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
rows=[]
for n,case in enumerate(cases):
    work=root/('physical-'+str(n)); work.mkdir(exist_ok=True)
    argv=[str(binary),'--gtest_filter=R4ReaderADTest.'+case,'--gtest_output=xml:'+str(out/('physical-'+case+'.xml'))]
    try:
        p=subprocess.run(argv,cwd=work,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
        code=p.returncode; text=p.stdout.decode(errors='replace')
    except subprocess.TimeoutExpired as ex:
        code='TIMEOUT'; text=(ex.stdout or b'').decode(errors='replace')
    (out/('physical-'+case+'.log')).write_text(text)
    xml_path=out/('physical-'+case+'.xml')
    root_xml=ET.parse(xml_path).getroot() if code == 0 and xml_path.exists() else None
    passed=(root_xml is not None and root_xml.get('tests') == '1' and root_xml.get('failures') == '0' and root_xml.get('errors') == '0' and root_xml.get('disabled') == '0')
    rows.append(dict(argv=argv,cwd=str(work),exit=code,verified_one_test=passed))
    print(case,code,'PASS' if passed else 'FAIL',flush=True)
(out/'physical-results.json').write_text(json.dumps(rows,indent=2)+'\n')
entries=['WtDataStorageAD/WtDataReaderAD.cpp','WtDataStorageAD/WtDataReaderAD.h','WtDataStorageAD/LMDBKeys.h','WTSUtils/WtLMDB.hpp','Includes/WTSStruct.h','TestUnits/coverage/p05_datastorage2/r4_reader_ad_slices_test.cpp','TestUnits/coverage/p05_datastorage2/r4_ad_fixture.h']
with (out/'physical-sources.sha256').open('w') as f:
    for rel in entries:
        for label,p in [('snapshot',root/'src'/rel),('workspace',pathlib.Path('src')/rel)]:
            f.write(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+label+'/'+rel+'\n')
    f.write(hashlib.sha256(binary.read_bytes()).hexdigest()+'  binary/p05009\n')

if not all(row['exit'] == 0 and row['verified_one_test'] for row in rows):
    sys.exit(1)
