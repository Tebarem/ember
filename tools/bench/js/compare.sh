#!/bin/sh
# compare.sh: Ember against QuickJS-ng v0.18.0 on the shared JS workloads.
# Both engines run the same concatenated script per workload: the dom stub and the
# framework bundle when the workload needs them, the workload, then driver.js. Ember runs it
# through bench_js_suite; QuickJS-ng runs it as qjs FILE.
#
# Usage: sh tools/bench/js/compare.sh [--interleave] BUILD_DIR [WORKLOAD...]
#   BUILD_DIR   a Release build with tools/bench/bench_js_suite (for example build/rel)
#   WORKLOAD    names from tools/bench/js (default: every workload except empty)
#   --interleave  the shared-machine protocol: the
#               two engines alternate in one session (E Q E Q ..., IL_RUNS runs each, pinned to the
#               same CPU), and the ratio is taken inside that session, so load hits both alike. The
#               metric per engine is the minimum of its per-process median_ms, with the median of
#               those for reference. The memory runs are the same as in the sequential mode.
#               Writes BUILD_DIR/bench-js-interleave.tsv in addition to bench-js.tsv. A change counts
#               only when it holds across two builds (for example one with -falign-functions=64).
# Environment:
#   SRC         source root (default: the checkout that holds this script)
#   QJS         QuickJS-ng binary (default /opt/quickjs-ng/bin/qjs)
#   QJS_SRC     QuickJS-ng source tree, read for its commit (default /opt/quickjs-ng-src)
#   EMBER_REV   Ember revision recorded in the report (default: git, else "unknown")
#   CPU         CPU to pin both engines to with taskset (default 2)
#   RUNS        timed process runs per workload and engine (default 7; sequential mode)
#   IL_RUNS     timed process runs per workload and engine in --interleave mode (default 9)
#   MEM_RUNS    process runs for peak memory per workload and engine (default 5)
#   BASE_HOST   a bench_js_suite of the J5m3 engine (the baseline of J5.md section 0's hard memory rule): its
#               peak RSS is measured the same way as Ember's, and the table reports Ember's peak delta over
#               it (the rule: a framework workload may not grow by more than 10%). Optional.
# Live memory (J5n1): the bytes still allocated after a full collection
# at the end of the run, with the workload's result kept in BENCH_RESULT. The run is 50 steps, the
# same as the peak run, then driver.js collects. QuickJS-ng runs as `qjs --std -d -C FILE`: the
# `-d` dump is printed to stdout at exit, before the runtime is freed, and its `memory used` line
# gives the live bytes in its SIZE column (fields: `memory used COUNT SIZE (...)`, field 4). The
# collection is std.gc(). Ember runs bench_js_suite on the same file: its gc() global asks for a full
# collection, which the host runs when the script has returned (J5n2: the conservative stack scan
# otherwise sees stale words in the interpreter's long-lived C frame, which pinned a replaced DOM tree
# of 1.4 MB in one build of preact_diff), and the host prints `live: BYTES` to stderr (cells plus external bytes after that
# collection). The live delta is each engine's live bytes minus its own live bytes for the empty
# workload, the median of MEM_RUNS runs. The ratio table gives the live gate (Ember's live delta at
# most QuickJS-ng's) and the peak gate for framework workloads (Ember's peak delta at most 1.5x
# QuickJS-ng's, or QuickJS-ng's delta plus 1 MB when that is under 1 MB).
# Output: BUILD_DIR/bench-js.tsv (workload, engine, median ms, spread, min ms, checksum, raw max RSS
# in KiB, RSS delta over the empty workload in KiB, live KiB, live delta KiB), per-run logs under
# BUILD_DIR/js/logs, and the ratio table on stdout. Exit status: 0 on success, 1 when a run fails or the two engines disagree
# on a checksum, 2 on a usage or setup error (wrong QuickJS-ng version, missing host).
# Timing is only meaningful on an idle machine, or with --interleave on a busy one. The harness does not
# check for other load.

set -u

here=$(cd "$(dirname "$0")" && pwd)
SRC=${SRC:-$(cd "$here/../../.." && pwd)}
QJS=${QJS:-/opt/quickjs-ng/bin/qjs}
QJS_SRC=${QJS_SRC:-/opt/quickjs-ng-src}
CPU=${CPU:-2}
RUNS=${RUNS:-7}
IL_RUNS=${IL_RUNS:-9}
MEM_RUNS=${MEM_RUNS:-5}
QJS_VERSION_WANT="QuickJS-ng version 0.18.0"
QJS_COMMIT_WANT=d7215b0ad3fc2c562e8317fd9bca058fd4863804
ALL_WORKLOADS="preact_render preact_diff vue_reactive vue_render json string_build props array_methods closures classes regexp"

