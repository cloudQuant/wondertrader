from pathlib import Path
import re
p=Path('src/WtCore/HftStraBaseCtx.cpp'); data=p.read_bytes()
for ret,name,value in [('bool','cancel','false'),('OrderIDs','cancel','OrderIDs()'),('OrderIDs','buy','OrderIDs()'),('OrderIDs','sell','OrderIDs()'),('uint32_t','enter_long','0'),('uint32_t','enter_short','0'),('uint32_t','exit_long','0'),('uint32_t','exit_short','0')]:
 pat=(rb'(^'+ret.encode()+rb' HftStraBaseCtx::stra_'+name.encode()+rb'\([^\r\n]*\)\r\n\{\r\n)')
 guard=(b'\tif (_trader == nullptr)\r\n\t{\r\n\t\tlog_error("Cannot '+name.encode()+b': trader adapter is not bound");\r\n\t\treturn '+value.encode()+b';\r\n\t}\r\n\r\n')
 data,n=re.subn(pat,lambda m:m.group(1)+guard,data,flags=re.M)
 assert n==1,(ret,name,n)
p.write_bytes(data)
