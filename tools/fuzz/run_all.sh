#!/bin/bash
# tools/fuzz/run_all.sh <seconds> [target...]
# Runs tools/fuzz/run.sh for every target (or the listed ones), several at a time so that the total number
# of libFuzzer workers stays at or below MAX_WORKERS (default 16). Each target gets WORKERS workers
# (default 4), so 4 targets run at once by default.
#
#   MAX_WORKERS  total worker budget (default 16)
#   WORKERS      workers per target (default 4)
# plus everything tools/fuzz/run.sh reads (FUZZ_BUILD, FUZZ_ROOT, ...).
#
# Output: one summary line per finished target on stdout; each target's full log is in
# $FUZZ_ROOT/<target>/work/run.out.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$here/../.." && pwd)
if [ $# -lt 1 ]; then sed -n 2,13p "$0"; exit 2; fi
SECS=$1; shift
BUILD=${FUZZ_BUILD:-$SRC/../fuzz-build}
if [ $# -eq 0 ]; then
  set -- $(ls "$BUILD/tests/fuzz" | grep -E '^fuzz_[a-z0-9_]+$' | sed 's/^fuzz_//')
fi
export WORKERS=${WORKERS:-4}
slots=$(( ${MAX_WORKERS:-16} / WORKERS ))
[ "$slots" -ge 1 ] || slots=1
printf '%s\n' "$@" | xargs -P "$slots" -I{} bash "$here/run.sh" {} "$SECS"
