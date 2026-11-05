#!/bin/sh
# Run against the detached src snapshot described in README.md.
set -u
root=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final5/tree/src
out=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/l00004/results
mkdir -p "$out"
: > "$out/results.txt"
failed=0
for header in \
  Share/threadpool/detail/scope_guard.hpp \
  TraderDumper/TraderAdapter.h \
  WtCore/CtaStrategyMgr.h \
  WtCore/HftStrategyMgr.h \
  WtCore/ParserAdapter.h \
  WtCore/SelStrategyMgr.h \
  WtCore/TraderAdapter.h \
  WtCore/WtExecMgr.h \
  WtCore/WtExecuterFactory.h \
  WtDtCore/ParserAdapter.h \
  WtDtServo/ParserAdapter.h \
  WtUftCore/ParserAdapter.h \
  WtUftCore/UftStrategyMgr.h
do
  name=$(printf '%s' "$header" | tr / _)
  printf '#include "%s"\n' "$header" > "$out/$name.cpp"
  clang++ -std=c++17 -Wall -Wextra -Werror \
    -Wno-deprecated-copy-with-user-provided-copy -Wno-\#pragma-messages \
    -fsyntax-only -I/opt/homebrew/Cellar/boost/1.88.0/include \
    -I/opt/homebrew/Cellar/fmt/11.2.0/include \
    -I/opt/homebrew/Cellar/spdlog/1.15.3/include \
    -I"$root" -I"$root/Includes" -I"$root/Share" \
    -I"$root/WTSTools" -I"$root/WTSUtils" \
    "$out/$name.cpp" > "$out/$name.log" 2>&1
  rc=$?
  printf '%s %s\n' "$rc" "$header" | tee -a "$out/results.txt"
  if [ "$rc" -ne 0 ]; then failed=1; fi
done
exit "$failed"
