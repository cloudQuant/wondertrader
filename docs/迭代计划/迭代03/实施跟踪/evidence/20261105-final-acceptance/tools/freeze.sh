#!/bin/bash
# Usage: freeze.sh <dest-dir>
# Copies src/ and dist/ (no symlinks) into <dest>/tree and records meta in <dest>/meta.
set -euo pipefail
R=/Users/yunjinqi/Documents/new_projects/wondertrader
D="$1"
mkdir -p "$D/tree" "$D/meta"
date +%Y-%m-%dT%H:%M:%S%z > "$D/meta/frozen_at.txt"
git -C "$R" rev-parse HEAD > "$D/meta/HEAD.txt"
git -C "$R" status --porcelain > "$D/meta/git-status.txt"
git -C "$R" diff --binary -- src dist | gzip -9 > "$D/meta/tracked.diff.gz"
# copy (no links); exclude nothing inside src/dist
rsync -a --no-links "$R/src" "$R/dist" "$D/tree/"
# verify no symlinks
if find "$D/tree" -type l | grep -q .; then echo "symlink found in snapshot" >&2; exit 3; fi
# verify identical to workspace right after copy
if ! diff -rq --no-dereference "$R/src" "$D/tree/src" > "$D/meta/post-copy-diff.txt" 2>&1; then echo "workspace changed during copy (src)" >&2; fi
if ! diff -rq --no-dereference "$R/dist" "$D/tree/dist" >> "$D/meta/post-copy-diff.txt" 2>&1; then echo "workspace changed during copy (dist)" >&2; fi
(cd "$D/tree" && find src dist -type f -print0 | sort -z | xargs -0 shasum -a 256) > "$D/meta/tree.sha256"
shasum -a 256 "$D/meta/tree.sha256" | awk '{print $1}' > "$D/meta/tree.sha256.digest"
wc -l < "$D/meta/tree.sha256" | tr -d ' ' > "$D/meta/file-count.txt"
echo "frozen $(cat $D/meta/frozen_at.txt) files=$(cat $D/meta/file-count.txt) digest=$(cat $D/meta/tree.sha256.digest)"
