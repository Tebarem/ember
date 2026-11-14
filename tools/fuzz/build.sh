#!/bin/bash
# tools/fuzz/build.sh [-s SRC] [-b BUILD] [target...]
# Configures and builds the libFuzzer harnesses (EMBER_FUZZ=ON) with clang on Linux.
#   SRC    source tree (default: the checkout this script is in)
#   BUILD  build directory (default: SRC/../fuzz-build next to it, or $EMBER_FUZZ_BUILD)
#   target fuzz_<name> executables to build; none builds all of them (target fuzz_all)
# Environment: EMBER_SLANGC (path of slangc, default /opt/slang/bin/slangc or slangc on PATH),
# JOBS (default 8), CLANG_BIN (default /usr/lib/llvm-19/bin when it exists).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$here/../.." && pwd)
BUILD=${EMBER_FUZZ_BUILD:-}
while getopts "s:b:" o; do
  case $o in s) SRC=$(cd "$OPTARG" && pwd);; b) BUILD=$OPTARG;; *) exit 2;; esac
done
shift $((OPTIND - 1))
[ -n "$BUILD" ] || BUILD="$SRC/../fuzz-build"
CLANG_BIN=${CLANG_BIN:-/usr/lib/llvm-19/bin}
[ -d "$CLANG_BIN" ] && export PATH="$CLANG_BIN:$PATH"
SLANGC=${EMBER_SLANGC:-}
[ -n "$SLANGC" ] || { [ -x /opt/slang/bin/slangc ] && SLANGC=/opt/slang/bin/slangc || SLANGC=$(command -v slangc); }
if [ ! -f "$BUILD/build.ninja" ]; then
  CC=clang cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMBER_FUZZ=ON \
     -DEMBER_TESTS=OFF -DEMBER_EXAMPLES=OFF -DEMBER_SLANGC="$SLANGC" -DCMAKE_C_FLAGS="-O1 -g"
fi
if [ $# -eq 0 ]; then set -- fuzz_all; fi
for t in "$@"; do args+=(--target "$t"); done
cmake --build "$BUILD" -j "${JOBS:-8}" "${args[@]}"
