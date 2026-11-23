#!/usr/bin/env bash
# det/verify.sh: prove the determinism contract for fixedstep, pool and rng.
#
# Checks, per module:
#   1. two independent runs are byte-for-byte identical (stdout + state hash)
#   2. running under a single pinned CPU changes nothing (schedule-independent)
#   3. --no-shared output is identical to --shared output
#   4. the thread table is identical across 1/2/4/8 threads
#   5. invalid parameters fail with a module-located error and non-empty stdout
#      is never produced before validation
#   6. --dump then --load reproduces the exact saved state hash
#
# Exit 0 only if every check passes.  Prints one PASS/FAIL line per check.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODULES=(fixedstep pool rng)
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; fails=$((fails + 1)); }

sha() { sha256sum "$1" | cut -d' ' -f1; }

# 0. The modules' original self-tests must still print exactly what they did
#    before the shared context existed: the refactor is not allowed to move
#    any published output, only to add evidence around it.
for m in "${MODULES[@]}"; do
    if [ ! -x "$ROOT/$m/$m" ]; then
        fail "$m: self-test binary missing (run make -C $m all)"
        continue
    fi
    if "$ROOT/$m/$m" >"$TMP/$m.selftest" 2>/dev/null &&
       cmp -s "$TMP/$m.selftest" "$ROOT/det/evidence/baseline-$m.txt"; then
        pass "$m: self-test output unchanged from pre-refactor baseline"
    else
        fail "$m: self-test output drifted from baseline"
        diff "$ROOT/det/evidence/baseline-$m.txt" "$TMP/$m.selftest" | sed 's/^/      /'
    fi
done

