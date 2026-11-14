#!/bin/bash
# tools/fuzz/run.sh <target> <seconds> [libFuzzer args...]
# Runs one libFuzzer harness (tests/fuzz/fuzz_<target>.c) for <seconds> with -jobs/-workers. The grown
# corpus stays on the machine in $FUZZ_ROOT/<target>/corpus (default /opt/ember/fuzz), crashes,
# hangs and OOMs land in $FUZZ_ROOT/<target>/crashes and a one-line summary is appended to
# $FUZZ_ROOT/<target>/summary.txt. The committed seed corpus (tests/fuzz/corpus/<target>) is read
# as a second corpus directory, and the dictionary (tests/fuzz/dict/<target>.dict) is used when it exists.
#
# Environment:
#   FUZZ_BUILD    build directory made by tools/fuzz/build.sh (default $SRC/../fuzz-build)
#   FUZZ_ROOT     where corpora and artifacts live (default /opt/ember/fuzz)
#   WORKERS       parallel workers (default 4; keep the total over all running targets at 16 or less)
#   MAX_LEN       override -max_len
# Without <seconds> it prints the usage. Extra arguments go straight to libFuzzer, for example
# -runs=0 to only replay the corpus, or a file name to replay a single reproducer.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$here/../.." && pwd)
if [ $# -lt 2 ]; then sed -n 2,17p "$0"; exit 2; fi
T=${1#fuzz_}; SECS=$2; shift 2
BUILD=${FUZZ_BUILD:-$SRC/../fuzz-build}
ROOT=${FUZZ_ROOT:-/opt/ember/fuzz}
EXE=$BUILD/tests/fuzz/fuzz_$T
[ -x "$EXE" ] || { echo "no $EXE (build it with tools/fuzz/build.sh fuzz_$T)"; exit 1; }
WORKERS=${WORKERS:-4}

# -max_len per target: large enough for the structured formats, small where bigger inputs only slow
# the run down.
case $T in
  html_doc|html_fragment) ML=16384;;
  css_sheet|css_selector) ML=8192;;
  css_value) ML=1024;;
  js_parse) ML=8192;;
  js_exec) ML=2048;;
  regexp|json_num) ML=1024;;
  url|mime_headers|encoding|multipart) ML=2048;;
  clone|importmap) ML=4096;;
  inflate|brotli) ML=16384;;
  woff|font) ML=1048576;;
  image) ML=65536;;
  *) ML=8192;;
esac
ML=${MAX_LEN:-$ML}

D=$ROOT/$T
mkdir -p "$D/corpus" "$D/crashes" "$D/work"
SEEDS=$SRC/tests/fuzz/corpus/$T
DICT=$SRC/tests/fuzz/dict/$T.dict
ARGS=()
[ -f "$DICT" ] && ARGS+=("-dict=$DICT")
CORPORA=("$D/corpus")
[ -d "$SEEDS" ] && CORPORA+=("$SEEDS")
# Raw fixtures that the harness reads as they are (the options of these harnesses are in the last byte).
case $T in
  font|woff) CORPORA+=("$SRC/tests/fonts" "$SRC/tests/wpt/fonts");;
  image) CORPORA+=("$SRC/tests/image");;
  brotli) CORPORA+=("$SRC/tests/data/brotli");;
  inflate) [ -d "$SRC/tests/data/deflate" ] && CORPORA+=("$SRC/tests/data/deflate");;
esac
before=$(ls "$D/crashes" | wc -l)

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_stack_use_after_return=0:detect_leaks=1:abort_on_error=0:handle_abort=1:symbolize=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
[ -x /usr/lib/llvm-19/bin/llvm-symbolizer ] && export ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-19/bin/llvm-symbolizer

cd "$D/work"
rm -f fuzz-*.log
set +e
"$EXE" -jobs="$WORKERS" -workers="$WORKERS" -max_total_time="$SECS" -max_len="$ML" -timeout=10 \
  -rss_limit_mb=3072 -print_final_stats=1 -artifact_prefix="$D/crashes/" "${ARGS[@]}" "$@" \
  "${CORPORA[@]}" > run.out 2>&1
rc=$?
set -e

after=$(ls "$D/crashes" | wc -l)
sum() { # sum a "stat::name: N" over all worker logs
  grep -h "^stat::$1:" fuzz-*.log 2>/dev/null | awk '{s += $2} END {printf "%d", s}'
}
execs=$(sum number_of_executed_units)
eps=$(sum average_exec_per_sec)
cov=$(grep -h "cov: " fuzz-*.log 2>/dev/null | sed -n 's/.*cov: \([0-9]*\).*/\1/p' | sort -n | tail -1)
ft=$(grep -h "ft: " fuzz-*.log 2>/dev/null | sed -n 's/.*ft: \([0-9]*\).*/\1/p' | sort -n | tail -1)
corp=$(ls "$D/corpus" | wc -l)
line="$(date -u +%FT%TZ) target=$T secs=$SECS workers=$WORKERS exec/s=$eps runs=$execs cov=${cov:-0} ft=${ft:-0} corpus=$corp new_artifacts=$((after - before)) rc=$rc"
echo "$line" | tee -a "$D/summary.txt"
if [ "$after" -gt "$before" ]; then
  echo "findings in $D/crashes:"; ls -t "$D/crashes" | head -$((after - before))
fi
