#!/bin/bash
set -euo pipefail
repo=$(git rev-parse --show-toplevel)
out=${1:?absolute, previously nonexistent output directory required}
case "$out" in /*) ;; *) exit 2;; esac
test ! -e "$out"
mkdir -p "$out"
find "$repo/src" -type l -print > "$out/source-symlinks.txt"
test ! -s "$out/source-symlinks.txt"
sources=(src/WtDataStorage/WtDataWriter.cpp src/WtDataStorage/WtDataWriter.h
 src/TestUnits/coverage/p10_storage_writer/p10_rt_block_files_test.cpp
 src/TestUnits/coverage/p10_storage_writer/p10_writer_prefix.h
 src/TestUnits/coverage/p10_storage_writer/p10_writer_test_hooks.h
 src/TestUnits/fixtures/p10_first_insert/first_insert_test.cpp
 src/Includes/WTSSessionInfo.hpp src/Share/TimeUtils.hpp)
(cd "$repo" && shasum -a 256 "${sources[@]}") > "$out/source-before.sha256"
cp -R "$repo/src" "$out/src"
(cd "$repo" && shasum -a 256 "${sources[@]}") > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
for source in "${sources[@]}"; do
    cmp "$repo/$source" "$out/$source"
done

# Insert barrier callbacks ONLY in the copied writer source. They run in the
# existing map-lock critical section and do not alter the product source.
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python - "$out/src/WtDataStorage/WtDataWriter.cpp" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text(encoding='utf-8-sig')
anchor='namespace fs = boost::filesystem;'
assert s.count(anchor)==1
s=s.replace(anchor,anchor+'\nextern "C" void p10_fixture_insert(WtDataWriter*, unsigned);\nextern "C" void p10_fixture_snapshot(WtDataWriter*, unsigned, long, std::weak_ptr<void>);')
for name,kind in [('getTickBlock',1),('getTransBlock',2),('getOrdDtlBlock',3),('getOrdQueBlock',4),('getKlineBlock',5)]:
    start=s.index('WtDataWriter::'+name+'(')
    at=s.index('pBlock = inserted.first->second;',start)+len('pBlock = inserted.first->second;')
    code='\n            p10_fixture_insert(this, '+('period == KP_Minute1 ? 5 : 6' if kind==5 else str(kind))+');'
    s=s[:at]+code+s[at:]
for field,kind in [('ticks',1),('trans',2),('order_details',3),('order_queues',4),('min1',5),('min5',6)]:
    needle={'ticks':'copyEntries(_rt_ticks_blocks, snapshot.ticks);','trans':'copyEntries(_rt_trans_blocks, snapshot.trans);','order_details':'copyEntries(_rt_orddtl_blocks, snapshot.order_details);','order_queues':'copyEntries(_rt_ordque_blocks, snapshot.order_queues);','min1':'copyEntries(_rt_min1_blocks, snapshot.min1);','min5':'copyEntries(_rt_min5_blocks, snapshot.min5);'}[field]
    assert s.count(needle)==1
    extra=f'\n if (!snapshot.{field}.empty()) p10_fixture_snapshot(this, {kind}, snapshot.{field}.front().second.use_count(), std::weak_ptr<void>(snapshot.{field}.front().second));'
    s=s.replace(needle,needle+extra)
p.write_text(s)
PY

cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
src="$out/src"
common=(-std=gnu++17 -g -O1 -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DWT_DATA_WRITER_TEST_HOOKS -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/usr/local/include -I"$src" -include stdlib.h -include "$src/Share/fmt_enum_compat.h")
for variant in normal tsan; do
 flags=(-Wno-unused-command-line-argument)
 if [[ "$variant" == tsan ]]; then flags=(-fsanitize=thread); fi
 "$cxx" "${common[@]}" "${flags[@]}" -Dinst_hlp=p10_fixture_product -c "$src/WtDataStorage/WtDataWriter.cpp" -o "$out/$variant-writer.o"
 "$cxx" "${common[@]}" "${flags[@]}" -Dinst_hlp=p10_fixture_test -c "$src/TestUnits/fixtures/p10_first_insert/first_insert_test.cpp" -o "$out/$variant-test.o"
 "$cxx" -g "${flags[@]}" "$out/$variant-writer.o" "$out/$variant-test.o" /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -L/opt/homebrew/Cellar/boost/1.88.0/lib -Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib -pthread -Wl,-undefined,dynamic_lookup -o "$out/$variant-test"
done
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
