#!/usr/bin/env bash
# det/reproduce.sh: the copy-pasteable reproduction for the determinism claim.
#
# For each module it:
#   1. runs the evidence driver twice and compares the full stdout byte for
#      byte (this is the "two runs must be bit-identical" check),
#   2. re-runs under a single pinned CPU and compares again,
#   3. hashes the whole stdout with sha256sum so the digest can be quoted,
#   4. compares the --no-shared output against --shared.
#
# Usage:  ./det/reproduce.sh            # human-readable
#         ./det/reproduce.sh --tsv      # one stable line per module for diffing
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-pretty}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fmt=(); [ "$MODE" = "--tsv" ] && fmt=(-t)
if [ "$MODE" = "--tsv" ]; then
    printf 'module\tstate_hash\ttable_hash\tstdout_sha256\truns_identical\tcpu_identical\tshared_identical\n'
fi

bad=0
for m in fixedstep pool rng; do
    bin="$ROOT/$m/evidence"
    "$bin" --shared >"$TMP/$m.a" 2>/dev/null
    "$bin" --shared >"$TMP/$m.b" 2>/dev/null
    taskset -c 0 "$bin" --shared >"$TMP/$m.pin" 2>/dev/null
    "$bin" --no-shared >"$TMP/$m.ns" 2>/dev/null

    runs=1; cmp -s "$TMP/$m.a" "$TMP/$m.b" || runs=0
    cpu=1;  cmp -s "$TMP/$m.a" "$TMP/$m.pin" || cpu=0
    shr=1;  cmp -s "$TMP/$m.a" "$TMP/$m.ns" || shr=0
    [ "$runs" = 1 ] && [ "$cpu" = 1 ] && [ "$shr" = 1 ] || bad=1
    state=$(grep -E "^$m\.state_hash=" "$TMP/$m.a" | cut -d= -f2)
    table=$(grep -E "^$m\.table_hash=" "$TMP/$m.a" | cut -d= -f2)
    outsha=$(sha256sum "$TMP/$m.a" | cut -d' ' -f1)

    if [ "$MODE" = "--tsv" ]; then
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$m" "$state" "$table" "$outsha" "$runs" "$cpu" "$shr"
    else
        printf '%-10s state=%-16s table=%-16s stdout=%s\n' "$m" "$state" "$table" "${outsha:0:24}…"
        printf '           runs_identical=%s cpu_identical=%s shared_identical=%s\n' \
            "$runs" "$cpu" "$shr"
    fi
done

# Exit non-zero if any module violated a property.
exit "$bad"
