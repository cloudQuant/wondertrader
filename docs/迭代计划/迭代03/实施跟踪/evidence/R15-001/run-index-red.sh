#!/bin/zsh
set -u
: > /private/tmp/wt-i03-r15-tags/red-index-results.log
for name in order_tag_lookup_miss_does_not_match_or_erase_neighbor order_tags_keep_unordered_ids_and_replace_exact_duplicate; do
 dir=/private/tmp/wt-i03-r15-tags/run/red-index-${name}
 mkdir -p "$dir"
 (cd "$dir" && LLVM_PROFILE_FILE="$dir/profile.profraw" perl -e 'alarm 30; exec @ARGV' -- /private/tmp/wt-i03-r15-tags/red-index-tests --gtest_filter="R15CtxTest.$name" --gtest_output=xml:result.xml > output.log 2>&1)
 case_exit=$?
 print -r -- "$name exit=$case_exit" >> /private/tmp/wt-i03-r15-tags/red-index-results.log
done
