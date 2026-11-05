#!/bin/bash
# G8 targeted verification: binaries that compile the G7->G8 changed files.
set -u
S=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
D=$S/g8; SYM=/opt/homebrew/opt/llvm/bin/llvm-symbolizer
ONLY=CoverageTests,CoverageTests_porter,CoverageTests_dtservo
log() { echo "$(date +%H:%M:%S) $*" | tee -a "$D/pipeline.log"; }
log "g8 normal run start"; "$S/run_full.sh" "$D/build" "$D/run" --workers 8 --only $ONLY > "$D/run.console" 2>&1; log "g8 normal run exit=$? $(tail -1 $D/run.console)"
log "g8 asan configure"
cmake -S "$D/tree/src" -B "$D/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include -DWT_WARNINGS_AS_ERRORS=OFF -DCMAKE_PROJECT_INCLUDE=$S/llvm-implicit-usrlocal.cmake \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer -isystem /usr/local/include" \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer -include cstdlib -isystem /usr/local/include" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" \
  "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" \
  "-DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" > "$D/configure-asan.log" 2>&1
log "g8 asan configure exit=$?"
cmake --build "$D/build-asan" -j 8 -- -k 0 > "$D/build-asan.log" 2>&1; log "g8 asan build exit=$? FAILED=$(grep -c FAILED: $D/build-asan.log)"
log "g8 asan run start"
"$S/run_full.sh" "$D/build-asan" "$D/run-asan" --workers 6 --only $ONLY --env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0:symbolize=1 \
  --env UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 --env ASAN_SYMBOLIZER_PATH=$SYM > "$D/run-asan.console" 2>&1
log "g8 asan run exit=$? $(tail -1 $D/run-asan.console)"
log "g8 tsan configure"; "$S/san-configure.sh" "$D/tree" "$D/build-tsan" tsan > "$D/configure-tsan.log" 2>&1; log "g8 tsan configure exit=$?"
cmake --build "$D/build-tsan" --target CoverageTests_dtservo WtDataStorage -j 8 > "$D/build-tsan.log" 2>&1; log "g8 tsan build exit=$?"
"$S/run_full.sh" "$D/build-tsan" "$D/run-tsan" --workers 6 --only CoverageTests_dtservo --env TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:report_signal_unsafe=0 \
  --env TSAN_SYMBOLIZER_PATH=$SYM > "$D/run-tsan.console" 2>&1
log "g8 tsan run exit=$? $(tail -1 $D/run-tsan.console)"
