#!/bin/sh
# 迭代03 卡12 容器所有权定向验证：green（当前头）/ red（baseline/Includes 下 HEAD ccaad010 头）
# 用法：run.sh <新的空输出目录>
set -u
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
repo_root=$(CDPATH= cd -- "$src_root/.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/i03_container_ownership_20261103"
out=${1:?pass a new output directory}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd -P)
CXX=${CXX:-/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++}
boost=${WT_BOOST_INCLUDE:-/opt/homebrew/opt/boost/include}
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
common="-std=c++17 -g -O1 -pthread -Wall -Wextra -Werror $san"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:halt_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export LSAN_OPTIONS=report_objects=1

cd "$repo_root" || exit 2
{
	echo "HEAD=$(git rev-parse HEAD)"
	"$CXX" --version | head -1
	shasum -a 256 src/Includes/WTSCollection.hpp \
		"$fixture/baseline/Includes/WTSCollection.hpp" \
		"$fixture/check.cpp" "$fixture/run.sh" \
		src/TestUnits/coverage/r12_headers/r12_collection_test.cpp \
		src/TestUnits/coverage/r12_headers/r53_collection_variant_test.cpp
} > "$out/env-and-hashes.txt" 2>&1

build() { # name extra-flags...
	name=$1; shift
	"$CXX" $common "$@" -I"$boost" "$fixture/check.cpp" -o "$out/$name" > "$out/$name.build.log" 2>&1
	echo "build $name exit=$?" | tee -a "$out/summary.txt"
}
build check-green -I"$src_root"
build check-red -I"$fixture/baseline" -I"$src_root/Includes" -I"$src_root"

runcases() { # name
	name=$1
	: > "$out/$name.results.tsv"
	for c in $("$out/$name" list | tail -n +2); do
		perl -e 'alarm 60; exec @ARGV' "$out/$name" "$c" > "$out/$name.$c.log" 2>&1
		rc=$?
		printf '%s\t%s\n' "$c" "$rc" >> "$out/$name.results.tsv"
	done
	perl -e 'alarm 60; exec @ARGV' "$out/$name" all > "$out/$name.all.log" 2>&1
	echo "run $name all exit=$?" | tee -a "$out/summary.txt"
}
runcases check-green
runcases check-red

# 现有 R12/R53 lane 用例：以 vendored gtest 独立编译（不链接任何 core 静态库）
gt="$src_root/TestUnits/gtest"
gtsrc="$gt/gtest.cc $gt/gtest-death-test.cc $gt/gtest-filepath.cc $gt/gtest-port.cc $gt/gtest-printers.cc $gt/gtest-test-part.cc $gt/gtest-typed-test.cc"
printf '#include <gtest/gtest.h>\nint main(int argc,char**argv){::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();}\n' > "$out/gtest_main.cpp"
lane="$src_root/TestUnits/coverage/r12_headers/r12_collection_test.cpp $src_root/TestUnits/coverage/r12_headers/r53_collection_variant_test.cpp"
buildlane() { # name includes...
	name=$1; shift
	"$CXX" -std=c++17 -g -O1 -pthread $san "$@" -I"$gt" -I"$src_root/TestUnits" -I"$boost" \
		$gtsrc $lane "$out/gtest_main.cpp" -o "$out/$name" > "$out/$name.build.log" 2>&1
	echo "build $name exit=$?" | tee -a "$out/summary.txt"
}
buildlane lane-green -I"$src_root"
filter='R12Collection.map_*:R12Collection.stack_*:r53_containers.*:r53_map.*'
perl -e 'alarm 120; exec @ARGV' "$out/lane-green" --gtest_filter="$filter" --gtest_output=xml:"$out/lane-green.xml" > "$out/lane-green.log" 2>&1
echo "run lane-green exit=$?" | tee -a "$out/summary.txt"
perl -e 'alarm 120; exec @ARGV' "$out/lane-green" --gtest_output=xml:"$out/lane-green-full-files.xml" > "$out/lane-green-full-files.log" 2>&1
echo "run lane-green (all cases in both files) exit=$?" | tee -a "$out/summary.txt"
# 旧 oracle（baseline/tests 下改动前的 R12/R53 用例）对新头：展示哪些旧断言编码了被否决的语义
oldlane="$fixture/baseline/tests/r12_collection_test.cpp $fixture/baseline/tests/r53_collection_variant_test.cpp"
"$CXX" -std=c++17 -g -O1 -pthread $san -I"$src_root" -I"$gt" -I"$src_root/TestUnits" -I"$boost" \
	$gtsrc $oldlane "$out/gtest_main.cpp" -o "$out/lane-oldoracle-newheader" > "$out/lane-oldoracle-newheader.build.log" 2>&1
echo "build lane-oldoracle-newheader exit=$?" | tee -a "$out/summary.txt"
perl -e 'alarm 120; exec @ARGV' "$out/lane-oldoracle-newheader" --gtest_filter="$filter" --gtest_also_run_disabled_tests --gtest_output=xml:"$out/lane-oldoracle-newheader.xml" > "$out/lane-oldoracle-newheader.log" 2>&1
echo "run lane-oldoracle-newheader exit=$?" | tee -a "$out/summary.txt"
cat "$out/summary.txt"
