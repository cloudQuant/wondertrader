from pathlib import Path
import sys

def function(src, marker):
 start=src.index(marker)
 brace=src.index('{',start)
 depth=0
 for i in range(brace,len(src)):
  if src[i]=='{': depth+=1
  elif src[i]=='}':
   depth-=1
   if depth==0:return src[start:i+1]
 raise RuntimeError('missing end')

runner=Path('src/WtPorter/WtRtRunner.cpp').read_text()
porter=Path('src/WtPorter/WtPorter.cpp').read_text()
body=function(runner,'uint32_t WtRtRunner::createSelContext(')
export=function(porter,'CtxHandler create_sel_context(')
if len(sys.argv)>2 and sys.argv[2]=='baseline':
 before='\tif (period == NULL)\n\t\treturn 0;\n\n'
 assert body.count(before)==1
 body=body.replace(before,'',1)
 before='\telse\n\t\treturn 0;\n\n\tExpSelContext* ctx'
 assert body.count(before)==1
 body=body.replace(before,'\telse\n\t\tptype = TPT_None;\n\n\tExpSelContext* ctx',1)
Path(sys.argv[1]).write_text('#include "test_support.hpp"\n'+body+'\n'+export+'\n')
