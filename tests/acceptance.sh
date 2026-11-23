#!/usr/bin/env bash
#
# acceptance.sh — deterministic-context acceptance for fixedstep / pool / rng.
#
# Verifies, for EACH of the three modules:
#   A. two consecutive runs produce byte-identical output (bit-for-bit);
#   B. output is independent of the worker thread count (1..oversubscription);
#   C. with the shared context disabled, output equals the original pre-change
#      program (rebuilt from git HEAD), byte-for-byte;
#   D. invalid context parameters are rejected with a nonzero exit and an
#      "error:" message, while boundary-valid values are accepted.
# Also verifies no third-party libraries are linked (libc/libm/pthread only).
#
# Requirements: bash, make, gcc, git, cmp, sha256sum, ldd, grep, sed.
# Nothing is downloaded or installed.
#
# Usage:  ./tests/acceptance.sh
# Exit:   0 only if every check passes.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODULES="fixedstep pool rng"
THREAD_LIST="1 2 4 16 32 64 128"
SEED_LIST="0 42 18446744073709551615"
PASS=0
FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

ok()   { PASS=$((PASS + 1)); printf '  [PASS] %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  [FAIL] %s\n' "$1"; }
note() { printf '  ....  %s\n' "$1"; }

# ---------------------------------------------------------------- build
echo "== build =="
make clean >/dev/null 2>&1
if make >"$TMP/build.log" 2>&1; then
    ok "all three modules build with -Wall -Wextra (no warnings above)"
    if grep -qi "warning" "$TMP/build.log"; then
        bad "compiler emitted warnings"
        sed 's/^/        /' "$TMP/build.log"
    else
        ok "zero compiler warnings"
    fi
else
    bad "build failed"
    sed 's/^/        /' "$TMP/build.log"
    echo "RESULT: FAIL ($FAIL failed)"; exit 1
fi

# ----------------------------------------------- A+B: determinism and threads
echo
echo "== A/B: bit-identical reruns and thread-count independence =="
# The first output line echoes the requested --threads value, so it is input
# echo rather than computed payload. Strip just that field when comparing runs
# made with different thread counts; identical reruns are still compared raw.
normalize() { sed 's/threads=[0-9][0-9]*/threads=N/'; }

for mod in $MODULES; do
    bin="./$mod/$mod"
    for seed in $SEED_LIST; do
        hashes=""
        for t in $THREAD_LIST; do
            "$bin" --seed="$seed" --threads="$t" > "$TMP/$mod-$seed-$t.a" 2>"$TMP/err" || { bad "$mod seed=$seed threads=$t exited nonzero"; cat "$TMP/err"; continue; }
            # A: second consecutive run, full file byte-for-byte
            "$bin" --seed="$seed" --threads="$t" > "$TMP/$mod-$seed-$t.b" 2>/dev/null
            if cmp -s "$TMP/$mod-$seed-$t.a" "$TMP/$mod-$seed-$t.b"; then
                ok "$mod seed=$seed threads=$t: two runs byte-identical"
            else
                bad "$mod seed=$seed threads=$t: rerun differs"
            fi
            h="$(normalize < "$TMP/$mod-$seed-$t.a" | sha256sum | cut -d' ' -f1)"
            hashes="$hashes $h"
        done
        distinct="$(printf '%s\n' $hashes | sort -u | wc -l)"
        if [ "$distinct" -eq 1 ]; then
            ok "$mod seed=$seed: identical payload sha256 across thread counts [$THREAD_LIST]"
            printf '          payload-sha256=%s\n' "$(printf '%s\n' $hashes | sort -u)"
        else
            bad "$mod seed=$seed: payload changes with thread count"
            printf '%s\n' $hashes | sort | uniq -c
        fi
        # and the payload checksum line printed by the program itself must match
        cs="$(for t in $THREAD_LIST; do "$bin" --seed="$seed" --threads="$t"; done | grep '^checksum=' | sort -u | wc -l)"
        [ "$cs" -eq 1 ] && ok "$mod seed=$seed: program-printed checksum invariant across threads" \
                          || bad "$mod seed=$seed: checksum line varies across threads"
    done

    # scheduling stress: many repeated runs on many threads, every payload equal
    ref="$TMP/$mod-stress-ref"
    "./$mod/$mod" --seed=777 --threads=16 | normalize > "$ref"
    differ=0
    for i in $(seq 1 20); do
        "./$mod/$mod" --seed=777 --threads=16 | normalize > "$TMP/stress" || differ=1
        cmp -s "$ref" "$TMP/stress" || differ=1
    done
    [ "$differ" -eq 0 ] && ok "$mod: 20 repeated 16-thread runs all byte-identical" \
                            || bad "$mod: repeated runs diverged"
done

# ----------------------------- C: shared off == original program from git HEAD
echo
echo "== C: --shared=off output equals the original pre-change program =="
HAVE_GIT=0
if git rev-parse --verify HEAD >/dev/null 2>&1 && \
   git cat-file -e "HEAD:fixedstep/src/fixedstep.c" >/dev/null 2>&1; then
    HAVE_GIT=1
