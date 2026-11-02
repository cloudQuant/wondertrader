from pathlib import Path
import xml.etree.ElementTree as ET
p=Path('/private/tmp/wt-i03-r15-tags/run')
for kind in ('final','asan-final'):
 rows=list(sorted(p.glob(kind+'-*/result.xml')))
 print(kind,'files',len(rows))
 assert len(rows)==7
 for f in rows:
  root=ET.parse(f).getroot(); assert root.get('tests')=='1' and root.get('failures')=='0' and root.get('disabled')=='0' and root.get('errors')=='0', (str(f),root.attrib)
  print(f.parent.name,'PASS')
