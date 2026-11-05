exec(open('/tmp/wt-i03-core-propagation/build.py').read().split("n='TestUnits/coverage/")[0])
p='TestUnits/coverage/CMakeFiles/CoverageTests.dir/'
d='TestUnits/coverage/CMakeFiles/CoverageTests_dtcore.dir/'
objects=[]
for name in ['WtDtRunner','WtDtPorter','ExpParser','ExpDumper']:
 objects.append(obj(p+'__/__/WtDtPorter/'+name+'.cpp.o','src/WtDtPorter/'+name+'.cpp','dt-'+name))
objects.append(obj(d+'__/__/WtDtCore/ParserAdapter.cpp.o','src/WtDtCore/ParserAdapter.cpp','dt-ParserAdapter'))
objects.append(obj(p+'r22_dtporter/r22_ext_porter_test.cpp.o','src/TestUnits/coverage/r22_dtporter/i03_parser_start_status_test.cpp','dt-test'))
raw=query('CoverageTests'); assert raw.startswith(': && ') and raw.endswith(' && :'); a=shlex.split(raw[5:-5]); a=[x for x in a if not x.endswith('.o')]; a[a.index('-o')+1]=str(out/'dtporter-tests'); a[1:1]=objects; run(a,'dtporter-link')
(out/'dt-commands.json').write_text(json.dumps(rec,indent=2))