INTERLEAVE=0
if [ "${1:-}" = "--interleave" ]; then
    INTERLEAVE=1
    shift
fi
if [ $# -lt 1 ]; then
    echo "usage: $0 [--interleave] BUILD_DIR [WORKLOAD...]" >&2
    exit 2
fi
BUILD=$1
shift
HOST=$BUILD/tools/bench/bench_js_suite
if [ ! -x "$HOST" ]; then
    echo "compare.sh: missing $HOST (build the bench_js_suite target)" >&2
    exit 2
fi
if [ $# -gt 0 ]; then
    WORKLOADS="$*"
else
    WORKLOADS="$ALL_WORKLOADS"
fi

# The harness refuses to run against any other QuickJS-ng. The commit comes from the source tree
# when it is present, and otherwise from the pinned value in the spec.
qjs_line=$("$QJS" --help 2>&1 | head -n 1)
case $qjs_line in
    "$QJS_VERSION_WANT"*) ;;
    *)
        echo "compare.sh: refusing: $QJS reports '$qjs_line', want '$QJS_VERSION_WANT'" >&2
        exit 2
        ;;
esac
if [ -d "$QJS_SRC/.git" ] || [ -f "$QJS_SRC/.git" ]; then
    qjs_commit=$(git -C "$QJS_SRC" rev-parse HEAD 2>/dev/null)
    if [ "$qjs_commit" != "$QJS_COMMIT_WANT" ]; then
        echo "compare.sh: refusing: $QJS_SRC is at '$qjs_commit', want $QJS_COMMIT_WANT" >&2
        exit 2
    fi
else
    qjs_commit=$QJS_COMMIT_WANT
fi
EMBER_REV=${EMBER_REV:-$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)}

OUT=$BUILD/js
LOGS=$OUT/logs
mkdir -p "$LOGS"
FW=$SRC/tests/js/frameworks
TSV=$BUILD/bench-js.tsv
ILTSV=$BUILD/bench-js-interleave.tsv
failed=0

# Median of the numbers on stdin (odd or even count; prints nan for none).
median() {
    sort -g | awk '{ a[NR] = $1 } END {
        if (NR == 0) { print "nan"; exit }
        if (NR % 2) print a[(NR + 1) / 2]; else print (a[NR / 2] + a[NR / 2 + 1]) / 2 }'
}

# Writes OUT/<name>.bench.js: the name, the frameworks the workload uses, the workload, the driver.
# Also OUT/<name>.mem.js (peak runs) and OUT/<name>.live.js (live-memory runs, J5n1).
build_script() {
    name=$1
    if [ ! -f "$here/$name.js" ]; then
        echo "compare.sh: no workload $here/$name.js" >&2
        exit 2
    fi
    {
        printf "var BENCH_NAME = '%s';\n" "$name"
        case $name in
            preact_*) cat "$FW/dom_stub.js" "$FW/preact-10.29.8.umd.js" ;;
            vue_*) cat "$FW/vue-reactivity-3.5.45.global.js" ;;
        esac
        cat "$here/$name.js" "$here/driver.js"
    } > "$OUT/$name.bench.js"
    # Memory mode (BENCH_MEM, a fixed step count) for the peak RSS runs, so the heap peak does not
    # depend on timing.
    {
        printf "var BENCH_MEM = true;\n"
        cat "$OUT/$name.bench.js"
    } > "$OUT/$name.mem.js"
    {
        printf "var BENCH_LIVE = true;\n"
        cat "$OUT/$name.bench.js"
    } > "$OUT/$name.live.js"
}

# Runs one engine on one script and prints its stdout. Stderr is appended to the log. QuickJS-ng
# runs in classic script mode (-C): its autodetection loads some files as modules, where top-level
# this is undefined and var does not reach the global object.
run_engine() {
    engine=$1
    file=$2
    errlog=$3
    case $engine in
        quickjs-ng) taskset -c "$CPU" "$QJS" -C "$file" 2>>"$errlog" ;;
        ember) taskset -c "$CPU" "$HOST" "$file" 2>>"$errlog" ;;
    esac
}

