#!/bin/sh
# micro.sh: bytes per object, array and closure.
#
# Usage: sh tools/bench/js/micro.sh HOST [HOST...]   (HOST: a bench_js_suite built by this tree, or by the
# J5m3 tree with the same two additions: a gc() global that collects when the script returns, and the
# "alloc-peak:" line)
#
# Each kind keeps N = 50000 items in an array and calls gc(); the host runs the collection when the script
# returns and prints "alloc-peak: PEAK live-now: BYTES" (embi_mem_live after it). The figure is
# (BYTES with 50000 items - BYTES with none) / 50000 - 8, the 8 being the slot of the array that holds
# the item. Malloc headers are not in it (embi_mem_live counts requested bytes), and chunk granularity
# (256 KiB over 50000 items) is under 6 bytes per item.
set -u
[ $# -ge 1 ] || { echo "usage: sh micro.sh HOST [HOST...]" >&2; exit 2; }
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
mk() { # name n body
    printf 'var N=%s; var keep=new Array(N);\nfor (var i=0;i<N;i++){ keep[i]=%s; }\nglobalThis.K=keep; gc();\n' "$2" "$3" > "$tmp/$1_$2.js"
}
for n in 0 50000; do
    mk obj2 $n '{a:i,b:2}'
    mk obj6 $n '{a:i,b:2,c:3,d:4,e:5,f:6}'
    mk obj12 $n '{a:i,b:2,c:3,d:4,e:5,f:6,g:7,h:8,i:9,j:10,k:11,l:12}'
    mk arr5 $n '[i,2,3,4,5]'
    mk arr20 $n '[i,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]'
    mk arrow $n '(() => 1)'
    mk clos0 $n '(function(){return i;})'
    mk clos1 $n '(function(x){return function(){return x;};})(i)'
    { printf 'class P{constructor(x){this.x=x;this.y=2;}}\n'; cat "$tmp/obj2_$n.js" | sed 's/{a:i,b:2}/new P(i)/'; } > "$tmp/cls_$n.js"
done
for host in "$@"; do
    echo "== $host"
    for k in obj2 obj6 obj12 arr5 arr20 arrow clos0 clos1 cls; do
        a=$("$host" "$tmp/${k}_0.js" 2>&1 >/dev/null | awk '/alloc-peak/ { print $4 }')
        b=$("$host" "$tmp/${k}_50000.js" 2>&1 >/dev/null | awk '/alloc-peak/ { print $4 }')
        awk -v k="$k" -v a="$a" -v b="$b" 'BEGIN { printf "%-8s %7.1f bytes per item\n", k, (b - a) / 50000 - 8 }'
    done
done
