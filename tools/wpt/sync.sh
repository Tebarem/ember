#!/usr/bin/env bash
# Vendor selected web-platform-tests directories into tests/wpt/ at a pinned commit.
# Usage: tools/wpt/sync.sh   (edit WPT_COMMIT / DIRS, then rerun and commit tests/wpt/)
set -euo pipefail
WPT_COMMIT="${WPT_COMMIT:-$(git ls-remote https://github.com/web-platform-tests/wpt refs/heads/master | cut -f1)}"
DIRS=(html/syntax/parsing)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
git -C "$TMP" init -q
git -C "$TMP" remote add origin https://github.com/web-platform-tests/wpt
git -C "$TMP" config core.sparseCheckout true
{ printf '%s/\n' "${DIRS[@]}"; echo /LICENSE.md; } > "$TMP/.git/info/sparse-checkout"
git -C "$TMP" fetch -q --depth 1 --filter=blob:none origin "$WPT_COMMIT"
git -C "$TMP" checkout -q FETCH_HEAD
for d in "${DIRS[@]}"; do rm -rf "$ROOT/tests/wpt/$d"; mkdir -p "$ROOT/tests/wpt/$(dirname "$d")"; cp -r "$TMP/$d" "$ROOT/tests/wpt/$d"; done
cp "$TMP/LICENSE.md" "$ROOT/tests/wpt/LICENSE.md"
echo "$WPT_COMMIT" > "$ROOT/tests/wpt/COMMIT"
rm -rf "$TMP"
echo "synced wpt @ $WPT_COMMIT: ${DIRS[*]}"