# Runs one engine on one script, pinned to CPU, and prints its peak RSS in KiB. The peak comes from
# rss_wrap.c (J5n2), a small launcher built on first use: ru_maxrss of a child includes the resident
# set its parent had at fork, so the python3 launcher used before (about 11 MB) floored every figure at
# 11 MB and hid every smaller peak. With no C compiler on the PATH it falls back to /usr/bin/time and
# then to python3, and says so, because those figures carry that floor. Stderr is appended to the log.
RSSWRAP=
build_rss_wrap() {
    for cc in ${CC:-} cc clang-19 clang gcc; do
        [ -n "$cc" ] || continue
        command -v "$cc" > /dev/null 2>&1 || continue
        if "$cc" -O1 -o "$OUT/rss_wrap" "$here/rss_wrap.c" 2> "$OUT/rss_wrap.build.log"; then
            RSSWRAP=$OUT/rss_wrap
            return 0
        fi
    done
    echo "compare.sh: no C compiler for rss_wrap.c; peak RSS figures below 11 MB are floored by the launcher" >&2
    return 1
}

max_rss() {
    errlog=$1
    engine=$2
    file=$3
    case $engine in
        quickjs-ng) set -- "$QJS" -C "$file" ;;
        ember) set -- "$HOST" "$file" ;;
        base) set -- "$BASE_HOST" "$file" ;;
    esac
    if [ -n "$RSSWRAP" ]; then
        tf=$LOGS/rss.$$.out
        "$RSSWRAP" "$tf" taskset -c "$CPU" "$@" 2>>"$errlog" || return 1
        tail -n 1 "$tf"
    elif [ -x /usr/bin/time ]; then
        tf=$LOGS/time.$$.out
        /usr/bin/time -f '%M' -o "$tf" taskset -c "$CPU" "$@" >/dev/null 2>>"$errlog" || return 1
        tail -n 1 "$tf"
    else
        python3 -c 'import resource, subprocess, sys
r = subprocess.call(sys.argv[1:], stdout=subprocess.DEVNULL)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
sys.exit(r)' taskset -c "$CPU" "$@" 2>>"$errlog" || return 1
    fi
}

# Live bytes (KiB, median of MEM_RUNS fresh processes) of one workload on one engine after the full
# collection of the live run (J5n1). The byte count is parsed as described in the header.
live_memory() {
    engine=$1
    name=$2
    file=$OUT/$name.live.js
    lres=$LOGS/$name.$engine.live
    errlog=$LOGS/$name.$engine.err
    : > "$lres"
    j=0
    while [ "$j" -lt "$MEM_RUNS" ]; do
        j=$((j + 1))
        case $engine in
            quickjs-ng)
                out=$(taskset -c "$CPU" "$QJS" --std -d -C "$file" 2>>"$errlog") || {
                    echo "compare.sh: $engine $name live run $j failed (see $errlog)" >&2
                    return 1
                }
                bytes=$(printf '%s\n' "$out" | awk '$1 == "memory" && $2 == "used" { print $4; exit }')
                ;;
            ember)
                errout=$(taskset -c "$CPU" "$HOST" "$file" 2>&1 >/dev/null) || {
                    printf '%s\n' "$errout" >> "$errlog"
                    echo "compare.sh: $engine $name live run $j failed (see $errlog)" >&2
                    return 1
                }
                printf '%s\n' "$errout" >> "$errlog"
                bytes=$(printf '%s\n' "$errout" | awk '$1 == "live:" { print $2; exit }')
                ;;
        esac
        case $bytes in
            ''|*[!0-9]*)
                echo "compare.sh: $engine $name printed no live figure (see $errlog)" >&2
                return 1
                ;;
        esac
        echo $(( (bytes + 1023) / 1024 )) >> "$lres"
    done
    median < "$lres"
}

# Peak RSS (KiB, median of MEM_RUNS fresh processes) of one workload on one engine.
memory() {
    engine=$1
    name=$2
    file=$OUT/$name.mem.js
    rres=$LOGS/$name.$engine.rss
    errlog=$LOGS/$name.$engine.err
    : > "$rres"
    j=0
    while [ "$j" -lt "$MEM_RUNS" ]; do
        j=$((j + 1))
        if ! kib=$(max_rss "$errlog" "$engine" "$file"); then
            echo "compare.sh: $engine $name memory run $j failed (see $errlog)" >&2
            return 1
        fi
        echo "$kib" >> "$rres"
    done
    median < "$rres"
}