fi
if [ "$HAVE_GIT" -eq 0 ]; then
    note "git history unavailable; falling back to recorded baselines in artifacts/baseline/"
fi
mkdir -p "$TMP/orig"
for mod in $MODULES; do
    legacy_ref="$TMP/orig/$mod.out"
    if [ "$HAVE_GIT" -eq 1 ]; then
        git show "HEAD:$mod/src/$mod.c" > "$TMP/orig/$mod.c" 2>/dev/null || {
            bad "$mod: cannot extract HEAD:$mod/src/$mod.c from git"; continue; }
        extra="-lm"; [ "$mod" != "fixedstep" ] && extra=""
        gcc -O2 -std=c11 -Wall -Wextra "$TMP/orig/$mod.c" -o "$TMP/orig/$mod" $extra 2>"$TMP/cc.log" || {
            bad "$mod: original source rebuild failed"; sed 's/^/        /' "$TMP/cc.log"; continue; }
        "$TMP/orig/$mod" > "$legacy_ref"
    else
        if [ -f "artifacts/baseline/$mod.txt" ]; then
            cp "artifacts/baseline/$mod.txt" "$legacy_ref"
        else
            bad "$mod: neither git HEAD nor artifacts/baseline/$mod.txt available"; continue
        fi
    fi

    # default shared-off run, and shared-off with arbitrary seed/threads:
    # legacy output must not depend on the context at all
    "./$mod/$mod" --shared=off > "$TMP/off1"
    "./$mod/$mod" --shared=off --seed=999 --threads=32 > "$TMP/off2"
    if cmp -s "$TMP/off1" "$legacy_ref" && cmp -s "$TMP/off2" "$legacy_ref"; then
        ok "$mod: --shared=off byte-identical to original (incl. with seed/threads set)"
        printf '          sha256=%s\n' "$(sha256sum "$TMP/off1" | cut -d' ' -f1)"
    else
        bad "$mod: --shared=off diverges from original"
        diff -u "$legacy_ref" "$TMP/off1" | sed 's/^/        /' | head -20
    fi
done

# --------------------------------------------- D: parameter validation
echo
echo "== D: invalid context parameters rejected, boundaries accepted =="
for mod in $MODULES; do
    bin="./$mod/$mod"
    # (argument, expected-result): bad args must exit !=0 and print error:
    for bad_arg in "--threads=0" "--threads=257" "--threads=-1" \
                   "--threads=abc" "--threads" "--seed=-1" "--seed=abc" \
                   "--seed=18446744073709551616" "--shared=maybe" "--bogus"; do
        "$bin" $bad_arg >"$TMP/o" 2>"$TMP/e"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            bad "$mod: '$bad_arg' should have been rejected"
        elif grep -q "^error:" "$TMP/e"; then
            ok "$mod: '$bad_arg' rejected (exit $rc, error message on stderr)"
        else
            bad "$mod: '$bad_arg' failed without an error: message"
            sed 's/^/        /' "$TMP/e"
        fi
    done
    # boundary-valid values must be accepted and run cleanly
    for good_arg in "--seed=0" "--seed=18446744073709551615" \
                    "--threads=1" "--threads=256" "--shared=on" "--shared=off"; do
        if "$bin" $good_arg >/dev/null 2>"$TMP/e"; then
            ok "$mod: '$good_arg' accepted"
        else
            bad "$mod: '$good_arg' wrongly rejected"
            sed 's/^/        /' "$TMP/e"
        fi
    done
    # help text
    if "$bin" --help 2>/dev/null | grep -q "usage:"; then
        ok "$mod: --help prints usage"
    else
        bad "$mod: --help missing usage text"
    fi
done

# --------------------------------------------- dependency audit
echo
echo "== dependency audit: system libc/libm/pthread only =="
allowed='linux-vdso|libc\.so|libm\.so|libpthread\.so|ld-linux|ld-musl'
for mod in $MODULES; do
    foreign="$(ldd "./$mod/$mod" | awk '{print $1}' | grep -Ev "$allowed" || true)"
    if [ -z "$foreign" ]; then
        ok "$mod: no third-party shared libraries"
        ldd "./$mod/$mod" | sed 's/^/          /'
    else
        bad "$mod: unexpected dependencies: $foreign"
    fi
done
if grep -RInE '#include\s+[<"]([^>"]*curl|sqlite|zlib|openssl)' --include='*.c' --include='*.h' . >/dev/null 2>&1; then
    bad "third-party headers included"
else
    ok "no third-party #include in sources"
fi

# ---------------------------------------------------------------- summary
echo
if [ "$FAIL" -eq 0 ]; then
    echo "RESULT: PASS — $PASS checks, 0 failures"
    exit 0
else
    echo "RESULT: FAIL — $PASS passed, $FAIL failed"
    exit 1
fi
