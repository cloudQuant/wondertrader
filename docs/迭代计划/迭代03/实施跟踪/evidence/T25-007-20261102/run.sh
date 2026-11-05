#!/bin/bash
set -uo pipefail
root=/tmp/wt-i03-t25-007-20261102
status=0
for name in login_warns_and_continues_when_cache_dir_is_not_writable order_insert_rejects_when_user_tag_cache_init_failed usertag_round_trips_from_eid_cache_into_order_then_trade; do
  mkdir -p "$root/run/$name/tmp"
  cd "$root/run/$name" || exit 1
  TMPDIR="$root/run/$name/tmp" perl -e 'alarm 40; exec @ARGV' -- "$root/focused" "--gtest_filter=T25TraderCTPTest.$name" "--gtest_output=xml:$root/$name.xml" > "$root/$name.log" 2>&1
  rc=$?
  printf '%s %s\n' "$name" "$rc" >> "$root/results.txt"
  if [ "$rc" -ne 0 ]; then status=1; fi
done
exit "$status"
