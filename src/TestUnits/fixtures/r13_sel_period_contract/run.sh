#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:?new output directory required}
test ! -e "$out"
mkdir -p "$out"
out=$(cd "$out" && pwd)
cd "$root"
fixture=src/TestUnits/fixtures/r13_sel_period_contract
sources=(src/WtPorter/WtRtRunner.cpp src/WtPorter/WtPorter.cpp "$fixture/extract.py" "$fixture/test_support.hpp" "$fixture/check.cpp" "$fixture/run.sh")
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python "$fixture/extract.py" "$out/current.cpp"
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python "$fixture/extract.py" "$out/baseline.cpp" baseline
cc=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for variant in normal asan; do
 flags=(-std=c++17 -g -O1 -fno-omit-frame-pointer)
 if [[ "$variant" == asan ]]; then flags+=(-fsanitize=address,undefined); fi
 for version in baseline current; do
  "$cc" "${flags[@]}" -I"$fixture" "$out/$version.cpp" "$fixture/check.cpp" -o "$out/$variant-$version"
 done
 for test in valid invalid null documented; do
  for version in baseline current; do
   set +e
   ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/$variant-$version" "$test" >"$out/$variant-$version-$test.log" 2>&1
   rc=$?
   set -e
   printf '%s %s %s %s\n' "$variant" "$version" "$test" "$rc" >> "$out/results.txt"
   case "$version:$test" in
    baseline:valid|current:valid|current:invalid|current:null) test "$rc" -eq 0;;
    baseline:invalid|baseline:null|baseline:documented|current:documented) test "$rc" -ne 0;;
   esac
  done
 done
done
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
cat "$out/results.txt"
