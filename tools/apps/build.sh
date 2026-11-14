#!/usr/bin/env bash
# Rebuilds the framework apps of tests/apps into their dist/ folders.
#
# Run it on a Linux build machine with Node 22 and npm, as a user that may run setpriv (it builds as user nobody):
#   [SRC=<tree>] [OUT=<work dir>] tools/apps/build.sh [app...]
# SRC defaults to the repository that holds this script. OUT (default /opt/ember/apps) is a work directory that
# the script writes: each app is copied to $OUT/<name>
# (the shared stylesheet to $OUT/shared, which the apps' vite configs alias as @shared), built
# as user nobody, and its dist/ (plus the framework's LICENSE and the lockfile) is copied back into SRC.
#
#   npm ci --ignore-scripts from package-lock.json; npm install --ignore-scripts only when an app has no
#   lockfile yet (the first time), so the committed lockfile decides every version. Then npm run build
#   (tsc where the app has a tsconfig, then vite build with base ./, target es2022, minified).
#
# The build is deterministic for the same inputs, so rerunning it reproduces the committed dist/ byte
# for byte. The script fails when a dist/ references an external URL.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${SRC:-$ROOT}
OUT=${OUT:-/opt/ember/apps}
ALL_APPS="preact-todomvc react-todomvc vue-todomvc svelte-todomvc solid-todomvc react-router vite-vanilla"
APPS=${*:-$ALL_APPS}
BUILD_HOME=/tmp/ember-home
export PATH="/usr/local/bin:/usr/bin:/bin:${PATH:-}"

as_nobody() {
    setpriv --reuid=nobody --regid=nogroup --clear-groups env HOME="$BUILD_HOME" PATH="$PATH" "$@"
}

# The framework whose LICENSE sits next to each app's dist/ (every one is MIT).
license_pkg() {
    case "$1" in
        preact-todomvc) echo preact ;;
        react-todomvc|react-router) echo react ;;
        vue-todomvc) echo vue ;;
        svelte-todomvc) echo svelte ;;
        solid-todomvc) echo solid-js ;;
        vite-vanilla) echo vite ;;
        *) echo "no license package for $1" >&2; exit 1 ;;
    esac
}

# Patterns for an external URL in a built file. Protocol-relative URLs (//host/...) count too, also inside
# CSS @import and url(). Relative and same-origin references are fine.
EXTERNAL_PATTERNS=(
    "(src|href)=[\"']?(https?:)?//"
    "url\([\"']?(https?:)?//"
    "@import[[:space:]]*(url\()?[\"']?(https?:)?//"
    "(fetch|import)\([\"\`](https?:)?//"
    "from[[:space:]]*[\"'](https?:)?//"
    "import[[:space:]]*[\"'](https?:)?//"
)

# check_external_urls DIR: fails the build when a file under DIR refers to an external URL.
# grep exits 0 when it found matches, 1 when there are none, and 2 on an error, which aborts the build
# instead of passing for a clean tree.
check_external_urls() {
    local dir=$1 hits rc=0
    hits=$(mktemp)
    local args=()
    for p in "${EXTERNAL_PATTERNS[@]}"; do
        args+=(-e "$p")
    done
    grep -rEIn --include='*.html' --include='*.js' --include='*.css' "${args[@]}" "$dir" >"$hits" 2>"$hits.err" || rc=$?
    case $rc in
        0)
            echo "external URL in $dir:" >&2
            head -n 20 "$hits" >&2
            rm -f "$hits" "$hits.err"
            exit 1
            ;;
        1)
            rm -f "$hits" "$hits.err"
            ;;
        *)
            echo "grep failed (exit $rc) while checking $dir:" >&2
            cat "$hits.err" >&2
            rm -f "$hits" "$hits.err"
            exit 2
            ;;
    esac
}

mkdir -p "$BUILD_HOME" "$OUT"
rm -rf "$OUT/shared"
mkdir -p "$OUT/shared"
cp -a "$SRC/tests/apps/shared/." "$OUT/shared/"

for app in $APPS; do
    src_app="$SRC/tests/apps/$app"
    dst="$OUT/$app"
    [ -f "$src_app/package.json" ] || { echo "no app $app under $SRC/tests/apps" >&2; exit 1; }
    echo "=== $app"
    rm -rf "$dst"
    mkdir -p "$dst"
    cp -a "$src_app/." "$dst/"
    rm -rf "$dst/dist" "$dst/node_modules"
    chown -R nobody:nogroup "$OUT"
    cd "$dst"
    if [ -f package-lock.json ]; then
        as_nobody npm ci --ignore-scripts --no-audit --no-fund
    else
        as_nobody npm install --ignore-scripts --no-audit --no-fund
    fi
    as_nobody npm run build

    # No external URLs: scripts, stylesheets, images, fetches and dynamic imports must all be relative or same-origin.
    check_external_urls "$dst/dist"

    lic_pkg=$(license_pkg "$app")
    lic=$(ls "$dst/node_modules/$lic_pkg"/LICENSE* 2>/dev/null | head -n 1)
    [ -n "$lic" ] || { echo "no LICENSE found for $lic_pkg in $app" >&2; exit 1; }
    cp "$lic" "$dst/LICENSE"

    # Copy the results back into the source tree.
    rm -rf "$src_app/dist"
    cp -a "$dst/dist" "$src_app/dist"
    cp "$dst/package-lock.json" "$src_app/package-lock.json"
    cp "$dst/LICENSE" "$src_app/LICENSE"
    echo "dist: $(find "$src_app/dist" -type f | wc -l) files, $(du -sb "$src_app/dist" | cut -f1) bytes"
done
echo "done: $APPS"
