#!/usr/bin/env bash
#
# summarize.sh - collect trajectory summaries from a run directory and
# print one compact row per run, plus a byte-level first-difference table
# against a chosen reference recording.
#
# usage: summarize.sh <rundir> <reference.rec>
set -u

DIR="$1"
REF="$2"
HDR=84

echo "run                         steps  dropped   spawns despawns   rows  payload_sha256(16)"
for f in "$DIR"/*.rec; do
    name="$(basename "$f" .rec)"
    sum="${f%.rec}.summary"
    # extract whole-line fields with sed; a plain grep on spawns would
    # also match the tail of despawns
    field() { sed -n "s/.*$1=\([0-9][0-9]*\).*/\1/p" "$sum" | head -1; }
    steps=$(field total_steps)
    dropped=$(field dropped)
    spawns=$(sed -n 's/.* spawns=\([0-9][0-9]*\) despawns.*/\1/p' "$sum")
    desp=$(field despawns)
    rows=$(field entity_rows)
    h=$(tail -c +$((HDR + 1)) "$f" | sha256sum | cut -c1-16)
    printf "%-26s %6s %8s %8s %9s %6s  %s\n" \
        "$name" "$steps" "$dropped" "$spawns" "$desp" "$rows" "$h"
done

echo
echo "first byte-level difference vs $(basename "$REF") (payload region, offset relative to payload start)"
ref_payload="$(mktemp)"; tail -c +$((HDR + 1)) "$REF" > "$ref_payload"
for f in "$DIR"/*.rec; do
    [ "$f" = "$REF" ] && continue
    p="$(mktemp)"; tail -c +$((HDR + 1)) "$f" > "$p"
    if cmp -s "$ref_payload" "$p"; then
        printf "%-26s IDENTICAL\n" "$(basename "$f")"
    else
        # cmp reports 1-based byte position within the payload region
        out=$(cmp "$ref_payload" "$p" 2>&1)
        off=$(printf '%s' "$out" | sed -n 's/.*byte \([0-9]*\).*/\1/p' | head -1)
        if [ -n "$off" ]; then
            printf "%-26s first_diff_payload_offset=%s\n" "$(basename "$f")" "$off"
        else
            # no content mismatch before one stream ends: length differs
            printf "%-26s length differs (ref=%s B, run=%s B; shorter is a byte-prefix)\n" \
                "$(basename "$f")" "$(stat -c%s "$ref_payload")" "$(stat -c%s "$p")"
        fi
    fi
    rm -f "$p"
done
rm -f "$ref_payload"
