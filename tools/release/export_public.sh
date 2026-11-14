#!/bin/bash
# export_public.sh OUT_DIR [REV]
#
# Writes a new git repository at OUT_DIR that holds a single commit: the tree of REV (default HEAD) without the files
# that only serve local development (EXCLUDE below), authored by the public identity. The submodules (third_party/*)
# stay submodules at the same commits. Nothing is pushed: publish the result yourself, for example
#   git -C OUT_DIR remote add origin git@github.com:OWNER/ember.git && git -C OUT_DIR push -u origin main
#
# Environment: EMBER_PUBLIC_NAME, EMBER_PUBLIC_EMAIL (the author and committer of the public commit),
# EMBER_PUBLIC_MESSAGE (its message).
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 OUT_DIR [REV]" >&2
    exit 2
fi
OUT=$1
REV=${2:-HEAD}
NAME=${EMBER_PUBLIC_NAME:-Tebarem}
EMAIL=${EMBER_PUBLIC_EMAIL:-104076087+Tebarem@users.noreply.github.com}
MESSAGE=${EMBER_PUBLIC_MESSAGE:-"Ember preview"}

# Local development files: agent instructions and the code-graph scope.
EXCLUDE=(CLAUDE.md .graphifyignore)

TOP=$(git rev-parse --show-toplevel)
cd "$TOP"
if [ ! -f CMakeLists.txt ] || [ ! -d src/js ]; then
    echo "export_public.sh: $TOP is not an Ember checkout" >&2
    exit 2
fi
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
    echo "export_public.sh: $OUT exists and is not empty" >&2
    exit 2
fi
REV_SHA=$(git rev-parse --verify "$REV^{commit}")

# The tree: REV's index minus the excluded paths, written with a private index file.
IDX=$(mktemp)
trap 'rm -f "$IDX"' EXIT
GIT_INDEX_FILE=$IDX git read-tree "$REV_SHA"
GIT_INDEX_FILE=$IDX git rm --cached -q -r --ignore-unmatch -- "${EXCLUDE[@]}"
TREE=$(GIT_INDEX_FILE=$IDX git write-tree)

COMMIT=$(GIT_AUTHOR_NAME=$NAME GIT_AUTHOR_EMAIL=$EMAIL GIT_COMMITTER_NAME=$NAME GIT_COMMITTER_EMAIL=$EMAIL \
    git commit-tree "$TREE" -m "$MESSAGE")

# Move the commit (and only what it reaches) into the new repository through a temporary ref.
REF=refs/export/public
git update-ref "$REF" "$COMMIT"
trap 'rm -f "$IDX"; git -C "$TOP" update-ref -d "$REF" 2>/dev/null || true' EXIT
mkdir -p "$OUT"
git -C "$OUT" init -q -b main
git -C "$OUT" fetch -q "$TOP" "$REF"
git update-ref -d "$REF"
git -C "$OUT" update-ref refs/heads/main FETCH_HEAD
git -C "$OUT" reset -q --hard main
git -C "$OUT" config user.name "$NAME"
git -C "$OUT" config user.email "$EMAIL"

echo "exported ${REV_SHA:0:10} as ${COMMIT:0:10} to $OUT (author $NAME <$EMAIL>)"
echo "next: git -C \"$OUT\" submodule update --init --depth 1   (fetches SDL, SDL_image, SheenBidi, kb, test suites)"
