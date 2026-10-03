#!/bin/sh
DIR=$(realpath "$1")
EFS="/tmp/test.$$.efs"

./efsbuilder "$EFS" "$DIR" || { echo "build failed"; exit 1; }

BAD="/tmp/badlist.$$.txt"
: > "$BAD"

find "$DIR" -type f ! -name "$EFS" | while IFS= read -r f; do
    rel="${f#$DIR/}"
    if ! ./efsreader "$EFS" "/$rel" > "/tmp/efs_out.$$" 2>/dev/null; then
        echo "MISS: $rel"
        echo "$rel" >> "$BAD"
        continue
    fi
    if ! cmp -s "$f" "/tmp/efs_out.$$"; then
        echo "MISMATCH: $rel"
        echo "$rel" >> "$BAD"
    else
        echo "GOOD: $rel"
    fi
    rm -f "/tmp/efs_out.$$"
done

if [ -s "$BAD" ]; then
    echo "failures:"
    cat "$BAD"
    rm -f "$BAD"
    echo "kept $EFS for inspection. delete manually."
    exit 1
else
    echo "check passed"
    rm -f "$BAD" "$EFS"
fi

