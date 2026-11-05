from pathlib import Path

root = Path('/tmp/wt-i03-r6-004-20261102/snapshot/src')
root_cmake = root / 'CMakeLists.txt'
content = root_cmake.read_bytes()
marker = b'# \xe5\x8c\x85\xe5\x90\xab libiconv \xe7\x9a\x84\xe5\xa4\xb4\xe6\x96\x87\xe4\xbb\xb6\xe8\xb7\xaf\xe5\xbe\x84'
if content.count(marker) != 1:
    raise SystemExit('root CMake insertion marker missing or duplicated')
content = content.replace(marker, b'include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)\r\n' + marker, 1)
root_cmake.write_bytes(content)

coverage = root / 'TestUnits/coverage/CMakeLists.txt'
content = coverage.read_bytes()
marker = b'\tWT_COV_ISOLATE_INST_HLP(${tus})'
if content.count(marker) != 1:
    raise SystemExit('coverage CMake insertion marker missing or duplicated')
addition = (b'\tIF(suffix STREQUAL "porter")\n'
            b'\t\tLIST(FILTER srcs INCLUDE REGEX "/r82_porter_runner_edges_test\\\\.cpp$")\n'
            b'\tENDIF()\n')
coverage.write_bytes(content.replace(marker, addition + marker, 1))
