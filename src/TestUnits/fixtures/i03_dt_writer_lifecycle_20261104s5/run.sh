#!/bin/bash
# dt-writer lifecycle fixture build (iteration 03, 2026-11-04, package dt-writer).
#
# usage: run.sh <src_root> <out_dir> <normal|asan|tsan> [baseline]
#   src_root  tree whose DataManager.cpp / WtHelper.cpp / WtDataStorage/*.cpp are tested
#   out_dir   new, absolute output directory (must not exist)
#   baseline  compile the test with -DS5_BASELINE_HOST (pre-fix DataManager has no observer)
#
# Fake writer modules are always compiled from this fixture's own repository tree (they
# need the V1 declarations); the old host simply ignores the extra export.
# Prebuilt, uninstrumented WTSTools/WTSUtils archives come from $S5_LIBS
# (default: the isolated snapshot build /tmp/wt-i03-s5-dt-writer-after/dbg; WTSTools/WTSUtils are not modified by this package).
# Creates no symbolic links.
set -euo pipefail
src=${1:?src_root}; out=${2:?out_dir}; variant=${3:?variant}; mode=${4:-}
case "$out" in /*) ;; *) echo "out_dir must be absolute"; exit 2;; esac
test ! -e "$out"
here=$(cd "$(dirname "$0")" && pwd)
repo_src=$(cd "$here/../../.." && pwd)
libs=${S5_LIBS:-/tmp/wt-i03-s5-dt-writer-after/dbg/build_x64/Debug/libs}
mkdir -p "$out/mods" "$out/obj"

case "$variant" in
  normal) cxx=/usr/bin/c++; san=(); extra=() ;;
  asan)   cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++; san=(-fsanitize=address,undefined -fno-omit-frame-pointer); extra=(-include stdlib.h) ;;
  tsan)   cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++; san=(-fsanitize=thread -fno-omit-frame-pointer); extra=(-include stdlib.h) ;;
  *) echo "unknown variant"; exit 2 ;;
esac
warn=(-Wall -Wextra -Werror -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages")
common=(-g -O1 -std=gnu++17 -arch arm64 -fPIC -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL ${extra[@]+"${extra[@]}"} ${san[@]+"${san[@]}"})
inc_for() { echo "-I/opt/homebrew/include -I/usr/local/include -I$1 -include $1/Share/fmt_enum_compat.h"; }
gtest=/opt/homebrew/Cellar/googletest/1.17.0
syslibs=(-L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread /usr/local/lib/libiconv.dylib)

{
  echo "variant=$variant mode=$mode src=$src repo_src=$repo_src libs=$libs cxx=$cxx"
  "$cxx" --version | head -1
  (cd "$src" && shasum -a 256 WtDtCore/DataManager.cpp WtDtCore/DataManager.h WtDataStorage/WtDataWriter.cpp WtDataStorage/WtDataWriter.h WtDataStorageAD/WtDataWriterAD.cpp WtDataStorageAD/WtDataWriterAD.h Includes/IDataWriter.h)
  (cd "$repo_src" && shasum -a 256 TestUnits/fixtures/i03_dt_writer_lifecycle_20261104s5/*.cpp TestUnits/fixtures/i03_dt_writer_lifecycle_20261104s5/*.h)
  shasum -a 256 "$libs/libWTSTools.a" "$libs/libWTSUtils.a"
} > "$out/build-info.txt"

# 1. writer module from the tree under test (hidden visibility like the CMake target)
objs=()
for f in "$src"/WtDataStorage/*.cpp; do
  o="$out/obj/storage_$(basename "$f" .cpp).o"
  "$cxx" "${common[@]}" "${warn[@]}" -fvisibility=hidden -fvisibility-inlines-hidden $(inc_for "$src") \
    -Dinst_hlp=s5_storage_$(basename "$f" .cpp) -c "$f" -o "$o"
  objs+=("$o")
done
"$cxx" ${san[@]+"${san[@]}"} -g -arch arm64 -dynamiclib -install_name @rpath/libWtDataStorage.dylib \
  -o "$out/mods/libWtDataStorage.dylib" "${objs[@]}" "$libs/libWTSUtils.a" "${syslibs[@]}"

# 1b. AD writer module from the tree under test (lifecycle V1 since 2026-11-04)
adobjs=()
for f in "$src"/WtDataStorageAD/*.cpp; do
  o="$out/obj/storagead_$(basename "$f" .cpp).o"
  "$cxx" "${common[@]}" "${warn[@]}" -fvisibility=hidden -fvisibility-inlines-hidden $(inc_for "$src") \
    -Dinst_hlp=s5_storagead_$(basename "$f" .cpp) -c "$f" -o "$o"
  adobjs+=("$o")
done
"$cxx" ${san[@]+"${san[@]}"} -g -arch arm64 -dynamiclib -install_name @rpath/libWtDataStorageAD.dylib \
  -o "$out/mods/libWtDataStorageAD.dylib" "${adobjs[@]}" "$libs/libWTSUtils.a" "${syslibs[@]}"

# 2. fake writer modules (fixture-owned, current repository headers)
fake() {
  local name=$1; shift
  "$cxx" "${common[@]}" -Wall -Wextra -Werror -fvisibility=hidden $(inc_for "$repo_src") "$@" \
    -dynamiclib -install_name "@rpath/lib$name.dylib" -o "$out/mods/lib$name.dylib" "$here/fake_writer.cpp"
}
fake s5_fake_legacy
fake s5_fake_v1 -DS5_FAKE_V1
fake s5_fake_v1_async -DS5_FAKE_V1 -DS5_FAKE_ASYNC

# 3. host + tests
defs=()
if [[ "$mode" == baseline ]]; then defs=(-DS5_BASELINE_HOST); fi
"$cxx" "${common[@]}" "${warn[@]}" $(inc_for "$src") -Dinst_hlp=s5_datamgr -c "$src/WtDtCore/DataManager.cpp" -o "$out/obj/DataManager.o"
"$cxx" "${common[@]}" "${warn[@]}" $(inc_for "$src") -Dinst_hlp=s5_wthelper -c "$src/WtDtCore/WtHelper.cpp" -o "$out/obj/WtHelper.o"
for t in lifecycle_test p10_008_reentry_test; do
  "$cxx" "${common[@]}" -Wall -Wextra -Wno-unused-parameter ${defs[@]+"${defs[@]}"} $(inc_for "$src") -I"$gtest/include" \
    -Dinst_hlp=s5_$t -c "$here/$t.cpp" -o "$out/obj/$t.o"
done
"$cxx" ${san[@]+"${san[@]}"} -g -arch arm64 -o "$out/s5_lifecycle_tests" \
  "$out/obj/lifecycle_test.o" "$out/obj/p10_008_reentry_test.o" "$out/obj/DataManager.o" "$out/obj/WtHelper.o" \
  "$libs/libWTSTools.a" "$libs/libWTSUtils.a" "$gtest/lib/libgtest_main.a" "$gtest/lib/libgtest.a" "${syslibs[@]}"

find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
(cd "$out" && shasum -a 256 s5_lifecycle_tests mods/*.dylib) > "$out/binaries.sha256"
echo "built $out"
