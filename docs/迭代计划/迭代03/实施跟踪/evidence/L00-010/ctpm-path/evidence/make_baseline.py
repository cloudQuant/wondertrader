from pathlib import Path
src=Path('src'); dest=Path('/tmp/ctpm-i03-evidence/baseline');dest.mkdir()
p=src/'ParserCTPMini/ParserCTPMini.cpp'; text=p.read_text(encoding='utf-8-sig')
start='\tstd::string module = config->getCString("ctpmodule");\n'; end='\n\tstd::string path = fmtutil::format('
a=text.index(start,text.index('bool ParserCTPMini::init')); b=text.index(end,a)
text=text[:a]+text[b:]
old='\tstd::string dllpath = (moduleDir == NULL ? std::string() : std::string(moduleDir)) + moduleFile;'
new='\tstd::string module = config->getCString("ctpmodule");\n\tif (module.empty())\n\t\tmodule = "thostmduserapi";\n\tstd::string dllpath = getBinDir() + DLLHelper::wrap_module(module.c_str(), "lib");;'
assert text.count(old)==1;text=text.replace(old,new);(dest/p.name).write_text(text)
p=src/'TraderCTPMini/TraderCTPMini.cpp';text=p.read_text(encoding='utf-8-sig')
a=text.index('\tconst std::string moduleFile = DLLHelper::wrap_module(',text.index('bool TraderCTPMini::init'));b=text.index('\n\tDllHandle candidateHandle',a)
text=text[:a]+'\tm_strModule = getBinDir() + DLLHelper::wrap_module(module.c_str(), "");\n'+text[b:];(dest/p.name).write_text(text)
