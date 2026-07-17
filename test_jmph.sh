#!/bin/sh
# test_jmph.sh - exercise the jmph.h MPH builder and verify it produces a
# correct, minimal perfect hash (MPHF) for a generated set of string keys.
#
# Pipeline (mirrors oomph's test_soomph.sh):
#   1. generate N_INPUTS distinct keys (seq N_INPUTS),
#   2. pipe them into the generator (jmph_test, built from jmph.h) which
#      builds the MPHF and emits "<key>\t<rank>" for every key,
#   3. verify: exactly N_INPUTS unique input lines, the output is a
#      permutation of 1..N_INPUTS (every rank in [1..N], all distinct).
set -eu

# Generator executable (defaults to ./jmph_test).
GEN="${1:-./jmph_test}"

# Locate repo root (directory of this script).
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd -- "$ROOT"

# Build the generator if needed.
if [ ! -x "$GEN" ] && [ ! -x "./$GEN" ]; then
    make "$(basename "$GEN")"
fi

N_INPUTS=${N_INPUTS:-10000}

# Portable timing (the `time` command is not always available under /bin/sh).
now() { date +%s.%N 2>/dev/null || echo 0; }
ts_start=$(now)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/jmph_test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

KEYS="$WORK/keys.txt"
OUT="$WORK/out.tsv"

# 1) Generate the input keys.
seq "$N_INPUTS" > "$KEYS"

# 2) Build the MPHF and emit key<->rank pairs.
./"$GEN" < "$KEYS" > "$OUT"

# 3a) The input must have exactly N_INPUTS unique lines.
unique=$(sort -u "$KEYS" | wc -l | tr -d ' ')
if [ "$unique" -ne "$N_INPUTS" ]; then
    echo "FAIL: expected $N_INPUTS unique input lines, got $unique" >&2
    exit 1
fi

# 3b) The number of output lines must equal N_INPUTS.
got=$(wc -l < "$OUT" | tr -d ' ')
if [ "$got" -ne "$N_INPUTS" ]; then
    echo "FAIL: expected $N_INPUTS output lines, got $got" >&2
    head "$OUT" >&2
    exit 1
fi

# 3c) Every emitted rank must be in [1..N_INPUTS]; none may exceed N_INPUTS.
max_rank=$(awk -F'\t' 'NF>=2 && $2+0 > m { m = $2+0 } END { print (m+0) }' "$OUT")
if [ "$max_rank" -gt "$N_INPUTS" ]; then
    echo "FAIL: rank $max_rank exceeds allowed maximum $N_INPUTS" >&2
    head "$OUT" >&2
    exit 1
fi

# 3d) All ranks distinct (a real minimal perfect hash is a permutation of 1..N).
distinct=$(awk -F'\t' 'NF>=2 { print $2 }' "$OUT" | sort -u | wc -l | tr -d ' ')
if [ "$distinct" -ne "$N_INPUTS" ]; then
    echo "FAIL: ranks are not a permutation of 1..$N_INPUTS (distinct=$distinct)" >&2
    head "$OUT" >&2
    exit 1
fi

ts_end=$(now)
elapsed=$(awk -v a="$ts_start" -v b="$ts_end" 'BEGIN { printf "%.3f", (b+0) - (a+0) }')

echo "PASS: jmph produced a valid MPHF for $N_INPUTS keys (ranks 1..$N_INPUTS, all distinct)"
echo "      (generator: $GEN; elapsed: ${elapsed}s)"
