#!/bin/bash
# Final whole-regression pipeline for iteration 03.
# Usage: final_pipeline.sh <dest-dir> [stage...]
# Stages (default all, in order): freeze build run disabled asanbuild asanrun lsanrun tsanbuild tsanrun
set -u
S=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
D="$1"; shift
STAGES="${*:-freeze build run disabled asanbuild asanrun lsanrun tsanbuild tsanrun}"
mkdir -p "$D"
log() { echo "$(date +%H:%M:%S) $*" | tee -a "$D/pipeline.log"; }
SYM=/opt/homebrew/opt/llvm/bin/llvm-symbolizer
for st in $STAGES; do
  case $st in
  freeze)
    log "freeze start"; "$S/freeze.sh" "$D" >> "$D/pipeline.log" 2>&1; log "freeze exit=$?" ;;
  build)
    log "configure normal"
    cmake -S "$D/tree/src" -B "$D/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include > "$D/configure.log" 2>&1
    c=$?; log "configure exit=$c"
    cmake --build "$D/build" -j 8 > "$D/build.log" 2>&1; b=$?
    echo "configure=$c" > "$D/build-status.txt"; echo "build=$b" >> "$D/build-status.txt"
    grep -n "warning:" "$D/build.log" > "$D/build-warnings.txt"; log "build exit=$b warnings=$(wc -l < $D/build-warnings.txt)" ;;
  run)
    log "normal run start"; "$S/run_full.sh" "$D/build" "$D/run" --workers 8 > "$D/run.console" 2>&1; log "normal run exit=$? $(tail -1 $D/run.console)" ;;
  disabled)
    log "disabled run start"; "$S/run_full.sh" "$D/build" "$D/run-disabled" --workers 8 --only-disabled --timeout 120 > "$D/run-disabled.console" 2>&1
    log "disabled run exit=$? $(tail -1 $D/run-disabled.console)" ;;
  asanbuild)
    log "asan configure"
    cmake -S "$D/tree/src" -B "$D/build-asan" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include -DWT_WARNINGS_AS_ERRORS=OFF -DCMAKE_PROJECT_INCLUDE=$S/llvm-implicit-usrlocal.cmake \
      -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
      "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer -isystem /usr/local/include" \
      "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer -include cstdlib -isystem /usr/local/include" \
      "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" \
      "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" \
      "-DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address,undefined --ld-path=/usr/bin/ld" > "$D/configure-asan.log" 2>&1
    log "asan configure exit=$?"
    cmake --build "$D/build-asan" -j 8 -- -k 0 > "$D/build-asan.log" 2>&1; log "asan build exit=$? FAILED=$(grep -c FAILED: $D/build-asan.log)" ;;
  asanrun)
    log "asan run start"
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0:symbolize=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
      ASAN_SYMBOLIZER_PATH=$SYM "$S/run_full.sh" "$D/build-asan" "$D/run-asan" --workers 6 --env ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0:symbolize=1 \
      --env UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 --env ASAN_SYMBOLIZER_PATH=$SYM > "$D/run-asan.console" 2>&1
    log "asan run exit=$? $(tail -1 $D/run-asan.console)" ;;
  lsanrun)
    log "lsan run start"
    "$S/run_full.sh" "$D/build-asan" "$D/run-lsan" --workers 6 --no-profile-env \
      --env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0:symbolize=1 --env UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
      --env ASAN_SYMBOLIZER_PATH=$SYM > "$D/run-lsan.console" 2>&1
    log "lsan run exit=$? $(tail -1 $D/run-lsan.console)" ;;
  tsanbuild)
    log "tsan configure"; "$S/san-configure.sh" "$D/tree" "$D/build-tsan" tsan > "$D/configure-tsan.log" 2>&1; log "tsan configure exit=$?"
    cmake --build "$D/build-tsan" -j 8 -- -k 0 > "$D/build-tsan.log" 2>&1; log "tsan build exit=$? FAILED=$(grep -c FAILED: $D/build-tsan.log)" ;;
  tsanrun)
    log "tsan run start"
    "$S/run_full.sh" "$D/build-tsan" "$D/run-tsan" --workers 6 --env TSAN_OPTIONS=halt_on_error=0:second_deadlock_stack=1:report_signal_unsafe=0 \
      --env TSAN_SYMBOLIZER_PATH=$SYM > "$D/run-tsan.console" 2>&1
    log "tsan run exit=$? $(tail -1 $D/run-tsan.console)" ;;
  *) log "unknown stage $st" ;;
  esac
done
log "pipeline done: $STAGES"
