#!/bin/sh
# test_soomph.sh - exercise soomph and verify it produces a correct,
# minimal perfect hash (MPHF) for a generated set of string keys.
#
# Pipeline:
#   1. generate N_INPUTS distinct keys (seq N_INPUTS),
#   2. pipe them into soomph -> emit C source into a temp file,
#   3. compile that C with -DTEST,
#   4. pipe the SAME keys into the test executable (it prints "<key>\t<rank>"),
#   5. verify: exactly N_INPUTS unique input lines, and every emitted rank
#      is in [1..N_INPUTS] (1-indexed; nothing exceeds N_INPUTS+1).
set -eu

# Generator executable (defaults to ./soomph, mirroring the original script).
GEN="${1:-./soomph}"

# Locate repo root (directory of this script).
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd -- "$ROOT"

# Build the generator if needed.
if [ ! -x "$GEN" ] && [ ! -x "./$GEN" ]; then
    make "$(basename "$GEN")"
fi

N_INPUTS=${N_INPUTS:-10000}

# Portable timing (the `time` command is not always available /bin/sh).
now() { date +%s.%N 2>/dev/null || echo 0; }
ts_start=$(now)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/soomph_test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

KEYS="$WORK/keys.txt"
GEN_C="$WORK/gen.c"
GEN_BIN="$WORK/gen"
OUT="$WORK/out.tsv"

# 1) Generate the input keys.
seq "$N_INPUTS" > "$KEYS"

# 2) Generate the MPHF C source from the input.
./soomph < "$KEYS" > "$GEN_C"

# 3) Compile it with -DTEST so it can self-check against the input.
cc -std=c11 -O2 -DTEST -o "$GEN_BIN" "$GEN_C"

# 4) Run the TEST program on the SAME input; it prints "<key>\t<rank>".
"$GEN_BIN" < "$KEYS" > "$OUT"

# 5a) The input must have exactly N_INPUTS unique lines.
unique=$(sort -u "$KEYS" | wc -l | tr -d ' ')
if [ "$unique" -ne "$N_INPUTS" ]; then
    echo "FAIL: expected $N_INPUTS unique input lines, got $unique" >&2
    exit 1
fi

# 5b) The number of output lines must equal N_INPUTS.
got=$(wc -l < "$OUT" | tr -d ' ')
if [ "$got" -ne "$N_INPUTS" ]; then
    echo "FAIL: expected $N_INPUTS output lines, got $got" >&2
    head "$OUT" >&2
    exit 1
fi

# 5c) Every emitted rank must be in [1..N_INPUTS]; i.e. none is > N_INPUTS+1
#     (output is 1-indexed, so the max valid rank is N_INPUTS).
max_rank=$(awk -F'\t' 'NF>=2 && $2+0 > m { m = $2+0 } END { print (m+0) }' "$OUT")
limit=$((N_INPUTS + 1))
if [ "$max_rank" -gt "$limit" ]; then
    echo "FAIL: rank $max_rank exceeds allowed maximum $limit (N_INPUTS+1)" >&2
    head "$OUT" >&2
    exit 1
fi

# 5d) Sanity: all ranks distinct (a real perfect hash is a permutation of 1..N).
distinct=$(awk -F'\t' 'NF>=2 { print $2 }' "$OUT" | sort -u | wc -l | tr -d ' ')
if [ "$distinct" -ne "$N_INPUTS" ]; then
    echo "FAIL: ranks are not a permutation of 1..$N_INPUTS (distinct=$distinct)" >&2
    head "$OUT" >&2
    exit 1
fi

ts_end=$(now)
elapsed=$(awk -v a="$ts_start" -v b="$ts_end" 'BEGIN { printf "%.3f", (b+0) - (a+0) }')

echo "PASS: soomph produced a valid MPHF for $N_INPUTS keys (ranks 1..$N_INPUTS, all distinct)"
echo "      (generator: $GEN; elapsed: ${elapsed}s)"
