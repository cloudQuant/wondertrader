#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?provide a configured build containing WTSTools and WTSUtils static libraries}
out=$(mktemp -d /tmp/wt-l04-import.XXXXXX)
cd "$root"
printf 'outputs: %s\n' "$out"
compiler=/usr/bin/c++
extra_flags=(-O0)
if [[ ${L04_SANITIZE:-0} == 1 ]]; then
  compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
  extra_flags=(-include stdlib.h -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
  export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
  export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1}
fi
"$compiler" -std=gnu++17 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL "${extra_flags[@]}" \
  -include src/Share/fmt_enum_compat.h -I/opt/homebrew/Cellar/boost/1.88.0/include \
  -I/opt/homebrew/include -Isrc -g -fprofile-instr-generate \
  src/TestUnits/fixtures/l04_contract_import/roundtrip.cpp \
  src/TestUnits/coverage/l03_includes/l03_contract_info_test.cpp \
  src/TestUnits/coverage/r1_storage/r1_basedatamgr_test.cpp \
  src/WTSTools/WTSBaseDataMgr.cpp \
  "$build/build_x64/Debug/libs/libWTSTools.a" \
  "$build/build_x64/Debug/libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread \
  -lpthread -ldl /usr/local/lib/libiconv.dylib -o "$out/tests"
LLVM_PROFILE_FILE="$out/profile-%p.profraw" perl -e 'alarm 60; exec @ARGV' -- "$out/tests" \
  '--gtest_filter=L04ContractImport.*:l03_contract.storage_width_contract_limits_preserve_valid_edges_and_reject_before_rules_registration:l03_contract.identity_fields_reject_values_that_do_not_fit:R1BaseDataMgrConfigBoundary.contract_identity_fields_enforce_caps_and_retain_values'
