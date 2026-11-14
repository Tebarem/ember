#!/bin/sh
# Library size report. POSIX sh + awk + binutils (readelf, nm, strip). Linux/ELF only.
#
#   tools/libsize.sh total <libember.so>      stripped size, section sizes, dynamic symbol count
#   tools/libsize.sh map <link.map> [N]       who owns the bytes: SDL3, SDL_image, third party, each src/<module>,
#                                             by module x section, and the N largest input files (default 25)
#   tools/libsize.sh syms <libember.so> [N]   the N largest symbols (default 40) of an unstripped library
#   tools/libsize.sh build <src> <out> [cc] [net]
#                                             configures and builds three measurement builds in <out>:
#                                             <out>/lto (the shipped one: MIN_SIZE, LTO, shared), <out>/ext (the same
#                                             with EMBER_SDL_EXTERNAL=ON: the library without SDL3 and SDL_image, so its
#                                             stripped size is Ember's own part) and <out>/map (the same flags with
#                                             EMBER_LTO=OFF and a linker map, for attribution), then prints the reports. net is ON or OFF (default OFF).
#                                             Extra cmake arguments go in CMAKE_ARGS.
#
# Why three builds: SDL3 and SDL_image are linked into libember statically, and the budget excludes them. The ext build
# leaves them out, so its stripped size is Ember's own part, exactly (undefined SDL_* names are resolved by the host).
# With LTO the linker sees one merged object, so a map cannot say which module a byte came from: the map build has the
# same flags without LTO and is used only for the split by module and section (its total is not the shipped size).
set -eu

# mawk has no strtonum: a hex parser for the awk programs below.
HEXFN='function hex(s,   i, c, v) { sub(/^0x/, "", s); v = 0; s = tolower(s)
    for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)) - 1; v = v * 16 + c } return v }'

strip_copy() { # lib -> path of a stripped copy
    t=$(mktemp)
    cp "$1" "$t"
    if command -v strip >/dev/null 2>&1; then strip --strip-all "$t"; else llvm-strip "$t"; fi
    echo "$t"
}

cmd_total() {
    lib=$1
    s=$(strip_copy "$lib")
    bytes=$(wc -c <"$s" | tr -d ' ')
    echo "stripped size: $bytes bytes"
    readelf -SW "$s" | sed 's/\[ *[0-9]*\]//' | awk "$HEXFN"'
        $1 ~ /^\./ && NF >= 6 { size = hex($5); if ($2 != "NOBITS") { printf "%-18s %10d\n", $1, size } }' | sort -k2 -n -r | head -16
    echo "dynamic symbols: $(readelf --dyn-syms -W "$lib" | awk 'NR > 3' | wc -l | tr -d ' ')"
    echo "exported (defined, default visibility): $(nm -D --defined-only "$lib" | wc -l | tr -d ' ')  of which emb_*: $(nm -D --defined-only "$lib" | awk '$3 ~ /^emb_/' | wc -l | tr -d ' ')"
    rm -f "$s"
}

