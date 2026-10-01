#!/usr/bin/env bash
# Reproducible EFS/BBHash benchmark. Run from any directory:
#   CHECK_N="100 1000 2000" MIN_BUILD_SECONDS=5 ./bench_bbhash.sh
#   ./bench_bbhash.sh --bbhash-ref c0162f4578ddbb066a944d23f0dc35ddc86254b0
#
# The test runner reports MPH bytes (summed over every directory), full image
# bytes, their ratio, time spent inside MPH builders, and total EFS build time.
# The timed batch lasts at least MIN_BUILD_SECONDS wall-clock seconds per set.
#
# --bbhash-ref REF benchmarks the BBHash header from an existing git revision
# using the current test runner and EFS sources (for before/after comparisons).
# --drop-caches requests a system-wide Linux page-cache drop before EACH round.
# It requires real uid 0 and a writable /proc/sys/vm/drop_caches; it runs sync
# first. This affects the whole machine. Without it, --cold-cache uses
# POSIX_FADV_DONTNEED on the generated source files and image (advisory only).
# To flush manually between runs as root: sync; echo 3 > /proc/sys/vm/drop_caches
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DROP_CACHES=0
BBHASH_REF=
while (($#)); do
    case $1 in
        --drop-caches) DROP_CACHES=1 ;;
        --bbhash-ref)
            (($# >= 2)) || { echo "--bbhash-ref requires a git revision" >&2; exit 2; }
            BBHASH_REF=$2
            shift
            ;;
        *)
            echo "usage: $0 [--drop-caches] [--bbhash-ref REV]" >&2
            exit 2
            ;;
    esac
    shift
done
if ((DROP_CACHES)) && [[ $(id -u) != 0 ]]; then
    echo "--drop-caches requires real uid 0; use --cold-cache advisories otherwise" >&2
    exit 2
fi
if ((DROP_CACHES)) && [[ ! -w /proc/sys/vm/drop_caches ]]; then
    echo "/proc/sys/vm/drop_caches is not writable" >&2
    exit 2
fi

MIN_BUILD_SECONDS=${MIN_BUILD_SECONDS:-5}
SEED=${SEED:-1}
CHECK_N=${CHECK_N:-"0 1 2 3 7 12 50 100 255 256 257 300 500 1000 2000"}
if [[ ! $MIN_BUILD_SECONDS =~ ^[1-9][0-9]*$ ]]; then
    echo "MIN_BUILD_SECONDS must be a positive integer" >&2
    exit 2
fi

BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/efs-bbhash-bench.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
for source in Makefile efs.h efs_mph.h efsbuilder.c efstest.c jmph.h bbhash.h oomph/tlist.h; do
    mkdir -p "$BUILD_DIR/$(dirname "$source")"
    cp "$ROOT/$source" "$BUILD_DIR/$source"
done
cp "$ROOT/oomph/tlist.h" "$BUILD_DIR/tlist.h"
if [[ -n $BBHASH_REF ]]; then
    if git -C "$ROOT" cat-file -e "$BBHASH_REF:bbhash.h" 2>/dev/null; then
        BBHASH_PATH=bbhash.h
    elif git -C "$ROOT" cat-file -e "$BBHASH_REF:oomph/mph.h" 2>/dev/null; then
        BBHASH_PATH=oomph/mph.h
    else
        echo "no bbhash.h or oomph/mph.h at revision $BBHASH_REF" >&2
        exit 2
    fi
    git -C "$ROOT" show "$BBHASH_REF:$BBHASH_PATH" > "$BUILD_DIR/bbhash.h"
fi
make -C "$BUILD_DIR" MPH=BBHASH \
    CFLAGS="${CFLAGS:-} -DEFS_BENCH -DMPH_API=static" efstest

field(){
    awk -v key="$1" '{for(i=1;i<=NF;i++) if(index($i,key "=")==1){sub(key "=","",$i); print $i; exit}}'
}

printf '# revision=%s seed=%s minimum_build_seconds=%s drop_caches=%s\n' \
    "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
    "$SEED" "$MIN_BUILD_SECONDS" "$DROP_CACHES"
if [[ -n $BBHASH_REF ]]; then
    printf '# bbhash_ref=%s:%s\n' "$BBHASH_REF" "$BBHASH_PATH"
fi
printf 'files\trounds\tavg_mph_ms\tavg_image_build_ms\tavg_total_ms\tavg_mph_bytes\tavg_image_bytes\tmph_pct_image\n'

for n in $CHECK_N; do
    rounds=0
    total_ns_sum=0
    build_ns_sum=0
    mph_ns_sum=0
    mph_bytes_sum=0
    image_bytes_sum=0
    batch_start=$SECONDS
    while ((SECONDS - batch_start < MIN_BUILD_SECONDS)); do
        if ((DROP_CACHES)); then
            sync
            printf '3\n' > /proc/sys/vm/drop_caches
        fi
        output=$("$BUILD_DIR/efstest" -n "$n" -s "$SEED" --bench-stats --cold-cache)
        line=$(printf '%s\n' "$output" | awk '/^BENCH /{print; exit}')
        if [[ -z $line ]]; then
            printf 'benchmark statistics missing for N=%s\n%s\n' "$n" "$output" >&2
            exit 1
        fi
        build_ns=$(printf '%s\n' "$line" | field build_ns)
        mph_ns=$(printf '%s\n' "$line" | field mph_ns)
        total_ns=$(printf '%s\n' "$line" | field total_ns)
        mph_bytes=$(printf '%s\n' "$line" | field mph_bytes)
        image_bytes=$(printf '%s\n' "$line" | field image_bytes)
        ((rounds+=1))
        ((total_ns_sum+=total_ns))
        ((build_ns_sum+=build_ns))
        ((mph_ns_sum+=mph_ns))
        ((mph_bytes_sum+=mph_bytes))
        ((image_bytes_sum+=image_bytes))
    done
    awk -v n="$n" -v rounds="$rounds" -v total="$total_ns_sum" \
        -v build="$build_ns_sum" -v mph_ns="$mph_ns_sum" \
        -v mph="$mph_bytes_sum" -v image="$image_bytes_sum" \
        'BEGIN { printf "%s\t%d\t%.3f\t%.3f\t%.3f\t%.1f\t%.1f\t%.3f%%\n", n, rounds,
                 mph_ns/rounds/1000000, build/rounds/1000000,
                 total/rounds/1000000, mph/rounds, image/rounds,
                 image ? 100*mph/image : 0 }'
done