# One timed process run of one engine on one workload. Appends "median_ms min_ms checksum" to the
# engine's times file. Fails when the run fails or prints no result line.
timed_run() {
    engine=$1
    name=$2
    file=$OUT/$name.bench.js
    errlog=$LOGS/$name.$engine.err
    res=$LOGS/$name.$engine.times
    if ! out=$(run_engine "$engine" "$file" "$errlog"); then
        echo "compare.sh: $engine $name run failed (see $errlog)" >&2
        return 1
    fi
    line=$(printf '%s\n' "$out" | grep "^$name " | tail -n 1)
    if [ -z "$line" ]; then
        echo "compare.sh: $engine $name printed no result line (see $errlog)" >&2
        return 1
    fi
    printf '%s\n' "$line" | awk '{ print $2, $3, $4 }' >> "$res"
}

# Reduces the times file of one engine to OUT/logs/<name>.<engine>.summary: the median of the
# per-run medians, the spread of those medians, the lowest minimum, the checksum, and the median
# peak RSS. The checksum must be the same in every run.
summarize() {
    engine=$1
    name=$2
    res=$LOGS/$name.$engine.times
    summary=$LOGS/$name.$engine.summary
    med=$(awk '{ print $1 }' "$res" | median)
    smin=$(awk '{ print $1 }' "$res" | sort -g | head -n 1)
    smax=$(awk '{ print $1 }' "$res" | sort -g | tail -n 1)
    mn=$(awk '{ print $2 }' "$res" | sort -g | head -n 1)
    nck=$(awk '{ print $3 }' "$res" | sort -u | wc -l)
    if [ "$nck" -ne 1 ]; then
        echo "compare.sh: $engine $name checksum changed between runs" >&2
        return 1
    fi
    ck=$(awk '{ print $3 }' "$res" | sort -u)
    rss=$(memory "$engine" "$name") || return 1
    live=$(live_memory "$engine" "$name") || return 1
    echo "$med $smin $smax $mn $ck $rss $live" > "$summary"
}

# Sequential timed runs of one workload on one engine (the default mode).
measure() {
    engine=$1
    name=$2
    : > "$LOGS/$name.$engine.times"
    : > "$LOGS/$name.$engine.err"
    i=0
    while [ "$i" -lt "$RUNS" ]; do
        i=$((i + 1))
        timed_run "$engine" "$name" || return 1
    done
    summarize "$engine" "$name"
}

# --interleave: the two engines alternate in one session, Ember first in each pair (E Q E Q ...),
# IL_RUNS pairs. Each engine's summary comes from its own runs, and the ratios are formed from
# those runs in this session only.
measure_interleaved() {
    name=$1
    for engine in ember quickjs-ng; do
        : > "$LOGS/$name.$engine.times"
        : > "$LOGS/$name.$engine.err"
    done
    i=0
    while [ "$i" -lt "$IL_RUNS" ]; do
        i=$((i + 1))
        for engine in ember quickjs-ng; do
            timed_run "$engine" "$name" || return 1
        done
    done
    for engine in ember quickjs-ng; do
        summarize "$engine" "$name" || return 1
    done
}

build_rss_wrap || true
echo "compare.sh: QuickJS-ng 0.18.0 ($qjs_commit), Ember $EMBER_REV, CPU $CPU, $([ "$INTERLEAVE" = 1 ] && echo "interleaved $IL_RUNS pairs" || echo "$RUNS timed runs"), $MEM_RUNS memory runs"

# The empty workload gives the RSS and live baselines of each engine.
build_script empty
for engine in quickjs-ng ember; do
    if ! rss_empty=$(memory "$engine" empty); then
        failed=1
        continue
    fi
    echo "$rss_empty" > "$LOGS/empty.$engine.rss.median"
    if ! live_empty=$(live_memory "$engine" empty); then
        failed=1
        continue
    fi
    echo "$live_empty" > "$LOGS/empty.$engine.live.median"
done

for name in $WORKLOADS; do
    build_script "$name"
    if [ "$INTERLEAVE" = 1 ]; then
        if ! measure_interleaved "$name"; then
            failed=1
        fi
        continue
    fi
    for engine in quickjs-ng ember; do
        if ! measure "$engine" "$name"; then
            failed=1
        fi
    done
