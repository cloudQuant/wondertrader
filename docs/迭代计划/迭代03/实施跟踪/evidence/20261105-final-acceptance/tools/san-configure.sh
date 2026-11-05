#!/bin/bash
# Usage: san-configure.sh <tree-root> <build-dir> asan|tsan
set -e
T="$1"; B="$2"; K="$3"
S=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
if [ "$K" = asan ]; then F="-fsanitize=address,undefined -fno-sanitize-recover=undefined"; L="-fsanitize=address,undefined"; else F="-fsanitize=thread"; L="-fsanitize=thread"; fi
cmake -S "$T/src" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include -DWT_WARNINGS_AS_ERRORS=OFF \
  -DCMAKE_PROJECT_INCLUDE=$S/llvm-implicit-usrlocal.cmake \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  "-DCMAKE_C_FLAGS=$F -fno-omit-frame-pointer -isystem /usr/local/include" \
  "-DCMAKE_CXX_FLAGS=$F -fno-omit-frame-pointer -include cstdlib -isystem /usr/local/include" \
  "-DCMAKE_EXE_LINKER_FLAGS=$L --ld-path=/usr/bin/ld" \
  "-DCMAKE_SHARED_LINKER_FLAGS=$L --ld-path=/usr/bin/ld" \
  "-DCMAKE_MODULE_LINKER_FLAGS=$L --ld-path=/usr/bin/ld"
