set -u
for mode in red green; do
  b=/tmp/wt-i03-s5-btcore-fix-$mode
  extra=""; [ $mode = red ] && extra="-DWT_SRC_ROOT=/tmp/wt-i03-s5-btcore-redsrc/src -DWT_S5_RED=ON"
  cmake -S /Users/yunjinqi/Documents/new_projects/wondertrader/src/TestUnits/fixtures/i03_btcore_s5_20261104s5 -B $b -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_LIB_DIR=/tmp/wt-i03-s5-btcore-fixlibs/red -DCMAKE_POLICY_VERSION_MINIMUM=3.5 $extra > $b.new029.configure.log 2>&1
  ninja -C $b -j 3 s5_new029 > $b.new029.build.log 2>&1; echo build-exit=$? >> $b.new029.build.log
  out=/tmp/wt-i03-s5-btcore-run/fix-$mode-new029; mkdir -p $out; : > $out/results.txt
  $b/s5_new029 --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}' > $out/cases
  while read -r c; do mkdir -p $out/work/$c; ( cd $out/work/$c && LLVM_PROFILE_FILE=$out/work/$c/%p.profraw perl -e 'alarm shift; exec @ARGV' 120 $b/s5_new029 --gtest_filter=$c --gtest_output=xml:$out/$c.xml > $out/$c.log 2>&1 ); echo "$c $?" >> $out/results.txt; done < $out/cases
done
echo new029-done
