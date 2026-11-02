#!/bin/bash
set -u
A=/tmp/wt-i03-r41-drain
for mode in early idle idle-stop guard pending throw; do
  mkdir -p "$A/audit-$mode"
  (cd "$A/audit-$mode" && LLVM_PROFILE_FILE="$A/audit-$mode/profile-%p.profraw" perl -e 'alarm 10; exec @ARGV' -- "$A/audit-after" "$mode") > "$A/audit-$mode.log" 2>&1
  printf '%s exit=%s\n' "$mode" "$?"
done