cmd_map() {
    map=$1
    top=${2:-25}
    awk -v top="$top" "$HEXFN"'
    function owner(f,   m) {
        if (f ~ /libSDL3_image/ || f ~ /SDL3_image/) return "SDL_image"
        if (f ~ /libSDL3/ || f ~ /third_party\/SDL\// || f ~ /SDL3-static/) return "SDL3"
        if (match(f, /ember_[A-Za-z0-9_]+\.dir/)) { m = substr(f, RSTART + 6, RLENGTH - 10); return "ember/" m }
        if (f ~ /libc\.|libm\.|libgcc|crt[1in]|\/usr\/lib|libpthread|libdl|libclang_rt/) return "libc/runtime"
        return "other"
    }
    function secclass(s) {
        if (s ~ /^\.text/) return "text"
        if (s ~ /^\.rodata/) return "rodata"
        if (s ~ /^\.data\.rel\.ro/) return "data.rel.ro"
        if (s ~ /^\.data/) return "data"
        if (s ~ /^\.bss/) return "bss"
        if (s ~ /^\.eh_frame/) return "eh_frame"
        if (s ~ /^\.(init|fini)/ || s ~ /^\.(ctors|dtors)/ || s ~ /^\.tdata/ || s ~ /^\.tbss/) return "misc"
        if (s ~ /^\.gcc_except_table/) return "except"
        return "other"
    }
    function note(sec, size, file, inname,   o, c) {
        if (size == 0) return
        # String and constant pools are merged: the first input of each kind carries the merged total, the others repeat
        # their own (pre-merge) sizes at addresses inside it.
        if (inname ~ /^\.rodata\.(str|cst)/ && seen[inname]++) return
        o = owner(file); c = secclass(sec)
        # The linker merges string and constant pools and builds its own tables, and the map credits the whole merged
        # section to the first object that contributed to it, so those bytes cannot be attributed to a module.
        if (inname ~ /^\.rodata\.(str|cst)/) o = "merged strings"
        else if (inname ~ /^(\.rela|\.dyn|\.gnu\.(hash|version)|\.hash|\.plt|\.got|\.eh_frame_hdr|\.interp|\.note|\.comment)/) o = "linker tables"
        if (c == "bss") { bssv[o] += size; return }
        tot[o] += size; cell[o, c] += size; cls[c] = 1; all += size; byfile[file] += size
    }
    /^Linker script and memory map/ { on = 1; next }
    !on { next }
    /^\./ { out = $1; next }
    /^ \.[^ ]+$/ { pend = $1; next }
    /^ \.[^ ]+ +0x[0-9a-f]+ +0x[0-9a-f]+ / { note(out, hex($3), $4, $1); pend = ""; next }
    pend != "" && /^ +0x[0-9a-f]+ +0x[0-9a-f]+ / { note(out, hex($2), $3, pend); pend = ""; next }
    END {
        n = 0; for (o in tot) { names[++n] = o }
        for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (tot[names[j]] > tot[names[i]]) { t = names[i]; names[i] = names[j]; names[j] = t }
        printf "%-16s %10s", "owner", "total"
        nc = split("text rodata data.rel.ro data eh_frame except misc other", cl, " ")
        for (c = 1; c <= nc; c++) printf " %11s", cl[c]
        printf "   (bytes of file-backed input sections; bss not counted)\n"
        for (i = 1; i <= n; i++) {
            o = names[i]; printf "%-16s %10d", o, tot[o]
            for (c = 1; c <= nc; c++) printf " %11d", cell[o, cl[c]]
            printf "\n"
            if (o ~ /^ember\//) ember += tot[o]
        }
        printf "%-16s %10d\n", "all inputs", all
        printf "Ember own (sum of ember/*): %d bytes; SDL3 %d; SDL_image %d\n", ember, tot["SDL3"], tot["SDL_image"]
        printf "\nlargest input files:\n"
        m = 0; for (f in byfile) { fl[++m] = f }
        for (i = 1; i <= top; i++) { best = i; for (j = i + 1; j <= m; j++) if (byfile[fl[j]] > byfile[fl[best]]) best = j
            if (best > m) break; t = fl[i]; fl[i] = fl[best]; fl[best] = t; if (i > m) break
            printf "%10d  %s\n", byfile[fl[i]], fl[i] }
    }' "$map"
}

cmd_syms() {
    lib=$1
    n=${2:-40}
    nm -S --size-sort -C "$lib" 2>/dev/null | tail -n "$n" | sort -r | awk "$HEXFN"'{ printf "%8d %s %s\n", hex($2), $3, $4 }'
}

cmd_build() {
    src=$1; out=$2; cc=${3:-clang-19}; net=${4:-OFF}
    mkdir -p "$out"
    common="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$cc -DEMBER_SHARED=ON -DEMBER_MIN_SIZE=ON -DEMBER_NET=$net -DEMBER_TESTS=OFF -DEMBER_EXAMPLES=OFF ${CMAKE_ARGS:-}"
    # shellcheck disable=SC2086
    cmake -S "$src" -B "$out/lto" $common >"$out/lto.cfg.log"
    cmake --build "$out/lto" --target ember >"$out/lto.build.log"
    # shellcheck disable=SC2086
    cmake -S "$src" -B "$out/ext" $common -DEMBER_SDL_EXTERNAL=ON >"$out/ext.cfg.log"
    cmake --build "$out/ext" --target ember >"$out/ext.build.log"
    # shellcheck disable=SC2086
    cmake -S "$src" -B "$out/map" $common -DEMBER_LTO=OFF "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-Map=$out/map/libember.map" >"$out/map.cfg.log"
    cmake --build "$out/map" --target ember >"$out/map.build.log"
    lib=$(find "$out/lto" -maxdepth 1 -name 'libember*.so' | head -n 1)
    echo "== shipped build (LTO), $cc, EMBER_NET=$net"
    cmd_total "$lib"
    echo "== Ember only (EMBER_SDL_EXTERNAL=ON: SDL3 and SDL_image left to the host), LTO"
    elib=$(find "$out/ext" -maxdepth 1 -name 'libember*.so' | head -n 1)
    cmd_total "$elib"
    echo "== map build (no LTO)"
    mlib=$(find "$out/map" -maxdepth 1 -name 'libember*.so' | head -n 1)
    cmd_total "$mlib"
    cmd_map "$out/map/libember.map"
}

c=${1:-}; [ $# -gt 0 ] && shift
case $c in
    total) cmd_total "$@" ;;
    map) cmd_map "$@" ;;
    syms) cmd_syms "$@" ;;
    build) cmd_build "$@" ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
esac
