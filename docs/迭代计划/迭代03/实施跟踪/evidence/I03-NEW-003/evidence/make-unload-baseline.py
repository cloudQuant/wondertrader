from pathlib import Path
p=Path('src/ParserCTPMini/ParserCTPMini.cpp'); s=p.read_text(encoding='utf-8-sig')
block='\t// Loading a library can already run vendor initialization. Keep its loader\n\t// reference even when symbol lookup or API creation fails: neither path has\n\t// a documented callback/thread quiescence guarantee.\n\tm_hInstCTP = library;\n'
assert s.count(block)==1;s=s.replace(block,'')
old='\t\twrite_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API creator symbol not found; module retained: {}", creatorName);'
new='\t\twrite_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API creator symbol not found: {}", creatorName);\n\t\tDLLHelper::free_library(library);'
assert s.count(old)==1;s=s.replace(old,new)
old='\t\twrite_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API creator returned null; module retained: {}", logPath);'
new='\t\twrite_log(m_sink, LL_ERROR, "[ParserCTPMini] Market data API creator returned null: {}", logPath);\n\t\tDLLHelper::free_library(library);'
assert s.count(old)==1;s=s.replace(old,new)
old='\tm_funcCreator = creator;';assert s.count(old)==1;s=s.replace(old,'\tm_hInstCTP = library;\n'+old)
Path('/tmp/ctpm-i03-init/unload-baseline.cpp').write_text(s)
