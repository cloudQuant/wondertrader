#!/bin/zsh
set -u
binary=$1
kind=$2
names=(order_tag_default_capacity_preserves_registered_tag order_tag_set_get_erase_with_default_context order_tag_lookup_miss_does_not_match_or_erase_neighbor order_tags_keep_unordered_ids_and_replace_exact_duplicate order_tags_do_not_silently_evict_or_truncate registered_order_tag_reaches_callbacks_and_remains_after_terminal_order order_tags_survive_open_close_and_reverse_without_truncation)
: > /private/tmp/wt-i03-r15-tags/${kind}-results.log
for name in $names; do
 dir=/private/tmp/wt-i03-r15-tags/run/${kind}-${name}
 mkdir -p "$dir"
 (cd "$dir" && LLVM_PROFILE_FILE="$dir/profile.profraw" perl -e 'alarm 30; exec @ARGV' -- "$binary" --gtest_filter="R15CtxTest.$name" --gtest_output=xml:result.xml > output.log 2>&1)
 case_exit=$?
 print -r -- "$name exit=$case_exit" >> /private/tmp/wt-i03-r15-tags/${kind}-results.log
done