done

# The J5m3 baseline's peak RSS, measured with the same launcher (BASE_HOST): one "name delta" line per
# workload in LOGS/base.delta, the delta over the baseline engine's own empty run.
: > "$LOGS/base.delta"
if [ -n "${BASE_HOST:-}" ] && [ -x "$BASE_HOST" ]; then
    build_script empty
    base_empty=$(memory base empty) || base_empty=""
    if [ -n "$base_empty" ]; then
        for name in $WORKLOADS; do
            build_script "$name"
            if b=$(memory base "$name"); then
                echo "$name $((b - base_empty))" >> "$LOGS/base.delta"
            fi
        done
    fi
fi

# Report: one row per workload and engine, with the RSS and live deltas over the empty workload.
{
    echo "# compare.sh: QuickJS-ng 0.18.0 commit $qjs_commit; Ember $EMBER_REV; CPU $CPU; median of $RUNS timed runs; RSS and live are the median of $MEM_RUNS runs."
    printf 'workload\tengine\tmedian_ms\tspread_min_ms\tspread_max_ms\tmin_ms\tchecksum\trss_kib\trss_delta_kib\tlive_kib\tlive_delta_kib\n'
    for name in $WORKLOADS; do
        ck_q=""
        ck_e=""
        for engine in quickjs-ng ember; do
            s=$LOGS/$name.$engine.summary
            if [ ! -f "$s" ]; then
                continue
            fi
            read -r med smin smax mn ck rss live < "$s"
            base=$(cat "$LOGS/empty.$engine.rss.median" 2>/dev/null || echo 0)
            delta=$((rss - base))
            lbase=$(cat "$LOGS/empty.$engine.live.median" 2>/dev/null || echo 0)
            ldelta=$((live - lbase))
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$engine" "$med" "$smin" "$smax" "$mn" "$ck" "$rss" "$delta" "$live" "$ldelta"
            if [ "$engine" = quickjs-ng ]; then ck_q=$ck; else ck_e=$ck; fi
        done
        if [ -n "$ck_q" ] && [ -n "$ck_e" ] && [ "$ck_q" != "$ck_e" ]; then
            echo "compare.sh: CHECKSUM MISMATCH $name: quickjs-ng $ck_q, ember $ck_e" >&2
            failed=1
        fi
    done
} > "$TSV"

echo
awk -F'\t' -v basefile="$LOGS/base.delta" '
    BEGIN { while ((getline line < basefile) > 0) { split(line, f, " "); bd[f[1]] = f[2] } }
    /^#/ || /^workload\t/ { next }
    $2 == "quickjs-ng" { if (!($1 in seen)) { seen[$1] = 1; order[++n] = $1 }; qt[$1] = $3; qd[$1] = $9; ql[$1] = $11 }
    $2 == "ember" { et[$1] = $3; ed[$1] = $9; el[$1] = $11 }
    END {
        printf "%-14s %9s %9s %7s %9s %9s %7s %9s %9s %7s %9s %7s  %s\n", "workload", "qjs ms", "ember ms", "time x", "qjs dKiB", "ember dKiB", "peak x", "qjs live", "ember live", "live x", "J5m3 dKiB", "vs J5m3", "memory is reported, not gated (J5.md section 0); hard rule: framework peak <= 1.10x J5m3"
        for (i = 1; i <= n; i++) {
            w = order[i]
            tr = et[w] / qt[w]
            mr = (qd[w] > 0) ? sprintf("%.3f", ed[w] / qd[w]) : "n/a"
            lr = (ql[w] > 0) ? sprintf("%.3f", el[w] / ql[w]) : "n/a"
            note = ""
            if (w ~ /^(preact|vue)_/) {
                bound = (qd[w] < 1024) ? qd[w] + 1024 : 1.5 * qd[w]
                note = (ed[w] <= bound) ? "peak within 1.5x" : "peak over 1.5x"
            }
            note = note (note == "" ? "" : ", ") ((el[w] <= ql[w]) ? "live <= qjs" : "live > qjs")
            bs = "n/a"; vs = "n/a"
            if (w in bd) {
                bs = sprintf("%d", bd[w])
                if (bd[w] > 0) {
                    vs = sprintf("%.3f", ed[w] / bd[w])
                    if (w ~ /^(preact|vue)_/) { note = note ", " ((ed[w] <= 1.10 * bd[w]) ? "peak rule ok" : "PEAK RULE BROKEN") }
                }
            }
            printf "%-14s %9.4f %9.4f %7.3f %9d %9d %7s %9d %9d %7s %9s %7s  %s\n", w, qt[w], et[w], tr, qd[w], ed[w], mr, ql[w], el[w], lr, bs, vs, note
            if (w !~ /^(preact|vue)_/) { lsum += log(tr); cnt++ }
        }
        if (cnt) printf "geometric mean of time ratios, non-framework workloads: %.3f\n", exp(lsum / cnt)
    }' "$TSV"

