#!/usr/bin/env bash
#
# reproduce.sh - determinism matrix for mini-sim-lab replay.
#
# Runs the fixed build across thread counts and frame groupings, three
# repetitions each, and verifies that all trajectory payloads are
# byte-identical. Then runs the legacy build the same way to demonstrate
# the pre-fix drift. Writes machine-readable results under <outdir>.
#
# usage: reproduce.sh [outdir]
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/artifacts/runs}"
FIXED="$ROOT/sim/sim"
LEGACY="$ROOT/sim/sim-legacy"
THREADS="1 2 4 8 16"
REPS=3
# RecHeader is 84 packed bytes; everything after it is trajectory payload.
HDR=84

mkdir -p "$OUT/fixed" "$OUT/legacy"

echo "### building"
make -C "$ROOT/fixedstep" clean all >/dev/null
make -C "$ROOT/pool" clean all >/dev/null
make -C "$ROOT/rng" clean all >/dev/null
make -C "$ROOT/sim" clean all >/dev/null

run_matrix() {
    local bin="$1" tag="$2" dir="$3"
    for rep in $(seq 1 "$REPS"); do
        for t in $THREADS; do
            for m in 0 1; do
                name="r${rep}_t${t}_m${m}"
                "$bin" "$t" "$m" "$dir/$name.rec" >"$dir/$name.summary"
            done
        done
    done
}

echo "### fixed build matrix ($REPS reps x [${THREADS}] x merge 0/1)"
run_matrix "$FIXED" fixed "$OUT/fixed"

echo "### legacy build matrix"
run_matrix "$LEGACY" legacy "$OUT/legacy"

# payload hash (excludes the input-parameter header, incl. thread count)
hash_payload() { tail -c +$((HDR + 1)) "$1" | sha256sum | cut -d' ' -f1; }

echo
echo "================= FIXED: payload hashes (must all match) ================="
ref="$(hash_payload "$OUT/fixed/r1_t1_m0.rec")"
fixed_fail=0
{
printf "rep threads merge payload_sha256 match_ref\n"
for rep in $(seq 1 "$REPS"); do
    for t in $THREADS; do
        for m in 0 1; do
            f="$OUT/fixed/r${rep}_t${t}_m${m}.rec"
            h="$(hash_payload "$f")"
            if [ "$h" = "$ref" ]; then ok=YES; else ok=NO; fixed_fail=1; fi
            printf "%3d %7d %5d %s %s\n" "$rep" "$t" "$m" "$h" "$ok"
        done
    done
done
} | tee "$OUT/fixed/bit_matrix.txt"
echo "REF_PAYLOAD_SHA256=$ref" | tee "$OUT/fixed/reference.txt"

echo
echo "================= LEGACY: payload hashes (expected to diverge) ==========="
lref="$(hash_payload "$OUT/legacy/r1_t1_m0.rec")"
flag="$(mktemp)"
{
printf "rep threads merge payload_sha256 same_as_baseline\n"
for rep in $(seq 1 "$REPS"); do
    for t in $THREADS; do
        for m in 0 1; do
            f="$OUT/legacy/r${rep}_t${t}_m${m}.rec"
            h="$(hash_payload "$f")"
            if [ "$rep" = 1 ] && [ "$t" = 1 ] && [ "$m" = 0 ]; then
                ok=BASELINE
            elif [ "$h" = "$lref" ]; then
                ok=SAME
            else
                ok=DIFF; echo 1 > "$flag"
            fi
            printf "%3d %7d %5d %s %s\n" "$rep" "$t" "$m" "$h" "$ok"
        done
    done
done
} | tee "$OUT/legacy/bit_matrix.txt"
legacy_different="$(cat "$flag")"; rm -f "$flag"

echo
echo "================= unit tests ===================="
( cd "$ROOT/fixedstep" && make test ) >"$OUT/fixedstep_test.log" 2>&1 && echo "fixedstep: PASS" || echo "fixedstep: FAIL"
( cd "$ROOT/pool" && make test ) >"$OUT/pool_test.log" 2>&1 && echo "pool: PASS" || echo "pool: FAIL"
( cd "$ROOT/rng" && make test ) >"$OUT/rng_test.log" 2>&1 && echo "rng: PASS" || echo "rng: FAIL"

echo
if [ "$fixed_fail" -eq 0 ] && [ "$legacy_different" -eq 1 ]; then
    echo "RESULT: PASS - fixed payloads bit-identical across all $((REPS*5*2)) runs; legacy drifts"
    echo 0 > "$OUT/result.exit"
else
    echo "RESULT: FAIL (fixed_fail=$fixed_fail legacy_drifting=$legacy_different)"
    echo 1 > "$OUT/result.exit"
fi
exit "$(cat "$OUT/result.exit")"
