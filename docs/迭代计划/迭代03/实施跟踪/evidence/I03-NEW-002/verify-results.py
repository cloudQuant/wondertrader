from pathlib import Path
import xml.etree.ElementTree as ET
root=Path('/private/tmp/wt-i03-new002/run')
for kind in ('normal-v5','asan-v5'):
 files=sorted(root.glob(kind+'-*/result.xml'))
 assert len(files)==24,(kind,len(files))
 for f in files:
  r=ET.parse(f).getroot()
  assert r.get('tests')=='1' and r.get('failures')=='0' and r.get('errors')=='0' and r.get('disabled')=='0',(str(f),r.attrib)
 print(kind, len(files), 'PASS')