# --interleave report: per workload, the minimum over each engine's runs of the per-process median_ms
# (column 1 of the times file) and the ratio of the minima (the headline), with the median of those
# per-process medians and their ratio for reference. column 2 (min_ms) is not used here. The memory columns are
# the same as in bench-js.tsv.
if [ "$INTERLEAVE" = 1 ]; then
    {
        echo "# compare.sh --interleave: QuickJS-ng 0.18.0 commit $qjs_commit; Ember $EMBER_REV; CPU $CPU; $IL_RUNS interleaved pairs per workload; ratios are Ember over QuickJS-ng within the session."
        printf 'workload\tember_min_ms\tqjs_min_ms\tmin_ratio\tember_median_ms\tqjs_median_ms\tmedian_ratio\tember_spread_min_ms\tember_spread_max_ms\tqjs_spread_min_ms\tqjs_spread_max_ms\tmem_ratio\tlive_delta_ratio\n'
        for name in $WORKLOADS; do
            ef=$LOGS/$name.ember.times
            qf=$LOGS/$name.quickjs-ng.times
            if [ ! -s "$ef" ] || [ ! -s "$qf" ]; then
                continue
            fi
            emin=$(awk '{ print $1 }' "$ef" | sort -g | head -n 1)
            qmin=$(awk '{ print $1 }' "$qf" | sort -g | head -n 1)
            emed=$(awk '{ print $1 }' "$ef" | median)
            qmed=$(awk '{ print $1 }' "$qf" | median)
            emax=$(awk '{ print $1 }' "$ef" | sort -g | tail -n 1)
            emn=$(awk '{ print $1 }' "$ef" | sort -g | head -n 1)
            qmax=$(awk '{ print $1 }' "$qf" | sort -g | tail -n 1)
            qmn=$(awk '{ print $1 }' "$qf" | sort -g | head -n 1)
            mem=$(awk -F'\t' -v w="$name" '$1 == w && $2 == "ember" { e = $9 } $1 == w && $2 == "quickjs-ng" { q = $9 }
                END { if (q > 0) printf "%.3f", e / q; else printf "n/a" }' "$TSV")
            live=$(awk -F'\t' -v w="$name" '$1 == w && $2 == "ember" { e = $11 } $1 == w && $2 == "quickjs-ng" { q = $11 }
                END { if (q > 0) printf "%.3f", e / q; else printf "n/a" }' "$TSV")
            awk -v w="$name" -v em="$emin" -v qm="$qmin" -v emd="$emed" -v qmd="$qmed" -v mem="$mem" -v live="$live" \
                -v ea="$emn" -v eb="$emax" -v qa="$qmn" -v qb="$qmax" 'BEGIN {
                printf "%s\t%s\t%s\t%.3f\t%s\t%s\t%.3f\t%s\t%s\t%s\t%s\t%s\t%s\n", w, em, qm, em / qm, emd, qmd, emd / qmd, ea, eb, qa, qb, mem, live }'
        done
    } > "$ILTSV"
    echo
    awk -F'\t' '
        /^#/ || /^workload\t/ { next }
        {
            printf "%-14s min: qjs %10.4f ember %10.4f ratio %6.3f | median ratio %6.3f | mem %s | live %s\n", $1, $3, $2, $4, $7, $12, $13
            if ($1 !~ /^(preact|vue)_/) { lsum += log($4); cnt++ }
        }
        END {
            if (cnt) printf "geometric mean of min ratios, non-framework workloads: %.3f\n", exp(lsum / cnt)
        }' "$ILTSV"
    echo "compare.sh: wrote $ILTSV"
fi

if [ "$failed" -ne 0 ]; then
    echo "compare.sh: FAILED (see messages above and $LOGS)" >&2
    exit 1
fi
echo "compare.sh: wrote $TSV"
