#!/bin/sh
# Runs the benchmark suite in an unprivileged network namespace, with one
# impairment spec applied to BOTH netem and ZNET_BENCH_IMPAIR so the two can
# never disagree (a mismatch silently mislabels and mis-scales the table).
#
# usage: run.sh [-i SPEC] [-c CORES] [-r REPS] [-o CSV] [-p KIND] [-s] BENCHDIR [BIN...]
#   -i SPEC   impairment, e.g. "delay=25,jitter=5,loss=5,dup=1" (one-way values)
#   -c CORES  taskset core list, e.g. 0-7,16-23 (keep it inside one L3 domain)
#   -r REPS   repetitions per case (ZNET_BENCH_REPS)
#   -o CSV    machine-readable output file (ZNET_BENCH_CSV; local use)
#   -p KIND   payload kind: binary|snapshot|text (ZNET_BENCH_PAYLOAD)
#   -s        skip the congestion pool (ZNET_BENCH_SKIP_CONGESTION=1)
#   BENCHDIR  directory holding the benchmark binaries (e.g. build/benchmarks)
#   BIN...    binaries to run; default: every known one present in BENCHDIR
set -eu

# re-exec inside a fresh namespace, so traffic stays off the host's loopback
if [ -z "${ZNET_BENCH_IN_NS:-}" ]; then
    exec unshare -rn env ZNET_BENCH_IN_NS=1 "$0" "$@"
fi

SPEC=""
CORES=""
usage() {
    sed -n '2,/^set /p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
    exit 1
}
while getopts "i:c:r:o:p:sh" opt; do
    case "$opt" in
        i) SPEC=$OPTARG ;;
        c) CORES=$OPTARG ;;
        r) ZNET_BENCH_REPS=$OPTARG; export ZNET_BENCH_REPS ;;
        o) ZNET_BENCH_CSV=$OPTARG; export ZNET_BENCH_CSV ;;
        p) ZNET_BENCH_PAYLOAD=$OPTARG; export ZNET_BENCH_PAYLOAD ;;
        s) ZNET_BENCH_SKIP_CONGESTION=1; export ZNET_BENCH_SKIP_CONGESTION ;;
        h|?) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || usage
DIR=$1
shift
[ -d "$DIR" ] || { echo "run.sh: no such directory: $DIR" >&2; exit 1; }

ip link set lo up

if [ -n "$SPEC" ]; then
    # translate the spec into netem arguments; same string goes to the env
    DELAY=0 JITTER=0 LOSS="" DUP=""
    OLDIFS=$IFS; IFS=,
    for item in $SPEC; do
        key=${item%%=*}
        val=${item#*=}
        case "$key" in
            delay)  DELAY=$val ;;
            jitter) JITTER=$val ;;
            loss)   LOSS=$val ;;
            dup)    DUP=$val ;;
            *) echo "run.sh: unknown impairment key '$key'" >&2; exit 1 ;;
        esac
    done
    IFS=$OLDIFS
    NETEM=""
    if [ "$DELAY" != 0 ] || [ "$JITTER" != 0 ]; then
        NETEM="delay ${DELAY}ms ${JITTER}ms"
    fi
    [ -n "$LOSS" ] && NETEM="$NETEM loss ${LOSS}%"
    [ -n "$DUP" ] && NETEM="$NETEM duplicate ${DUP}%"
    # a real MTU, so fragmentation behaves like a network rather than loopback
    ip link set lo mtu 1500
    # shellcheck disable=SC2086
    tc qdisc add dev lo root netem $NETEM
    ZNET_BENCH_IMPAIR=$SPEC
    export ZNET_BENCH_IMPAIR
    echo "netem: $NETEM (lo, mtu 1500)"
fi

[ $# -ge 1 ] || set -- znet-bench baseline-bench fanout-bench enet-bench raknet-bench gns-bench

for bin in "$@"; do
    if [ ! -x "$DIR/$bin" ]; then
        echo "-- skipping $bin (not built)"
        continue
    fi
    echo "== $bin =="
    if [ -n "$CORES" ]; then
        taskset -c "$CORES" "$DIR/$bin"
    else
        "$DIR/$bin"
    fi
done