for m in "${MODULES[@]}"; do
    bin="$ROOT/$m/evidence"
    if [ ! -x "$bin" ]; then
        fail "$m: evidence binary missing (run make -C $m all)"
        continue
    fi

    # 1. two independent runs
    "$bin" --shared >"$TMP/$m.a" 2>"$TMP/$m.a.err"
    "$bin" --shared >"$TMP/$m.b" 2>"$TMP/$m.b.err"
    if cmp -s "$TMP/$m.a" "$TMP/$m.b" && [ "$(sha "$TMP/$m.a")" = "$(sha "$TMP/$m.b")" ]; then
        pass "$m: two runs byte-for-byte identical ($(sha "$TMP/$m.a" | cut -c1-16)…)"
    else
        fail "$m: two runs differ"
    fi
    state_a=$(grep -E "^$m\.state_hash=" "$TMP/$m.a" | cut -d= -f2)
    if [ -z "$state_a" ]; then fail "$m: no state_hash in output"; fi

    # 2. single-pinned-CPU run
    if command -v taskset >/dev/null 2>&1; then
        taskset -c 0 "$bin" --shared >"$TMP/$m.pin" 2>/dev/null
        if cmp -s "$TMP/$m.a" "$TMP/$m.pin"; then
            pass "$m: single pinned CPU identical"
        else
            fail "$m: output depends on CPU scheduling"
        fi
    fi

    # 3. --no-shared equivalence
    "$bin" --no-shared >"$TMP/$m.ns" 2>"$TMP/$m.ns.err"
    if cmp -s "$TMP/$m.a" "$TMP/$m.ns"; then
        pass "$m: --no-shared identical to --shared"
    else
        fail "$m: --no-shared differs from --shared"
        diff "$TMP/$m.a" "$TMP/$m.ns" | sed 's/^/      /'
    fi

    # 4. thread-independence table
    digests=$(grep -E "^$m\.threads\.[0-9]+\.digest=" "$TMP/$m.a" | cut -d= -f2 | sort -u | wc -l)
    eq=$(grep -E "^$m\.table_equal=" "$TMP/$m.a" | cut -d= -f2)
    if [ "$digests" = "1" ] && [ "$eq" = "1" ]; then
        pass "$m: thread table identical across $(grep -cE "^$m\.threads\.[0-9]+\.digest=" "$TMP/$m.a") thread counts"
    else
        fail "$m: thread table differs (distinct digests=$digests, table_equal=$eq)"
    fi

    # 5. invalid parameters: exit 2, module-named stderr, no partial stdout
    case "$m" in
        fixedstep) bad=(--frames 0 --step -1 --catchup xyz --threads 4,2 --bogus 1) ;;
        pool)      bad=(--ops 0 --tiles -1 --threads 8,1 --bogus 1) ;;
        rng)       bad=(--draws "" --seed -1 --threads 1,9 --bogus 1) ;;
    esac
    i=0
    while [ $i -lt ${#bad[@]} ]; do
        opt="${bad[$i]}"; val="${bad[$((i + 1))]}"; i=$((i + 2))
        "$bin" --no-shared "$opt" "$val" >"$TMP/e.out" 2>"$TMP/e.err"
        rc=$?
        msg=$(cat "$TMP/e.err")
        if [ $rc -eq 2 ] && printf '%s' "$msg" | grep -q "^$m: " && [ ! -s "$TMP/e.out" ]; then
            pass "$m: rejects $opt $val -> ${msg}"
        else
            fail "$m: bad handling of $opt $val (rc=$rc, stdout=$(wc -c <"$TMP/e.out")B, msg='$msg')"
        fi
    done

    # 6. dump / load round-trip
    "$bin" --shared --dump "$TMP/$m.bin" >"$TMP/$m.dumped" 2>/dev/null
    "$bin" --shared --load "$TMP/$m.bin" >"$TMP/$m.loaded" 2>"$TMP/$m.loaded.err"
    h_dump=$(grep -E "^$m\.state_hash=" "$TMP/$m.dumped" | cut -d= -f2)
    h_load=$(grep -E "^$m\.state_hash=" "$TMP/$m.loaded" | cut -d= -f2)
    if [ -n "$h_dump" ] && [ "$h_dump" = "$h_load" ]; then
        pass "$m: --dump/--load reproduces state ($h_dump)"
    else
        fail "$m: --dump/--load mismatch ($h_dump vs $h_load)"
    fi

    # 6b. snapshot bytes are themselves deterministic, and identical whether
    #     taken through the shared context or a private block.
    "$bin" --shared --dump "$TMP/$m.snap1" --threads 1 >/dev/null 2>&1
    "$bin" --shared --dump "$TMP/$m.snap2" --threads 8 >/dev/null 2>&1
    "$bin" --no-shared --dump "$TMP/$m.snap3" >/dev/null 2>&1
    if cmp -s "$TMP/$m.snap1" "$TMP/$m.snap2" && cmp -s "$TMP/$m.snap1" "$TMP/$m.snap3"; then
        pass "$m: snapshot bytes deterministic ($(sha "$TMP/$m.snap1" | cut -c1-16)…)"
    else
        fail "$m: snapshot bytes differ across runs/thread-counts/modes"
    fi

    # 6c. a foreign snapshot is rejected by module
    other=fixedstep; [ "$m" = fixedstep ] && other=rng
    otherbin="$ROOT/$other/evidence"
    if [ -x "$otherbin" ]; then
        "$otherbin" --shared --dump "$TMP/$other.only" >/dev/null 2>&1
        "$bin" --no-shared --load "$TMP/$other.only" >"$TMP/f.out" 2>"$TMP/f.err"
        rc=$?
        if [ $rc -eq 2 ] && grep -q "^$m: load:" "$TMP/f.err"; then
            pass "$m: rejects $other snapshot -> $(cat "$TMP/f.err")"
        else
            fail "$m: did not reject foreign snapshot (rc=$rc)"
        fi
    fi
    # 7. full stdout matches the frozen golden file, byte for byte
    if cmp -s "$TMP/$m.a" "$ROOT/det/evidence/golden-$m.txt"; then
        pass "$m: stdout matches golden ($(sha "$TMP/$m.a" | cut -c1-16)…)"
    else
        fail "$m: stdout drifted from golden-$m.txt"
        diff "$ROOT/det/evidence/golden-$m.txt" "$TMP/$m.a" | sed 's/^/      /'
    fi
done

echo "-----"
if [ $fails -eq 0 ]; then
    echo "ALL CHECKS PASSED"
    exit 0
fi
echo "$fails CHECK(S) FAILED"
exit 1
