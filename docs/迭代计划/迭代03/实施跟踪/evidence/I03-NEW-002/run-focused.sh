#!/bin/zsh
set -u
binary=$1
ulimit -c 0
kind=$2
shift 2
: > /private/tmp/wt-i03-new002/${kind}-results.log
for name in "$@"; do
 dir=/private/tmp/wt-i03-new002/run/${kind}-${name}
 mkdir -p "$dir"
 (cd "$dir" && LLVM_PROFILE_FILE="$dir/profile.profraw" perl -e 'alarm 30; exec @ARGV' -- "$binary" --gtest_filter="R15CtxTest.$name" --gtest_output=xml:result.xml > output.log 2>&1)
 case_exit=$?
 print -r -- "$name exit=$case_exit" >> /private/tmp/wt-i03-new002/${kind}-results.log
done
