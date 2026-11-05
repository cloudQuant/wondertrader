#!/bin/bash
set -u
S=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad; K=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/k49; SYM=/opt/homebrew/opt/llvm/bin/llvm-symbolizer
T=CoverageTests_t_TraderCTPMini
cmake -S $K/tree/src -B $K/build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include -DWT_WARNINGS_AS_ERRORS=OFF -DCMAKE_PROJECT_INCLUDE=$S/llvm-implicit-usrlocal.cmake \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer -isystem /usr/local/include" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer -include cstdlib -isystem /usr/local/include" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" \
  "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" \
  "-DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" > $K/logs/configure-asan.log 2>&1; echo "asan-configure=$?"
cmake --build $K/build-asan --target $T -j 6 > $K/logs/build-asan.log 2>&1; echo "asan-build=$?"
$S/san-configure.sh $K/tree $K/build-tsan tsan > $K/logs/configure-tsan.log 2>&1; echo "tsan-configure=$?"
cmake --build $K/build-tsan --target $T -j 6 > $K/logs/build-tsan.log 2>&1; echo "tsan-build=$?"
mkdir -p $K/run-asan $K/run-tsan
(cd $K/run-asan && ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0:symbolize=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ASAN_SYMBOLIZER_PATH=$SYM LLVM_PROFILE_FILE=$K/run-asan/%p.profraw $K/build-asan/build_x64/Debug/bin/TestUnits/$T --gtest_output=xml:$K/run-asan/all.xml > all.log 2>&1; echo "asan-run=$?")
(cd $K/run-tsan && TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:report_signal_unsafe=0 TSAN_SYMBOLIZER_PATH=$SYM LLVM_PROFILE_FILE=$K/run-tsan/%p.profraw $K/build-tsan/build_x64/Debug/bin/TestUnits/$T --gtest_output=xml:$K/run-tsan/all.xml > all.log 2>&1; echo "tsan-run=$?")
