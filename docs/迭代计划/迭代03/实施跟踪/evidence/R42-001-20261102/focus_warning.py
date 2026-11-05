from pathlib import Path
root=Path('/tmp/wt-i03-r42-001-20261102/snapshot/src')
for relative, needle, insertion in [
 ('WtBtCore/CMakeLists.txt', b'ADD_LIBRARY(WtBtCore STATIC', b'set_source_files_properties(HftMocker.cpp PROPERTIES COMPILE_OPTIONS -Wno-error=nontrivial-memcall)\r\n'),
 ('WtBtPorter/CMakeLists.txt', b'ADD_LIBRARY(WtBtPorter SHARED', b'set_source_files_properties(ExpHftMocker.cpp WtBtPorter.cpp WtBtRunner.cpp PROPERTIES COMPILE_OPTIONS -Wno-error=nontrivial-memcall)\r\n')]:
 p=root/relative; b=p.read_bytes(); assert b.count(needle)==1; p.write_bytes(b.replace(needle,insertion+needle))
