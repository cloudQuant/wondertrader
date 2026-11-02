from pathlib import Path
import xml.etree.ElementTree as ET
base=Path('/tmp/wt-i03-r60-lifecycle')
sets=[('final-green',6),('repeat',20),('t6',3)]
for name,expected in sets:
 files=list((base/name).glob('*/result.xml')); assert len(files)==expected,(name,len(files),expected)
 for f in files:
  r=ET.parse(f).getroot(); tests=int(r.attrib['tests']); errors=int(r.attrib.get('errors',0)); fails=int(r.attrib.get('failures',0)); disabled=int(r.attrib.get('disabled',0));
  assert (tests,errors,fails,disabled)==(1,0,0,0),(f,r.attrib)
 print(name,len(files),'PASS, no disabled')
for name in ('stopped_requests_are_rejected_without_retaining','stopping_closes_request_admission_before_join'):
 f=base/'final-red'/name/'result.xml'; r=ET.parse(f).getroot(); assert int(r.attrib['failures'])==1,(f,r.attrib)
 print('red',name,'expected failure')
