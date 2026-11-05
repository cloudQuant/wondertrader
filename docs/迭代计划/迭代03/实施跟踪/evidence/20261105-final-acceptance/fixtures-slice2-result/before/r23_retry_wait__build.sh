#!/bin/bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
out=${1:?provide a new absolute output directory}
case "$out" in /*) ;; *) exit 2;; esac
test ! -e "$out"
mkdir -p "$out"
cd "$repo"
sources=(src/WtDataStorage/WtDataWriter.cpp src/WtDataStorage/WtDataWriter.h
 src/TestUnits/fixtures/r23_retry_wait/probe.cpp
 src/TestUnits/coverage/p10_storage_writer/p10_writer_prefix.h
 src/Includes/WTSSessionInfo.hpp src/Share/TimeUtils.hpp)
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
cp -R src "$out/src"
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
for source in "${sources[@]}"; do cmp "$repo/$source" "$out/$source"; done
src="$out/src"
cp "$src/WtDataStorage/WtDataWriter.cpp" "$out/current.cpp"
cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
common=(-std=gnu++17 -g -O1 -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL
 -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include
 -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/opt/homebrew/Cellar/googletest/1.17.0/include
 -I/usr/local/include -I"$src" -include stdlib.h -include "$src/Share/fmt_enum_compat.h")
for variant in baseline normal asan tsan; do
 cp "$out/current.cpp" "$src/WtDataStorage/WtDataWriter.cpp"
 /Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python - "$src/WtDataStorage/WtDataWriter.cpp" "$variant" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text(encoding='utf-8-sig'); variant=sys.argv[2]
anchor='namespace fs = boost::filesystem;'
assert s.count(anchor)==1
s=s.replace(anchor,anchor+'\nextern "C" void r23_before_remove();\nextern "C" void r23_before_wait();')
anchor='std::string path = fmtutil::format("{}rt/min1/", _base_dir);'
assert s.count(anchor)==1
s=s.replace(anchor,'r23_before_remove();\n'+anchor)
anchor='StdUniqueLock procLock(_proc_mtx);\n\t\t\t\t\t_proc_cond.wait_for(procLock, std::chrono::seconds(300),\n\t\t\t\t\t\t[this]() { return _proc_stopping; });'
assert s.count(anchor)==1
if variant=='baseline':
 s=s.replace(anchor,'r23_before_wait();\n\t\t\t\t\tstd::this_thread::sleep_for(std::chrono::seconds(300));')
else:
 s=s.replace(anchor,anchor.replace('_proc_cond.wait_for', 'r23_before_wait();\n\t\t\t\t\t_proc_cond.wait_for'))
p.write_text(s)
PY
 flags=(-Wno-unused-command-line-argument)
 [[ "$variant" == asan ]] && flags=(-fsanitize=address,undefined)
 [[ "$variant" == tsan ]] && flags=(-fsanitize=thread)
 "$cxx" "${common[@]}" "${flags[@]}" -Dinst_hlp=r23_retry_writer -c "$src/WtDataStorage/WtDataWriter.cpp" -o "$out/$variant-writer.o"
 "$cxx" "${common[@]}" "${flags[@]}" -Dinst_hlp=r23_retry_test -c "$src/TestUnits/fixtures/r23_retry_wait/probe.cpp" -o "$out/$variant-probe.o"
 "$cxx" "${flags[@]}" "$out/$variant-writer.o" "$out/$variant-probe.o" /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a -L/opt/homebrew/Cellar/boost/1.88.0/lib -Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib -pthread -Wl,-undefined,dynamic_lookup -o "$out/$variant-probe"
done
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
